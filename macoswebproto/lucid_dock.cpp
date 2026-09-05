#include <gtk/gtk.h>

#if defined(HAVE_GTK4_LAYER_SHELL)
#if !__has_include(<gtk4-layer-shell.h>)
#error "HAVE_GTK4_LAYER_SHELL is defined but <gtk4-layer-shell.h> is missing. Install gtk4-layer-shell (NOT libgtk-layer-shell-dev, which is the GTK3 build)."
#endif
#include <gtk4-layer-shell.h>
#endif

#include <gio/gdesktopappinfo.h>

#include "dock_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace lucid;

constexpr double BASE_WIDTH = 57.6;
constexpr double DEFAULT_MAX_SCALE = 2.0;

// Bounds the surface has to be able to contain. Configuration is clamped to
// these so that the surface itself can be a constant -- see SURFACE_HEIGHT.
constexpr int MIN_ICON_SIZE = 24;
constexpr int MAX_ICON_SIZE = 80;
constexpr double MIN_MAX_SCALE = 1.0;
constexpr double MAX_MAX_SCALE = 3.0;

constexpr int PANEL_PADDING_X = 10;
constexpr int PANEL_PADDING_Y = 8;
constexpr int ITEM_GAP = 10;
// The row of item slots is tall enough for a fully magnified icon. It is a
// layout container only -- it draws nothing.
constexpr int ITEM_SLOT_HEIGHT = 128;
constexpr int DOT_SIZE = 4;
// Distance from the bottom of an item slot up to the bottom of its icon. Icons
// are bottom-anchored at this line and grow upward from it, so the line is the
// one thing in the vertical layout that never moves.
constexpr int ICON_BOTTOM_INSET = 13;
constexpr int BOTTOM_MARGIN = 8;

constexpr int icon_px_for(double icon_size) { return static_cast<int>(icon_size + 0.5); }
constexpr int panel_bg_height_for(int icon_px) {
    return icon_px + DOT_SIZE + 1 + PANEL_PADDING_Y * 2;
}

// A separator is a hairline with generous margins. The margins are the point:
// the line is what you see, the slot is what you can hit, and right-clicking
// the dock needs something hittable that is not an icon. Icons magnify and
// shove the 10 px inter-item gaps around under the pointer; this slot does not
// magnify, so it stays the width it looks.
constexpr int DIVIDER_WIDTH = 1;
constexpr int DIVIDER_MARGIN = 8;
constexpr int DIVIDER_SLOT = DIVIDER_WIDTH + DIVIDER_MARGIN * 2;

// The visible panel is sized for an UNMAGNIFIED icon, not a magnified one.
// Sizing it for the magnified case is why the idle dock was 144 px tall and
// looked like a slab: it was reserving room it only needs while you are
// pointing at it. macOS does not reserve that room -- magnified icons simply
// overflow above the panel, and the reference does the same thing by giving
// .dock-el a fixed height and letting the <img> grow past it. The panel is
// therefore a separate widget from the row of items, so the items can be drawn
// outside it.
// The name label above the hovered icon. The reference has no transition on it
// at all -- display: none to display: block -- so it appears on the same frame
// the pointer arrives. GTK's stock tooltip cannot do that: it waits ~500 ms by
// design, which reads as a different interaction entirely.
constexpr int TOOLTIP_GAP = 10;         // icon top to label bottom
constexpr int TOOLTIP_RESERVE = 36;     // headroom to reserve for it

// Vertical headroom above a fully magnified icon: enough for the name label,
// and for the launch bounce, which lifts an icon by BOUNCE_HEIGHT and clips
// against the top of the surface without room to move into.
constexpr int TOP_SLACK = TOOLTIP_RESERVE + TOOLTIP_GAP + 8;

// The surface is a CONSTANT size, deliberately, sized for the largest
// configuration that will be accepted rather than for the current one.
//
// Sizing it to the current configuration is what made the dock walk down the
// screen every time magnification was increased: growing the surface is only
// free under layer-shell, where it is anchored to the bottom edge and grows
// upward. On the fallback path the compositor owns placement, keeps the
// window's top where it is, and the extra height comes out of the bottom -- so
// each increase pushed the dock further down. A constant surface cannot do
// that, on either path, and it costs only transparent pixels.
constexpr int SURFACE_HEIGHT = panel_bg_height_for(MAX_ICON_SIZE) +
                               static_cast<int>(MAX_ICON_SIZE * MAX_MAX_SCALE) -
                               MAX_ICON_SIZE + BOTTOM_MARGIN + TOP_SLACK;




// Magnification easing is a damped spring, not exponential decay, and it is the
// same spring in both directions. Exponential decay was the wrong model: it is
// front-loaded and then crawls, so releasing spent 300 ms covering the last 10%
// of the distance and read as sluggish no matter what time constant was used.
//
// These constants reproduce macos-web's `spring({damping: 0.47, stiffness:
// 0.12})`, which is where this dock's magnification curve came from. Svelte's
// spring is a frame-normalised discrete integrator; solving its characteristic
// polynomial z^2 - 1.41z + 0.53 at 60 Hz gives poles of magnitude 0.728 at
// 0.2522 rad/frame, i.e. these continuous parameters. Reproducing them exactly
// rather than by eye means the feel does not drift from the reference.
//
// Release, 2x -> 1x: 50% in 67 ms, 90% in 117 ms, settled at 150 ms, with a 2%
// undershoot that reads as the icon arriving rather than asymptotically giving
// up. The previous RELEASE_TAU of 135 ms took 622 ms to get equally close.
// Deliberately above the reference's 24.32: the same spring shape, run ~23%
// quicker, because the reference tracks the pointer a touch lazily for a dock
// you drive with a mouse rather than a trackpad. Release settles in 122 ms
// rather than 150. Raise it further for snappier, lower for softer; the feel
// stays the same because zeta is what sets the shape.
constexpr double SPRING_OMEGA = 30.0;   // undamped natural frequency, rad/s
constexpr double SPRING_ZETA = 0.783;   // damping ratio, < 1 so it settles by
                                        // arriving rather than by creeping
constexpr double ENVELOPE_SETTLE = 0.001;
constexpr double ENVELOPE_SETTLE_VELOCITY = 0.02;

constexpr double BOUNCE_HEIGHT = 40.0;
constexpr double BOUNCE_DURATION = 0.4;

// A launch keeps bouncing until the application actually appears, so the
// animation answers "did my click work?" rather than just decorating it. It
// gives up after LAUNCH_TIMEOUT so an app that never starts -- or one whose
// process name we cannot match -- stops bouncing rather than bouncing forever.
//
// Five seconds, not twenty. Past about five the bounce has stopped saying
// "starting" and started saying "something is wrong", and twenty seconds of it
// is an irritation rather than feedback. Anything slower than that is better
// served by the icon going quiet and the running dot appearing late.
constexpr double LAUNCH_TIMEOUT = 5.0;
constexpr unsigned LAUNCH_POLL_MS = 400;

// A frame at 60 Hz is 16.7 ms, so anything approaching a millisecond off the
// frame clock is worth naming.
constexpr gint64 SLOW_WORK_US = 1000;

// How far the pointer must travel before a press becomes a drag rather than a
// click. Below this the button still gets its click.
constexpr double REORDER_THRESHOLD = 8.0;

struct DesktopApp {
    std::string app_id;
    std::string title;
    std::filesystem::path desktop_path;
    std::string icon_name;
    std::string exec_line;
    std::vector<std::string> process_candidates;
    bool is_open = false;
    bool divider_before = false;
    // False for an application that is on the dock only because it is running.
    // It leaves when the application quits, and is not written to Pinned.
    bool pinned = true;
};

struct IconRuntime {
    GtkWidget* button = nullptr;
    GtkWidget* fixed = nullptr;
    GtkWidget* image = nullptr;
    GtkWidget* dot = nullptr;

    std::string app_id;
    std::string title;
    std::filesystem::path desktop_path;
    std::string icon_name;
    std::vector<std::string> process_candidates;

    double current_width = BASE_WIDTH;   // animated
    double width_velocity = 0.0;         // px/s, carried across frames by the spring

    // Horizontal displacement from this icon's laid-out slot, sprung to zero.
    // A reorder sets it to "where I used to be minus where I now am", so the
    // icon starts drawn in its old place and slides to the new one. The slot
    // itself changes instantly; only the drawing lags, which is what makes the
    // swap read as motion rather than a jump.
    double x_offset = 0.0;
    double x_offset_velocity = 0.0;
    double target_width = BASE_WIDTH;    // where the pointer says it should be
    double bounce_offset = 0.0;
    std::optional<double> bounce_started_at;

    // Set when we launch this app, cleared when its process shows up. While it
    // is set the bounce restarts each time it finishes.
    std::optional<double> launch_pending_since;

    // The full desktop file id ("firefox.desktop"). app_id above is its stem,
    // which is what process matching wants; this is what the config file's
    // Pinned list is written in, so reordering needs it.
    std::string desktop_id;

    double last_image_y = -1.0;
    int last_dot_x = -1;
    int last_dot_y = -1;
    double panel_x = -1.0;   // laid-out slot
    double drawn_x = -1.0;   // where the widget actually is
    bool is_open = false;

    bool pinned = true;

    // A separator drawn immediately before this item. Owned here rather than in
    // a parallel list so it cannot fall out of step with the icon order.
    bool divider_before = false;
    GtkWidget* divider = nullptr;
    double divider_x = -1.0;

    // Resolved once at build time so the icon can be re-rasterised at a new
    // scale factor without redoing theme lookup. Exactly one is set.
    std::filesystem::path resolved_file;
    std::string resolved_icon_name;
};

std::string to_lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}





std::vector<std::string> exec_candidates(const std::string& exec_line, const std::string& desktop_id) {
    std::vector<std::string> candidates;
    candidates.push_back(std::filesystem::path(desktop_id).stem().string());

    if (exec_line.empty()) {
        return candidates;
    }

    gint argc = 0;
    gchar** argv = nullptr;
    GError* error = nullptr;
    if (g_shell_parse_argv(exec_line.c_str(), &argc, &argv, &error)) {
        if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
            const std::filesystem::path binary(argv[0]);
            candidates.push_back(binary.filename().string());
            candidates.push_back(binary.stem().string());
        }
        g_strfreev(argv);
    } else if (error != nullptr) {
        g_error_free(error);
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::unordered_set<std::string> collect_running_process_names();

// Icon lookup is GtkIconTheme's job. This used to walk
// /usr/share/icons/Lucid-Light by hand across a fixed list of size
// directories, which meant every icon fell back to a generic one on any
// machine without that theme installed -- and skipped the inheritance and
// fallback chain that the icon theme specification defines and GTK already
// implements. The theme to prefer is now a setting; empty follows the desktop.

// What to pin when there is no config file and no GNOME to borrow from. One
// entry per role, first match wins, anything not installed is skipped -- so
// this produces a short, sane dock on KDE or sway rather than an empty one.
using CandidateCache = std::map<std::string, std::vector<std::string>>;

std::vector<DesktopApp> load_dock_apps(const DockConfig& config,
                                       const std::map<std::string, DesktopCatalogEntry>& catalog,
                                       const CandidateCache* cache = nullptr,
                                       const std::unordered_set<std::string>* running = nullptr) {
    // exec_candidates() is a pure function of the entry, so on the hot path it
    // comes from the cache the engine keeps beside the catalog.
    // Returns a reference on the cached path: this runs once per catalog entry
    // per tick, and copying a vector of strings 65 times is not free.
    std::vector<std::string> scratch;
    auto candidates_for = [&](const std::string& desktop_id,
                              const DesktopCatalogEntry& entry) -> const std::vector<std::string>& {
        if (cache != nullptr) {
            const auto it = cache->find(desktop_id);
            if (it != cache->end()) {
                return it->second;
            }
        }
        scratch = exec_candidates(entry.exec_line, desktop_id);
        return scratch;
    };
    const auto& favorites = config.pinned;

    // Scanning /proc costs ~11 ms. The caller usually has just done it, so it
    // is passed in rather than repeated -- refresh_running_indicators() scanned
    // /proc and then called this, which scanned it again, twice per tick.
    std::unordered_set<std::string> scanned;
    if (running == nullptr) {
        scanned = collect_running_process_names();
    }
    const std::unordered_set<std::string>& running_names = running != nullptr ? *running : scanned;

    std::vector<DesktopApp> apps;
    apps.reserve(favorites.size());

    for (const auto& desktop_id : favorites) {
        const auto it = catalog.find(desktop_id);
        if (it == catalog.end()) {
            continue;
        }

        const DesktopCatalogEntry& entry = it->second;
        const std::string app_id = std::filesystem::path(desktop_id).stem().string();
        const auto& candidates = candidates_for(desktop_id, entry);

        const bool is_open = std::any_of(candidates.begin(), candidates.end(), [&](const std::string& candidate) {
            return running_names.find(candidate) != running_names.end();
        });

        const bool divider_before =
            std::find(config.dividers_before.begin(), config.dividers_before.end(), desktop_id) !=
            config.dividers_before.end();

        apps.push_back(DesktopApp{
            .app_id = app_id,
            .title = entry.title,
            .desktop_path = entry.desktop_path,
            .icon_name = entry.icon_name,
            .exec_line = entry.exec_line,
            .process_candidates = candidates,
            .is_open = is_open,
            .divider_before = divider_before,
            .pinned = true,
        });
    }

    if (!config.show_running) {
        return apps;
    }

    // Applications that are running but not pinned, appended behind a
    // separator. Matching is by process name derived from the Exec line, which
    // is a heuristic but a portable one: it needs nothing from the compositor
    // and works identically on GNOME, KDE, sway and Hyprland.
    //
    // The better answer once LucidOS is on its own compositor is
    // ext-foreign-toplevel-list-v1, which enumerates actual windows rather than
    // guessing from processes. It reports what has a window instead of what has
    // a process, so it does not miss Flatpak apps whose binary name differs
    // from the desktop id, and does not show background daemons. Mutter does
    // not implement it, but neither does it implement layer-shell, so on every
    // compositor this dock can anchor to, the protocol is available.
    std::vector<std::string> pinned_ids(favorites.begin(), favorites.end());
    std::unordered_set<std::string> seen_processes;
    bool first_unpinned = true;

    // A pinned icon already represents its process, so an unpinned entry that
    // resolves to the same binary must not appear a second time.
    for (const auto& app : apps) {
        for (const auto& candidate : app.process_candidates) {
            if (running_names.find(candidate) != running_names.end()) {
                seen_processes.insert(candidate);
            }
        }
    }

    for (const auto& [desktop_id, entry] : catalog) {
        if (std::find(pinned_ids.begin(), pinned_ids.end(), desktop_id) != pinned_ids.end()) {
            continue;
        }

        const auto& candidates = candidates_for(desktop_id, entry);
        if (candidates.empty()) {
            continue;
        }
        const auto match = std::find_if(
            candidates.begin(), candidates.end(), [&](const std::string& candidate) {
                return running_names.find(candidate) != running_names.end();
            });
        if (match == candidates.end()) {
            continue;
        }

        // One process, one icon. Several desktop files can launch the same
        // binary -- a KDE-specific variant beside the plain one, say -- and
        // matching by process name finds all of them, so the dock would show
        // System Monitor twice for a single running copy.
        if (!seen_processes.insert(*match).second) {
            continue;
        }

        apps.push_back(DesktopApp{
            .app_id = std::filesystem::path(desktop_id).stem().string(),
            .title = entry.title,
            .desktop_path = entry.desktop_path,
            .icon_name = entry.icon_name,
            .exec_line = entry.exec_line,
            .process_candidates = candidates,
            .is_open = true,
            .divider_before = first_unpinned,
            .pinned = false,
        });
        first_unpinned = false;
    }

    return apps;
}

class MagnificationProfile {
  public:
    MagnificationProfile() { configure(BASE_WIDTH, DEFAULT_MAX_SCALE, 6.0); }

    // The reference's width curve is BASE * {1, 1.1, 1.414, 2} across the
    // half-width of the falloff. Expressed as a fraction of the peak excursion
    // that is {0, 0.1, 0.414, 1}, which generalises to any peak scale and
    // reproduces the reference exactly at 2.0.
    void configure(double base_width, double max_scale, double spread) {
        static const double kShape[4] = {0.0, 0.1, 0.414, 1.0};
        base_width_ = base_width;
        const double excursion = base_width * (max_scale - 1.0);
        // The falloff is measured in icon widths, so it scales with the icon
        // size rather than staying a fixed number of pixels -- a bigger dock
        // should magnify over a proportionally bigger span, not a stubbier one.
        // The spread itself is what decides how much the hovered icon stands
        // out from its neighbours.
        const double limit = base_width * spread;

        distance_input_ = {
            -limit, -limit / 1.25, -limit / 2.0, 0.0, limit / 2.0, limit / 1.25, limit,
        };
        width_output_ = {
            base_width + kShape[0] * excursion, base_width + kShape[1] * excursion,
            base_width + kShape[2] * excursion, base_width + kShape[3] * excursion,
            base_width + kShape[2] * excursion, base_width + kShape[1] * excursion,
            base_width + kShape[0] * excursion,
        };
    }

    double interpolate_width(double distance) const {
        if (distance <= distance_input_.front() || distance >= distance_input_.back()) {
            return base_width_;
        }

        for (std::size_t i = 0; i + 1 < distance_input_.size(); ++i) {
            const double left = distance_input_[i];
            const double right = distance_input_[i + 1];
            if (left <= distance && distance <= right) {
                const double start = width_output_[i];
                const double end = width_output_[i + 1];
                const double span = right - left;
                if (span == 0.0) {
                    return end;
                }
                const double mix = (distance - left) / span;
                return start + (end - start) * mix;
            }
        }

        return base_width_;
    }

  private:
    double base_width_ = BASE_WIDTH;
    std::vector<double> distance_input_;
    std::vector<double> width_output_;
};

static double sine_in_out(double progress) {
    return -(std::cos(M_PI * progress) - 1.0) / 2.0;
}

std::unordered_set<std::string> collect_running_process_names() {
    std::unordered_set<std::string> names;

    const std::filesystem::path proc_root("/proc");
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(proc_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }

        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue;
        }

        const std::filesystem::path comm_path = entry.path() / "comm";
        const std::filesystem::path cmdline_path = entry.path() / "cmdline";

        {
            std::ifstream comm(comm_path);
            std::string name;
            if (std::getline(comm, name) && !name.empty()) {
                names.insert(name);
            }
        }

        {
            std::ifstream cmdline(cmdline_path, std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
            if (!raw.empty()) {
                const std::size_t nul = raw.find('\0');
                std::string first = raw.substr(0, nul);
                if (!first.empty()) {
                    names.insert(std::filesystem::path(first).filename().string());
                }
            }
        }
    }

    return names;
}

class DockEngine {
  public:
    DockEngine() = default;

    // Pin the toplevel to the full monitor width. Both dimensions have to be
    // set together and every time: passing -1 for the width to "only change the
    // height" clears the pin, the window auto-sizes down to the panel, and
    // centring against that width parks the dock at the left edge. That is
    // exactly what a config reload used to do.
    //
    // Fallback path only -- under layer-shell the compositor owns the size, and
    // the pin exists because without it the toplevel renegotiates its surface
    // size with the compositor on every magnification frame, which halves the
    // frame rate.
    // Everything derived from the config in one place, so a reload cannot
    // update half of it.
    void apply_config_geometry() {
        base_width_ = config_.icon_size > 0
                          ? std::clamp(static_cast<double>(config_.icon_size),
                                       static_cast<double>(MIN_ICON_SIZE),
                                       static_cast<double>(MAX_ICON_SIZE))
                          : BASE_WIDTH;
        magnifier_.configure(base_width_, std::clamp(config_.max_scale, MIN_MAX_SCALE, MAX_MAX_SCALE),
                             config_.spread);
        last_applied_pixel_widths_.clear();
        last_applied_panel_width_ = -1;
        last_icon_scale_ = 0;
    }

    void apply_window_size() {
        if (window_ == nullptr || layer_shell_active_) {
            return;
        }

        int pinned_width = 1920;
        if (GdkDisplay* display = gdk_display_get_default()) {
            if (GListModel* monitors = gdk_display_get_monitors(display)) {
                if (auto* monitor = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, 0))) {
                    GdkRectangle geometry;
                    gdk_monitor_get_geometry(monitor, &geometry);
                    if (geometry.width > 0) {
                        pinned_width = geometry.width;
                    }
                    g_object_unref(monitor);
                }
            }
        }

        gtk_widget_set_size_request(window_, pinned_width, window_height());
    }

        void build_ui(GtkApplication* app, const DockConfig& config,
                      const std::vector<DesktopApp>& dock_apps) {
        config_ = config;
        apply_config_geometry();
        window_ = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(window_), "Lucid Dock C++");
        gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    #if defined(HAVE_GTK4_LAYER_SHELL)
        // Compile-time availability is not run-time availability: Mutter does not
        // implement wlr-layer-shell, so a binary built with layer-shell still has
        // to run without it on GNOME. Ask before initialising rather than letting
        // gtk4-layer-shell warn once per property set.
        layer_shell_active_ = gtk_layer_is_supported();
    #endif

        // Fallback path only. Pin the toplevel to the full monitor width: without
        // it the window auto-sizes to the dock panel, and because the panel resizes
        // on every magnification frame the toplevel renegotiates its surface size
        // with the compositor every frame -- a round-trip per frame, which halves
        // the frame rate. Under layer-shell the compositor owns the size and this
        // cannot happen, so the pin is skipped there.
        apply_window_size();
        // Width is not ours to choose: anchored left+right, the compositor gives
        // us the output width. -1 means "no opinion" rather than a guess that is
        // wrong on every display that is not 1280 logical pixels wide.
        gtk_window_set_default_size(GTK_WINDOW(window_), -1, window_height());
        g_signal_connect(window_, "notify::scale-factor",
                         G_CALLBACK(&DockEngine::on_scale_changed_static), this);
        g_signal_connect(window_, "realize",
                         G_CALLBACK(&DockEngine::on_window_realize_static), this);

    #if defined(HAVE_GTK4_LAYER_SHELL)
        if (!layer_shell_active_) {
            g_warning("This compositor does not implement wlr-layer-shell (Mutter "
                      "does not, and has declined to). The dock falls back to an "
                      "unanchored toplevel: it will not reserve space and its "
                      "position is a guess. Use a layer-shell compositor to test "
                      "the real path.");
        } else {
        gtk_layer_init_for_window(GTK_WINDOW(window_));
        gtk_layer_set_layer(GTK_WINDOW(window_), GTK_LAYER_SHELL_LAYER_OVERLAY);
        // Anchored to both sides as well as the docked edge, so the surface
        // spans the output and the panel is centred inside it. Anchoring only
        // the one edge would leave the surface its natural width and hand
        // horizontal placement to the compositor.
        const auto edge = at_top() ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM;
        gtk_layer_set_anchor(GTK_WINDOW(window_), edge, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(window_), edge, 0);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window_), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        }
    #else
        g_warning("lucid-dock was built WITHOUT gtk4-layer-shell. The dock cannot "
                  "anchor to the screen edge and will float in the wrong place. "
                  "Rebuild with gtk4-layer-shell.");
    #endif

        apply_icon_theme();
        install_css();

        root_fixed_ = gtk_fixed_new();
        gtk_widget_add_css_class(root_fixed_, "dock-root");
        gtk_window_set_child(GTK_WINDOW(window_), root_fixed_);

        // Two widgets, not one: the rounded panel that is drawn, and the row of
        // item slots that is not. Added in this order so the items sit above
        // the panel and can overflow past its top edge when magnified.
        panel_bg_ = gtk_fixed_new();
        gtk_widget_add_css_class(panel_bg_, "dock-panel");
        gtk_fixed_put(GTK_FIXED(root_fixed_), panel_bg_, 0.0, 0.0);

        panel_fixed_ = gtk_fixed_new();
        gtk_fixed_put(GTK_FIXED(root_fixed_), panel_fixed_, 0.0, 0.0);

        // Added last so it draws over the items, and parented to root_fixed_
        // rather than the panel so it can sit above the panel's top edge.
        tooltip_ = gtk_label_new("");
        gtk_widget_add_css_class(tooltip_, "dock-tooltip");
        gtk_widget_set_visible(tooltip_, FALSE);
        gtk_widget_set_can_target(tooltip_, FALSE);
        gtk_fixed_put(GTK_FIXED(root_fixed_), tooltip_, 0.0, 0.0);

        build_icons(dock_apps);

        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion", G_CALLBACK(&DockEngine::on_motion_static), this);
        g_signal_connect(motion, "leave", G_CALLBACK(&DockEngine::on_leave_static), this);
        gtk_widget_add_controller(root_fixed_, motion);

        // Secondary click in the CAPTURE phase, so it is seen before the item
        // buttons get a chance at it. Right-click therefore works anywhere on
        // the dock, icons included -- which it has to, because the only parts
        // that are not an icon are 10 px strips that the icons shove around as
        // they magnify. When icons grow their own context menu they can claim
        // this in the bubble phase and the dock menu keeps the background and
        // the separators.
        // Drag to reorder. CAPTURE phase so it sees the press before the
        // button does, but it does not claim the sequence until the pointer
        // has actually travelled REORDER_THRESHOLD -- below that the press
        // stays a click and still launches the app. Claiming immediately would
        // mean the dock could be rearranged but never clicked.
        GtkGesture* reorder = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(reorder), GDK_BUTTON_PRIMARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(reorder),
                                                   GTK_PHASE_CAPTURE);
        g_signal_connect(reorder, "drag-begin",
                         G_CALLBACK(&DockEngine::on_reorder_begin_static), this);
        g_signal_connect(reorder, "drag-update",
                         G_CALLBACK(&DockEngine::on_reorder_update_static), this);
        g_signal_connect(reorder, "drag-end",
                         G_CALLBACK(&DockEngine::on_reorder_end_static), this);
        gtk_widget_add_controller(root_fixed_, GTK_EVENT_CONTROLLER(reorder));

        GtkGesture* secondary = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(secondary), GTK_PHASE_CAPTURE);
        g_signal_connect(secondary, "pressed", G_CALLBACK(&DockEngine::on_secondary_press_static), this);
        gtk_widget_add_controller(root_fixed_, GTK_EVENT_CONTROLLER(secondary));

        install_menu();
        watch_config();

        g_signal_connect(window_, "map", G_CALLBACK(&DockEngine::on_map_static), this);

        running_refresh_id_ = g_timeout_add_seconds(4, &DockEngine::on_running_refresh_static, this);
        queue_layout();

        if (const char* bench = g_getenv("LUCID_DOCK_BENCH")) {
            bench_total_ = std::max(60, atoi(bench));
            bench_remaining_ = bench_total_;
            ensure_tick();
        }

        gtk_window_present(GTK_WINDOW(window_));
    }

  private:
    static void on_map_static(GtkWidget*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->queue_layout();
    }

    // Is the pointer actually on the dock panel? Coordinates are relative to
    // root_fixed_, which spans the whole monitor -- so "the pointer is in this
    // widget" is nowhere near the same question as "the pointer is on the dock".
    //
    // Getting this wrong is what made magnification feel dead. Motion anywhere
    // in the bottom strip of the screen used to set the pointer position, and
    // the curve reaches DISTANCE_LIMIT (6 icon widths) past the icon it is
    // measured from, so the dock started magnifying while the cursor was still
    // 300 px away from it. By the time the cursor arrived at the panel edge the
    // nearest icon was already at 78% of full magnification, leaving only the
    // last 22% to happen during the traversal you can actually see.
    //
    // macos-web has no such region: `.dock-container` is pointer-events: none
    // and only `.dock-el`, the panel itself, is pointer-events: auto, so
    // mouseleave fires at the panel edge. This is that, in a hit test.
    // Everything that depends on the configured peak magnification. The
    // surface has to be tall enough to draw a magnified icon and its bounce,
    // so raising MaxScale grows the window rather than clipping the icons.
    // Idle icon size comes from the config; everything else is derived from it.
    double base_width() const { return base_width_; }
    int base_icon_px() const { return icon_px_for(base_width_); }
    int panel_bg_height() const { return panel_bg_height_for(base_icon_px()); }
    double max_icon_width() const { return base_width_ * config_.max_scale; }
    int max_icon_px() const { return static_cast<int>(max_icon_width() + 0.5); }
    int icon_source_size() const { return max_icon_px() + 1; }
    int window_height() const {
        // Constant. See SURFACE_HEIGHT: sizing this to the configuration is
        // what walked the dock down the screen on every magnification change.
        return SURFACE_HEIGHT;
    }

    // Which item is under this x, in root_fixed_ coordinates. Uses the animated
    // width, so the label tracks the icon it belongs to as that icon grows.
    // A separator before the first icon would be a line hanging off the left
    // end of the dock, so there is never one there. This has to be the single
    // answer used by the width sum, the placement, and the widget's visibility:
    // when it was written out three times, the widget was created for icon 0
    // and then skipped by the layout, leaving it parked at the panel origin on
    // top of the first icon.
    bool draws_divider(std::size_t i) const {
        return i > 0 && icons_[i].divider_before && icons_[i].divider != nullptr;
    }

    // Everything about the edge lives behind these. "top" and "bottom" are a
    // horizontal bar reflected vertically, which is a handful of sign flips.
    // "left" and "right" are not: they need the item strip laid out down the
    // screen instead of across it, and that is a different layout, not a flag.
    // Rejected at load rather than half-implemented.
    bool at_top() const { return config_.position == "top"; }

    // Distance from the icon's slot to the icon itself, on the side away from
    // the screen edge. Icons grow away from the edge they are docked to.
    double icon_inset_y(double pixel_width) const {
        return at_top() ? static_cast<double>(ICON_BOTTOM_INSET)
                        : static_cast<double>(ITEM_SLOT_HEIGHT) - pixel_width - ICON_BOTTOM_INSET;
    }

    // The bounce moves away from the screen edge, so it flips with the edge.
    double bounce_direction() const { return at_top() ? 1.0 : -1.0; }

    std::optional<std::size_t> icon_at(double x) const {
        for (std::size_t i = 0; i < icons_.size(); ++i) {
            const double left = panel_x_ + icons_[i].panel_x;
            if (x >= left && x <= left + icons_[i].current_width) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Centred over the hovered icon and sitting above it, repositioned every
    // frame so it rides the magnification rather than lagging behind it.
    void update_tooltip(int win_w) {
        if (tooltip_ == nullptr) {
            return;
        }

        if (!hovered_index_.has_value() || *hovered_index_ >= icons_.size()) {
            gtk_widget_set_visible(tooltip_, FALSE);
            return;
        }

        const IconRuntime& icon = icons_[*hovered_index_];
        if (icon.title != tooltip_text_) {
            tooltip_text_ = icon.title;
            gtk_label_set_text(GTK_LABEL(tooltip_), tooltip_text_.c_str());
            tooltip_w_ = -1;   // measurement is stale
        }
        gtk_widget_set_visible(tooltip_, TRUE);

        // Measuring a GtkLabel runs a Pango layout, which is expensive enough
        // to show up as frame-time spikes: doing it every frame took p95 from
        // 0.04 ms to 0.27 ms with a 4.9 ms worst case. The size only depends on
        // the text, and the text only changes when the pointer moves to a
        // different icon -- what changes every frame is the position, which is
        // arithmetic. So measure on text change and cache it.
        if (tooltip_w_ < 0) {
            gtk_widget_measure(tooltip_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &tooltip_w_,
                               nullptr, nullptr);
            gtk_widget_measure(tooltip_, GTK_ORIENTATION_VERTICAL, tooltip_w_, nullptr,
                               &tooltip_h_, nullptr, nullptr);
        }
        const int natural_w = tooltip_w_;
        const int natural_h = tooltip_h_;

        const double centre = panel_x_ + icon.panel_x + icon.current_width / 2.0;
        double x = centre - natural_w / 2.0;
        // Keep it on screen for the first and last items rather than letting it
        // run off the edge.
        x = std::clamp(x, 0.0, std::max(0.0, static_cast<double>(win_w - natural_w)));

        const double y = at_top()
                             ? icon_baseline_y_ + icon.current_width + TOOLTIP_GAP
                             : icon_baseline_y_ - icon.current_width - TOOLTIP_GAP - natural_h;
        gtk_fixed_move(GTK_FIXED(root_fixed_), tooltip_, x, std::max(0.0, y));
    }

    bool pointer_is_on_panel(double x, double y) const {
        if (panel_content_width_ <= 0) {
            return false;
        }

        const double left = static_cast<double>(panel_x_);
        const double right = left + panel_content_width_ + 2.0 * PANEL_PADDING_X;
        if (x < left || x > right) {
            return false;
        }

        // Vertically the region is whatever the dock currently occupies: the
        // panel, plus however far the icons are sticking out above it right
        // now. Using the panel alone would mean the magnified part of an icon
        // is not part of the dock, so pointing at the big icon would shrink it.
        // The DOM gets this for free -- an overflowing <img> is still a
        // descendant of .dock-el, so mouseleave does not fire over it.
        double tallest = base_icon_px();
        for (const auto& icon : icons_) {
            tallest = std::max(tallest, icon.current_width);
        }

        const double top = at_top() ? static_cast<double>(panel_bg_y_)
                                    : icon_baseline_y_ - tallest - PANEL_PADDING_Y;
        // Downward it runs to the bottom of the window rather than the bottom
        // of the panel: the BOTTOM_MARGIN strip underneath is the screen edge,
        // and a dock you cannot hit by slamming the pointer into the bottom of
        // the screen is a dock you have to aim at.
        const double bottom = std::max(gtk_widget_get_height(window_), window_height());
        return y >= top && y <= bottom;
    }

    static void on_motion_static(GtkEventControllerMotion*, double x, double y, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);

        if (!self->pointer_is_on_panel(x, y)) {
            // Off the panel is off the dock, even though it is still inside the
            // window. Same effect as leaving.
            if (self->current_mouse_x_.has_value() || self->hovered_index_.has_value()) {
                self->current_mouse_x_.reset();
                self->hovered_index_.reset();
                self->queue_layout();
            }
            return;
        }

        // No motion throttle any more. A motion event now only updates a
        // target; the tick callback does the work at frame rate. Throttling
        // here was what made pointer tracking feel steppy.
        self->current_mouse_x_ = x;
        self->last_pointer_x_ = x;
        self->hovered_index_ = self->icon_at(x);
        self->layout_dirty_ = true;
        self->ensure_tick();
    }

    static void on_leave_static(GtkEventControllerMotion*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->current_mouse_x_.reset();
        self->hovered_index_.reset();
        self->queue_layout();
    }

    // The handler is given the engine and finds the icon by desktop id. It used
    // to be handed &icons_.back() directly, which is a pointer into a vector --
    // fine until the vector reorders or reallocates, at which point every
    // button launches the wrong application. Reordering makes that reachable.
    static void on_icon_clicked_static(GtkButton* button, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        const char* id = static_cast<const char*>(
            g_object_get_data(G_OBJECT(button), "lucid-desktop-id"));
        if (id == nullptr) {
            return;
        }
        IconRuntime* icon = self->icon_by_desktop_id(id);
        if (icon == nullptr) {
            return;
        }

        if (!icon->desktop_path.empty()) {
            GDesktopAppInfo* app_info =
                g_desktop_app_info_new_from_filename(icon->desktop_path.c_str());
            if (app_info != nullptr) {
                GError* error = nullptr;
                if (!g_app_info_launch(G_APP_INFO(app_info), nullptr, nullptr, &error) &&
                    error != nullptr) {
                    g_warning("Failed to launch %s: %s", icon->title.c_str(), error->message);
                    g_error_free(error);
                    g_object_unref(app_info);
                    return;
                }
                g_object_unref(app_info);
            }
        }

        // Bounce until the process appears, not for a fixed 0.4 s. The dot
        // stays hidden until it is genuinely running, so the two states mean
        // different things: bouncing is "starting", dot is "running".
        const double now = monotonic_seconds();
        icon->bounce_started_at = now;
        icon->launch_pending_since = now;
        self->ensure_launch_poll();
        self->ensure_tick();   // a click can arrive with the clock stopped
    }

    IconRuntime* icon_by_desktop_id(const std::string& id) {
        for (auto& icon : icons_) {
            if (icon.desktop_id == id) {
                return &icon;
            }
        }
        return nullptr;
    }

    static gboolean on_tick_static(GtkWidget*, GdkFrameClock* clock, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        return self->on_tick(clock);
    }

    static gboolean on_running_refresh_static(gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->refresh_running_indicators();
        return G_SOURCE_CONTINUE;
    }

    void ensure_tick() {
        if (tick_id_ == 0) {
            tick_id_ = gtk_widget_add_tick_callback(window_, &DockEngine::on_tick_static, this, nullptr);
        }
    }

    void queue_layout() {
        layout_dirty_ = true;
        ensure_tick();
    }

    // One underdamped spring, solved in closed form, shared by the magnification
    // envelope and the reorder slide so the dock only ever has one feel. Returns
    // true while still moving.
    static bool spring_step(double& value, double& velocity, double target, double dt,
                            double settle, double settle_velocity) {
        const double a = value - target;
        if (std::abs(a) <= settle && std::abs(velocity) <= settle_velocity) {
            value = target;
            velocity = 0.0;
            return false;
        }
        const double sigma = SPRING_ZETA * SPRING_OMEGA;
        const double omega_d = SPRING_OMEGA * std::sqrt(1.0 - SPRING_ZETA * SPRING_ZETA);
        const double decay = std::exp(-sigma * dt);
        const double cos_wd = std::cos(omega_d * dt);
        const double sin_wd = std::sin(omega_d * dt);
        const double b = (velocity + sigma * a) / omega_d;
        value = target + decay * (a * cos_wd + b * sin_wd);
        velocity = decay * ((omega_d * b - sigma * a) * cos_wd -
                            (omega_d * a + sigma * b) * sin_wd);
        return true;
    }

    // Slide every icon back toward its slot.
    bool step_offsets(double dt) {
        bool moving = false;
        for (auto& icon : icons_) {
            if (spring_step(icon.x_offset, icon.x_offset_velocity, 0.0, dt, 0.05, 0.5)) {
                moving = true;
            }
        }
        return moving;
    }

    // Advance the width spring by dt, using the closed-form solution of the
    // underdamped spring rather than a stepped integrator. That matters here:
    // a stepped integrator's behaviour depends on how often it is called, and
    // this dock demonstrably runs anywhere between 20 and 60 fps depending on
    // the GSK renderer, so a stepped spring would change feel with the renderer.
    // The closed form is exact at any dt and cannot go unstable at a low one.
    bool step_widths(double dt) {
        const auto shape = compute_item_widths();
        if (shape.size() != icons_.size()) {
            return false;
        }

        // Spring the ENVELOPE, not the widths.
        //
        // Springing each icon's width made the dock a low-pass filter on
        // pointer position: sweep quickly and every icon holds a large target
        // for only a few frames, so none of them get large and they converge on
        // looking identical. Tuning the spring to fix that is the wrong knob,
        // because the same spring is what shapes the shrink -- speeding up the
        // tracking necessarily speeds up the release, and those are not the
        // same thing.
        //
        // macOS does not filter the shape at all. Icon size is a direct
        // function of pointer position, so a fast sweep magnifies exactly as
        // much as a slow one. What animates is entering and leaving the dock.
        // So the shape is applied instantly and a single 0..1 envelope is
        // sprung: 1 while the pointer is on the dock, 0 when it is not. The
        // release animation and the magnification response are now independent,
        // and the fast-sweep attenuation is not a tuning problem, it is gone.
        // The target comes from where the pointer IS -- current_mouse_x_ -- and
        // never from last_pointer_x_, which is only retained to shape the decay
        // after the pointer has gone. Deriving the target from the remembered
        // position instead makes the two circular: the target stays 1 because
        // the position is set, and the position is only cleared once the target
        // reaches 0. The dock then magnifies on first hover and never shrinks.
        // Dragging collapses the dock to rest size. Magnifying under the
        // pointer while an icon is being carried makes every slot a moving
        // target and the drop position impossible to judge -- macOS shrinks for
        // the same reason. The envelope already springs, so this shrinks and
        // grows back smoothly with no extra animation code.
        const double target =
            (current_mouse_x_.has_value() && config_.magnification && !dragging_) ? 1.0 : 0.0;

        const double sigma = SPRING_ZETA * SPRING_OMEGA;
        const double omega_d = SPRING_OMEGA * std::sqrt(1.0 - SPRING_ZETA * SPRING_ZETA);
        const double a = envelope_ - target;
        bool moving = false;

        if (std::abs(a) <= ENVELOPE_SETTLE && std::abs(envelope_velocity_) <= ENVELOPE_SETTLE_VELOCITY) {
            envelope_ = target;
            envelope_velocity_ = 0.0;
            // Retained only to shape the decay. Once the decay is finished it
            // is nothing but stale state, and holding it would keep the dock
            // laying out against a pointer that is no longer there.
            if (target == 0.0) {
                last_pointer_x_.reset();
            }
        } else {
            const double decay = std::exp(-sigma * dt);
            const double cos_wd = std::cos(omega_d * dt);
            const double sin_wd = std::sin(omega_d * dt);
            const double b = (envelope_velocity_ + sigma * a) / omega_d;

            envelope_ = target + decay * (a * cos_wd + b * sin_wd);
            envelope_velocity_ = decay * ((omega_d * b - sigma * a) * cos_wd -
                                          (omega_d * a + sigma * b) * sin_wd);
            moving = true;
        }

        for (std::size_t i = 0; i < icons_.size(); ++i) {
            icons_[i].target_width = shape[i];
            icons_[i].current_width = base_width_ + envelope_ * (shape[i] - base_width_);
        }

        return moving;
    }

    // LUCID_DOCK_BENCH=<frames> sweeps a synthetic pointer across the dock and
    // reports where the frame time actually goes. Measuring beats guessing:
    // "layout" is the time inside layout_panel(), "frame" is the interval the
    // frame clock actually achieved.
    void bench_step(GdkFrameClock* clock) {
        if (bench_remaining_ <= 0) {
            return;
        }

        const double phase = static_cast<double>(bench_total_ - bench_remaining_) /
                             static_cast<double>(bench_total_);

        // The last quarter of the run takes the pointer off the dock, so the
        // release is measured rather than only the sweep. The dock has now
        // twice shipped a bug where it magnified and then would not shrink --
        // once from a 622 ms tail, once from an envelope whose target could
        // never reach zero. Both were invisible to a benchmark that never let
        // go of the pointer.
        if (phase < 0.75) {
            const double sweep = phase / 0.75;
            const double span = static_cast<double>(panel_content_width_ + 2 * PANEL_PADDING_X);
            current_mouse_x_ = panel_x_ + span * (0.5 - 0.5 * std::cos(sweep * 2.0 * M_PI * 3.0));
            // Real motion sets this too, and it is what turns the name label
            // on. A benchmark that left it unset measured a code path nobody
            // hovers through -- 60 fps here while actual hovering ran at 30.
            hovered_index_ = icon_at(*current_mouse_x_);
        } else {
            current_mouse_x_.reset();
            hovered_index_.reset();
        }
        layout_dirty_ = true;

        if (clock != nullptr) {
            const gint64 t = gdk_frame_clock_get_frame_time(clock);
            if (bench_last_frame_us_ > 0) {
                bench_frame_us_.push_back(t - bench_last_frame_us_);
            }
            bench_last_frame_us_ = t;
        }

        if (--bench_remaining_ == 0) {
            bench_report();
        }
    }

    static double pct(std::vector<gint64>& v, double q) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const std::size_t i = std::min(v.size() - 1,
            static_cast<std::size_t>(q * static_cast<double>(v.size() - 1)));
        return static_cast<double>(v[i]) / 1000.0;
    }

    // Anything that runs off a timer rather than off the frame clock is
    // invisible to a frame-time percentile: it fires a handful of times across
    // a run, so it hides in the tail. Timing each one by name says which.
    struct WorkStat {
        gint64 calls = 0;
        gint64 total_us = 0;
        gint64 max_us = 0;
    };

    void record_work(const char* name, gint64 us) {
        WorkStat& stat = work_[name];
        stat.calls += 1;
        stat.total_us += us;
        stat.max_us = std::max(stat.max_us, us);

        // Also complain in real time. A stall only shows up in the benchmark
        // if it happens to fire during the run, and the 4 s timer usually does
        // not.
        if (us >= SLOW_WORK_US && g_getenv("LUCID_DOCK_TIMERS") != nullptr) {
            g_message("slow: %s took %.2f ms", name, static_cast<double>(us) / 1000.0);
        }
    }

    // Scoped timer. Declaring one times the enclosing block.
    class ScopedWork {
      public:
        ScopedWork(DockEngine* engine, const char* name)
            : engine_(engine), name_(name), start_(g_get_monotonic_time()) {}
        ~ScopedWork() { engine_->record_work(name_, g_get_monotonic_time() - start_); }

      private:
        DockEngine* engine_;
        const char* name_;
        gint64 start_;
    };

    void report_work() {
        if (work_.empty()) {
            return;
        }
        g_print("\noff-frame work (timers, not the frame clock)\n");
        g_print("  %-34s %6s %10s %10s\n", "what", "calls", "mean ms", "max ms");
        for (const auto& [name, stat] : work_) {
            g_print("  %-34s %6ld %10.3f %10.3f\n", name.c_str(),
                    static_cast<long>(stat.calls),
                    stat.calls ? (static_cast<double>(stat.total_us) / stat.calls) / 1000.0 : 0.0,
                    static_cast<double>(stat.max_us) / 1000.0);
        }
    }

    void bench_report() {
        g_print("\n--- lucid-dock frame benchmark ---\n");
        g_print("window           : %dx%d logical\n",
                gtk_widget_get_width(window_), gtk_widget_get_height(window_));
        g_print("icons            : %zu\n", icons_.size());
        g_print("scale factor     : %d\n", current_scale());
        g_print("frames measured  : %zu\n", bench_frame_us_.size());
        g_print("layout_panel() ms: p50 %.3f  p95 %.3f  max %.3f\n",
                pct(bench_layout_us_, 0.50), pct(bench_layout_us_, 0.95),
                pct(bench_layout_us_, 1.0));
        g_print("frame interval ms: p50 %.3f  p95 %.3f  max %.3f\n",
                pct(bench_frame_us_, 0.50), pct(bench_frame_us_, 0.95),
                pct(bench_frame_us_, 1.0));
        // Did letting go actually put it back? Reported as a fact rather than
        // trusted, because "it magnifies" and "it un-magnifies" are separate
        // claims and only the first one is obvious while developing.
        double widest = 0.0;
        for (const auto& icon : icons_) {
            widest = std::max(widest, icon.current_width);
        }
        g_print("after release    : envelope %.4f, widest icon %.1f px (idle is %.1f) -- %s\n",
                envelope_, widest, base_width_,
                (envelope_ < 0.01 && widest <= base_width_ + 0.5) ? "SHRANK" : "STUCK MAGNIFIED");

        const double p50 = pct(bench_frame_us_, 0.50);
        if (p50 > 0.0) {
            g_print("effective fps    : %.1f (p50)\n", 1000.0 / p50);
        }
        report_work();
        g_print("---------------------------------\n");
        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window_))));
    }

    gboolean on_tick(GdkFrameClock* clock) {
        bench_step(clock);
        const double now = monotonic_seconds();
        const double dt = last_tick_at_ > 0.0 ? std::min(0.1, now - last_tick_at_) : 1.0 / 60.0;
        last_tick_at_ = now;

        bool animations_running = false;

        for (auto& icon : icons_) {
            animations_running = update_bounce(icon, now) || animations_running;
        }

        const bool widths_moving = step_widths(dt);
        const bool offsets_moving = step_offsets(dt);
        animations_running = animations_running || widths_moving || offsets_moving;

        if (g_getenv("LUCID_BENCH_IDLE") != nullptr) {
            layout_dirty_ = false;
            animations_running = false;
        }

        if (layout_dirty_ || animations_running) {
            const gint64 t0 = g_get_monotonic_time();
            layout_panel();
            if (bench_total_ > 0) {
                bench_layout_us_.push_back(g_get_monotonic_time() - t0);
            }
            layout_dirty_ = false;
        }

        if (animations_running || layout_dirty_ || bench_remaining_ > 0) {
            return G_SOURCE_CONTINUE;
        }

        tick_id_ = 0;
        last_tick_at_ = 0.0;
        return G_SOURCE_REMOVE;
    }

    bool update_bounce(IconRuntime& icon, double now) {
        if (!icon.bounce_started_at.has_value()) {
            return false;
        }

        const double elapsed = now - *icon.bounce_started_at;
        const double progress = std::min(1.0, elapsed / BOUNCE_DURATION);

        if (progress < 0.5) {
            const double local = sine_in_out(progress * 2.0);
            icon.bounce_offset = bounce_direction() * BOUNCE_HEIGHT * local;
        } else {
            const double local = sine_in_out((progress - 0.5) * 2.0);
            icon.bounce_offset = bounce_direction() * BOUNCE_HEIGHT * (1.0 - local);
        }

        if (progress >= 1.0) {
            const bool still_launching =
                icon.launch_pending_since.has_value() &&
                (now - *icon.launch_pending_since) < LAUNCH_TIMEOUT && !icon.is_open;
            if (still_launching) {
                icon.bounce_started_at = now;      // go again
            } else {
                icon.bounce_started_at.reset();
                icon.bounce_offset = 0.0;
                icon.launch_pending_since.reset();
            }
        }

        update_icon_internal_layout(icon);
        return icon.bounce_started_at.has_value();
    }

    // Targets only. The animated value is integrated separately in step_widths()
    // so that losing the pointer eases back to rest instead of snapping.
    std::vector<double> compute_item_widths() const {
        std::vector<double> widths(icons_.size(), base_width_);
        if (!last_pointer_x_.has_value()) {
            return widths;
        }

        // No overhang allowance here any more. This used to admit pointers up to
        // DISTANCE_LIMIT (345 px) past either end of the panel, which is what
        // let the dock magnify for a cursor that was nowhere near it. Whether
        // the pointer counts at all is now pointer_is_on_panel()'s decision,
        // and it stops at the panel edge.
        const double pointer_x = *last_pointer_x_ - panel_x_;

        for (int pass = 0; pass < 2; ++pass) {
            double cursor = PANEL_PADDING_X;
            std::vector<double> next(widths.size());
            next.reserve(widths.size());

            for (std::size_t i = 0; i < widths.size(); ++i) {
                if (draws_divider(i)) {
                    cursor += DIVIDER_SLOT;
                }
                const double center_x = cursor + widths[i] / 2.0;
                next[i] = magnifier_.interpolate_width(pointer_x - center_x);
                cursor += widths[i] + ITEM_GAP;
            }
            widths = std::move(next);
        }

        return widths;
    }

    // An icon's WIDTH has to be a whole number of pixels -- it is a raster size.
    // Its POSITION does not, and deriving positions from the rounded widths is
    // what made the dock twitch under a slowly moving pointer: one icon's
    // rounding flipping by 1 px shifted every icon after it, and integer
    // division for the centre shifted the whole panel again. Creeping the
    // pointer 0.5 px per frame moved icons by up to 2 px, on half of all frames.
    //
    // So sizes are quantised and positions are not. Positions accumulate from
    // the unrounded widths and are placed with gtk_fixed_move(), which takes
    // doubles. Same 0.5 px pointer move now moves icons 0.5 px.
    void layout_panel() {
        std::vector<int> pixel_widths;
        pixel_widths.reserve(icons_.size());

        for (const auto& icon : icons_) {
            pixel_widths.push_back(std::max(1, static_cast<int>(std::round(icon.current_width))));
        }

        double content_width = 0.0;
        for (std::size_t i = 0; i < icons_.size(); ++i) {
            if (draws_divider(i)) {
                content_width += DIVIDER_SLOT;
            }
            content_width += icons_[i].current_width;
            if (i + 1 < icons_.size()) {
                content_width += ITEM_GAP;
            }
        }
        panel_content_width_ = content_width;

        const bool widths_changed = pixel_widths != last_applied_pixel_widths_;

        {
            const int panel_width =
                static_cast<int>(std::round(content_width)) + PANEL_PADDING_X * 2;

            if (widths_changed) {
                for (std::size_t i = 0; i < icons_.size(); ++i) {
                    gtk_widget_set_size_request(icons_[i].button, pixel_widths[i], ITEM_SLOT_HEIGHT);
                    gtk_image_set_pixel_size(GTK_IMAGE(icons_[i].image), pixel_widths[i]);
                }
            }

            const int panel_height = ITEM_SLOT_HEIGHT + PANEL_PADDING_Y * 2;
            if (panel_width != last_applied_panel_width_) {
                last_applied_panel_width_ = panel_width;
                gtk_widget_set_size_request(panel_fixed_, panel_width, panel_height);
                gtk_widget_set_size_request(panel_bg_, panel_width, panel_bg_height());
            }

            // Centre against the width we actually have. The old code took
            // max(actual, 1280), so on any display narrower than 1280 logical
            // pixels the dock centred itself against a window that did not
            // exist and items landed off-screen.
            const int win_w = gtk_widget_get_width(window_);
            const int win_h = std::max(gtk_widget_get_height(window_), window_height());

            if (win_w <= 0) {
                // Not allocated yet. Re-run once the compositor has sized us
                // rather than laying out against a width of zero.
                layout_dirty_ = true;
                ensure_tick();
                return;
            }

            // Centred in floating point: quantising this to whole pixels makes
            // the entire dock hop sideways every time the panel's rounded width
            // ticks over.
            panel_x_ = std::max(0.0, (win_w - (content_width + PANEL_PADDING_X * 2)) / 2.0);

            // The panel sits BOTTOM_MARGIN above the screen edge and is only
            // tall enough for an unmagnified icon. Everything else is placed
            // relative to the icon baseline inside it, so magnified icons grow
            // up and out of the panel instead of the panel growing to contain
            // them.
            if (at_top()) {
                panel_bg_y_ = BOTTOM_MARGIN;
                // Docked to the top, an icon's fixed edge is its top, and it
                // grows downward. The baseline is that top edge.
                icon_baseline_y_ = panel_bg_y_ + PANEL_PADDING_Y;
                panel_y_ = icon_baseline_y_ - PANEL_PADDING_Y - ICON_BOTTOM_INSET;
            } else {
                panel_bg_y_ = win_h - panel_bg_height() - BOTTOM_MARGIN;
                icon_baseline_y_ = panel_bg_y_ + PANEL_PADDING_Y + base_icon_px();
                panel_y_ = icon_baseline_y_ -
                           (PANEL_PADDING_Y + ITEM_SLOT_HEIGHT - ICON_BOTTOM_INSET);
            }

            gtk_fixed_move(GTK_FIXED(root_fixed_), panel_bg_, panel_x_, static_cast<double>(panel_bg_y_));
            gtk_fixed_move(GTK_FIXED(root_fixed_), panel_fixed_, panel_x_, static_cast<double>(panel_y_));

            // LUCID_DOCK_GEOM=1 dumps the vertical layout once. The dock's
            // geometry is the part that has been wrong most often and it is
            // invisible in a screenshot, so make it printable.
            if (!geom_logged_ && g_getenv("LUCID_DOCK_GEOM") != nullptr) {
                geom_logged_ = true;
                g_print("--- lucid-dock geometry ---\n");
                g_print("window            : %d x %d\n", win_w, win_h);
                g_print("panel (drawn)     : y %d .. %d   height %d\n",
                        panel_bg_y_, panel_bg_y_ + panel_bg_height(), panel_bg_height());
                // The baseline is the icon edge that does not move: its bottom
                // when docked to the bottom, its top when docked to the top.
                // Reporting it as though it were always the bottom printed
                // negative coordinates for a dock that was laid out correctly.
                const int idle_near = at_top() ? icon_baseline_y_
                                               : icon_baseline_y_ - base_icon_px();
                const int idle_far  = at_top() ? icon_baseline_y_ + base_icon_px()
                                               : icon_baseline_y_;
                const int mag_far   = at_top() ? icon_baseline_y_ + max_icon_px()
                                               : icon_baseline_y_ - max_icon_px();
                const int overhang  = at_top()
                                          ? mag_far - (panel_bg_y_ + panel_bg_height())
                                          : panel_bg_y_ - mag_far;
                g_print("position          : %s\n", config_.position.c_str());
                g_print("icon baseline     : y %d  (the %s edge of an icon)\n",
                        icon_baseline_y_, at_top() ? "top" : "bottom");
                g_print("idle icon         : y %d .. %d  (inside the panel)\n",
                        idle_near, idle_far);
                g_print("magnified reaches : y %d  (%d px %s the panel, scale %.2f)\n",
                        mag_far, overhang, at_top() ? "below" : "above", config_.max_scale);
                g_print("item row (no draw): y %d .. %d\n",
                        panel_y_, panel_y_ + panel_height);
                g_print("screen-edge gap   : %d px\n", win_h - (panel_bg_y_ + panel_bg_height()));
            }

            // Separators span the drawn panel's interior, not the item slot:
            // the slot is tall enough for a magnified icon, and a separator
            // that tall would stick out of the panel along with the icons.
            const int divider_y = (panel_bg_y_ - panel_y_) + PANEL_PADDING_Y;
            const int divider_height = panel_bg_height() - PANEL_PADDING_Y * 2;

            // A twentieth of a pixel is under the threshold of anything the
            // compositor can draw differently, and skipping those saves a
            // widget move per icon per frame.
            constexpr double kPositionEpsilon = 0.05;

            double cursor = PANEL_PADDING_X;
            for (std::size_t i = 0; i < icons_.size(); ++i) {
                auto& icon = icons_[i];

                // Visibility is set from the same predicate that decides
                // placement, so a separator that is not positioned this frame
                // is not on screen either -- including after a drag moves its
                // icon to the front.
                if (icon.divider != nullptr) {
                    gtk_widget_set_visible(icon.divider, draws_divider(i) ? TRUE : FALSE);
                }

                if (draws_divider(i)) {
                    const double line_x = cursor + DIVIDER_MARGIN;
                    if (std::abs(icon.divider_x - line_x) > kPositionEpsilon) {
                        icon.divider_x = line_x;
                        gtk_widget_set_size_request(icon.divider, DIVIDER_WIDTH, divider_height);
                        gtk_fixed_move(GTK_FIXED(panel_fixed_), icon.divider,
                                       line_x, static_cast<double>(divider_y));
                    }
                    cursor += DIVIDER_SLOT;
                }

                icon.panel_x = cursor;

                // Second half of the FLIP: the slot moved instantly, so start
                // the drawing where the icon used to be and let the spring
                // close the gap.
                const auto flip = pending_flip_.find(icon.desktop_id);
                if (flip != pending_flip_.end()) {
                    icon.x_offset = flip->second - cursor;
                    icon.x_offset_velocity = 0.0;
                }

                double draw_x = cursor + icon.x_offset;

                // The icon being carried is not in its slot: it is under the
                // pointer, and its slot is the gap it will drop into. Drawn
                // directly rather than sprung, because a lag between the
                // pointer and the thing it is holding feels like the drag has
                // come loose.
                const bool is_carried =
                    dragging_ && drag_from_.has_value() && i == *drag_from_;
                if (is_carried) {
                    draw_x = drag_pointer_x_ - panel_x_ - icon.current_width * 0.5;
                }

                if (std::abs(icon.drawn_x - draw_x) > kPositionEpsilon) {
                    icon.drawn_x = draw_x;
                    gtk_fixed_move(GTK_FIXED(panel_fixed_), icon.button, draw_x,
                                   static_cast<double>(PANEL_PADDING_Y));
                }
                cursor += icon.current_width + ITEM_GAP;
            }

            pending_flip_.clear();
            last_applied_pixel_widths_ = std::move(pixel_widths);
        }

        for (auto& icon : icons_) {
            update_icon_internal_layout(icon);
        }

        update_tooltip(gtk_widget_get_width(window_));
        update_input_region();
    }

    // Restrict the pointer region to the pixels the dock actually occupies.
    //
    // A layer surface takes pointer input across its whole area by default, and
    // this one is monitor-wide and SURFACE_HEIGHT tall -- so without this the
    // dock silently swallows every click in a band across the bottom of the
    // screen, including on the windows behind it. That was already true before
    // the surface grew; the constant height just makes it unmissable.
    //
    // The region is the same rectangle pointer_is_on_panel() accepts, so what
    // the compositor delivers and what the dock considers "on the dock" cannot
    // disagree.
    void update_input_region() {
        GtkNative* native = window_ != nullptr ? gtk_widget_get_native(window_) : nullptr;
        GdkSurface* surface = native != nullptr ? gtk_native_get_surface(native) : nullptr;
        if (surface == nullptr || panel_content_width_ <= 0) {
            return;
        }

        double tallest = base_icon_px();
        for (const auto& icon : icons_) {
            tallest = std::max(tallest, icon.current_width);
        }

        const int x = static_cast<int>(std::floor(panel_x_));
        const int width =
            static_cast<int>(std::ceil(panel_content_width_ + 2.0 * PANEL_PADDING_X)) + 1;
        const int top = static_cast<int>(std::floor(
            at_top() ? static_cast<double>(panel_bg_y_)
                     : icon_baseline_y_ - tallest - PANEL_PADDING_Y));
        // Docked to the top the region runs from the edge down past the
        // tallest icon; docked to the bottom it runs from the tallest icon to
        // the bottom of the surface.
        const int height =
            at_top() ? static_cast<int>(std::ceil(tallest + PANEL_PADDING_Y * 2)) + BOTTOM_MARGIN
                     : std::max(gtk_widget_get_height(window_), SURFACE_HEIGHT) - top;

        const cairo_rectangle_int_t rect{x, std::max(0, top), width, std::max(1, height)};
        if (rect.x == last_input_region_.x && rect.y == last_input_region_.y &&
            rect.width == last_input_region_.width && rect.height == last_input_region_.height) {
            return;
        }
        last_input_region_ = rect;

        cairo_region_t* region = cairo_region_create_rectangle(&rect);
        gdk_surface_set_input_region(surface, region);
        cairo_region_destroy(region);
    }

    void update_icon_internal_layout(IconRuntime& icon) {
        const int pixel_width = static_cast<int>(std::round(icon.current_width));
        const double base_y = icon_inset_y(pixel_width);
        const double image_y = std::max(0.0, base_y + icon.bounce_offset);
        const int dot_x = std::max(0, (pixel_width - DOT_SIZE) / 2);
        // The running dot sits against the screen edge, so it swaps ends too.
        const int dot_y = at_top() ? 8 : ITEM_SLOT_HEIGHT - 12;

        // Deliberately NOT a GskTransform. Measured on Iris Pro 5200 at 2x:
        // gtk_fixed_set_child_transform() per child per frame runs at 30 fps,
        // gtk_image_set_pixel_size() at 60. A non-trivial transform makes GSK
        // render the child into an offscreen texture, and ten offscreens a
        // frame cost more than re-rasterising ten SVGs. Sub-pixel y is kept --
        // that is what stops the bounce stepping between integers.
        if (icon.last_image_y != image_y) {
            icon.last_image_y = image_y;
            gtk_fixed_move(GTK_FIXED(icon.fixed), icon.image, 0.0, image_y);
        }
        if (icon.last_dot_x != dot_x || icon.last_dot_y != dot_y) {
            icon.last_dot_x = dot_x;
            icon.last_dot_y = dot_y;
            gtk_fixed_move(GTK_FIXED(icon.fixed), icon.dot, static_cast<double>(dot_x), static_cast<double>(dot_y));
        }
    }

    // Current display scale, or 1 before the window is realised. GTK reports
    // the integer scale; under fractional scaling it rounds up, which is what
    // we want -- rasterising slightly large and scaling down stays crisp.
    int current_scale() const {
        const int scale = window_ != nullptr ? gtk_widget_get_scale_factor(window_) : 1;
        return scale > 0 ? scale : 1;
    }

    // Rasterise at icon_source_size() x scale so every magnification step and
    // every display scale is a downscale rather than an upscale.
    void apply_icon_paintable(IconRuntime& icon) {
        const int scale = current_scale();
        GtkIconPaintable* paintable = nullptr;

        if (!icon.resolved_file.empty()) {
            GFile* file = g_file_new_for_path(icon.resolved_file.c_str());
            paintable = gtk_icon_paintable_new_for_file(file, icon_source_size(), scale);
            g_object_unref(file);
        } else {
            GtkIconTheme* theme = lookup_theme();
            {
                const char* fallbacks[] = {"application-x-executable", nullptr};
                paintable = gtk_icon_theme_lookup_icon(
                    theme,
                    icon.resolved_icon_name.c_str(),
                    fallbacks,
                    icon_source_size(),
                    scale,
                    GTK_TEXT_DIR_NONE,
                    GTK_ICON_LOOKUP_FORCE_REGULAR);
            }
        }

        if (paintable != nullptr) {
            gtk_image_set_from_paintable(GTK_IMAGE(icon.image), GDK_PAINTABLE(paintable));
            g_object_unref(paintable);
        }
    }

    void reload_icons_for_scale() {
        const int scale = current_scale();
        if (scale == last_icon_scale_) {
            return;
        }
        last_icon_scale_ = scale;
        for (auto& icon : icons_) {
            if (icon.image != nullptr) {
                apply_icon_paintable(icon);
            }
        }
        queue_layout();
    }

    static void on_scale_changed_static(GObject*, GParamSpec*, gpointer user_data) {
        static_cast<DockEngine*>(user_data)->reload_icons_for_scale();
    }

    static void on_surface_size_changed_static(GObject*, GParamSpec*, gpointer user_data) {
        static_cast<DockEngine*>(user_data)->queue_layout();
    }

    static void on_window_realize_static(GtkWidget* widget, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
        if (surface != nullptr) {
            g_signal_connect(surface, "notify::width",
                             G_CALLBACK(&DockEngine::on_surface_size_changed_static), self);
            g_signal_connect(surface, "notify::height",
                             G_CALLBACK(&DockEngine::on_surface_size_changed_static), self);
        }
        self->reload_icons_for_scale();
        self->queue_layout();
    }

    void build_icons(const std::vector<DesktopApp>& dock_apps) {
        icons_.clear();
        icons_.reserve(dock_apps.size());

        for (const auto& app : dock_apps) {
            IconRuntime icon;
            icon.app_id = app.app_id;
            icon.desktop_id = app.desktop_path.filename().string();
            icon.pinned = app.pinned;
            icon.title = app.title;
            icon.desktop_path = app.desktop_path;
            icon.icon_name = app.icon_name;
            icon.process_candidates = app.process_candidates;
            icon.is_open = app.is_open;
            icon.divider_before = app.divider_before;

            icon.button = gtk_button_new();
            gtk_widget_add_css_class(icon.button, "dock-item");
            gtk_widget_set_can_focus(icon.button, FALSE);

            icon.fixed = gtk_fixed_new();
            gtk_button_set_child(GTK_BUTTON(icon.button), icon.fixed);

            // An absolute path in the desktop file is used as-is; anything
            // else is a name, and names are the icon theme's business.
            if (!icon.icon_name.empty() && std::filesystem::is_regular_file(icon.icon_name)) {
                icon.resolved_file = icon.icon_name;
            } else {
                icon.resolved_icon_name =
                    icon.icon_name.empty() ? "application-x-executable" : icon.icon_name;
            }

            icon.current_width = base_width_;
            icon.target_width = base_width_;
            icon.image = gtk_image_new();
            apply_icon_paintable(icon);
            gtk_image_set_pixel_size(GTK_IMAGE(icon.image), base_icon_px());
            gtk_fixed_put(GTK_FIXED(icon.fixed), icon.image, 0.0, 0.0);

            icon.dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(icon.dot, "dock-dot");
            gtk_widget_set_size_request(icon.dot, DOT_SIZE, DOT_SIZE);
            gtk_widget_set_visible(icon.dot, icon.is_open ? TRUE : FALSE);
            gtk_fixed_put(GTK_FIXED(icon.fixed), icon.dot, 0.0, 0.0);

            gtk_fixed_put(GTK_FIXED(panel_fixed_), icon.button, 0.0, 0.0);

            if (icon.divider_before) {
                icon.divider = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_add_css_class(icon.divider, "dock-divider");
                gtk_fixed_put(GTK_FIXED(panel_fixed_), icon.divider, 0.0, 0.0);
            }

            icons_.push_back(std::move(icon));
            IconRuntime& new_icon = icons_.back();
            g_object_set_data_full(G_OBJECT(new_icon.button), "lucid-desktop-id",
                                   g_strdup(new_icon.desktop_id.c_str()), g_free);
            g_signal_connect(new_icon.button, "clicked",
                             G_CALLBACK(&DockEngine::on_icon_clicked_static), this);
        }
    }

    // -----------------------------------------------------------------------
    // Reordering
    //
    // The icon order and the config file's Pinned list are the same list. The
    // drag reorders icons_ and then rewrites Pinned from it, so what is on
    // screen and what is on disk cannot disagree -- there is no third copy to
    // fall out of step.

    // Which slot would an icon dropped at x land in? Unlike icon_at() this
    // never returns nothing: a drag has to resolve to some slot, so a pointer
    // past either end clamps to the first or last.
    std::size_t drop_slot_at(double x) const {
        if (icons_.empty()) {
            return 0;
        }
        for (std::size_t i = 0; i < icons_.size(); ++i) {
            const double left = panel_x_ + icons_[i].panel_x;
            if (x < left + icons_[i].current_width * 0.5) {
                return i;
            }
        }
        return icons_.size() - 1;
    }

    static void on_reorder_begin_static(GtkGestureDrag* gesture, double x, double y,
                                        gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        (void)gesture;
        self->drag_from_ = std::nullopt;
        self->dragging_ = false;
        if (!self->pointer_is_on_panel(x, y)) {
            return;
        }
        self->drag_origin_x_ = x;
        self->drag_from_ = self->icon_at(x);
    }

    static void on_reorder_update_static(GtkGestureDrag* gesture, double dx, double dy,
                                         gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        if (!self->drag_from_.has_value()) {
            return;
        }

        if (!self->dragging_) {
            if (std::abs(dx) < REORDER_THRESHOLD) {
                return;   // still a click
            }
            self->dragging_ = true;
            self->drag_pointer_x_ = self->drag_origin_x_ + dx;
            self->queue_layout();   // start the collapse immediately
            // Now it is a drag: take the sequence so the button underneath
            // does not also fire a click when the pointer is released.
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        }
        (void)dy;

        const double x = self->drag_origin_x_ + dx;
        self->drag_pointer_x_ = x;
        const std::size_t from = *self->drag_from_;
        const std::size_t to = self->drop_slot_at(x);
        if (to == from) {
            return;
        }

        // FLIP: remember where everything is drawn now, reorder, and let
        // layout turn the difference into an offset that springs back to zero.
        // Reordering without this is correct and instant, which is exactly what
        // makes it read as a glitch rather than a swap.
        std::map<std::string, double> before;
        for (const auto& icon : self->icons_) {
            before[icon.desktop_id] = icon.panel_x + icon.x_offset;
        }

        IconRuntime moved = std::move(self->icons_[from]);
        self->icons_.erase(self->icons_.begin() + static_cast<long>(from));
        self->icons_.insert(self->icons_.begin() + static_cast<long>(to), std::move(moved));
        self->drag_from_ = to;
        self->current_mouse_x_ = x;
        self->hovered_index_ = to;
        self->pending_flip_ = std::move(before);
        self->queue_layout();
    }

    static void on_reorder_end_static(GtkGestureDrag*, double, double, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        const bool moved = self->dragging_;
        const auto index = self->drag_from_;

        // Where the carried icon was last drawn, so releasing it slides it into
        // the slot rather than teleporting it there.
        if (moved && index.has_value() && *index < self->icons_.size()) {
            auto& icon = self->icons_[*index];
            icon.x_offset = icon.drawn_x - icon.panel_x;
            icon.x_offset_velocity = 0.0;
        }

        self->dragging_ = false;
        self->drag_from_ = std::nullopt;
        self->queue_layout();
        if (!moved) {
            return;
        }

        // Deliberately placing a running application means keeping it. A drag
        // that snapped back when the app quit would make the gesture feel
        // broken, so dropping it pins it where it was dropped.
        if (index.has_value() && *index < self->icons_.size() &&
            !self->icons_[*index].pinned) {
            self->icons_[*index].pinned = true;
            self->icons_[*index].divider_before = false;
            self->refresh_running_divider();
        }
        self->persist_order();
    }

    // The separator marks where pinned items end. After anything is pinned or
    // unpinned it belongs before the first remaining running-only icon, and
    // nowhere else.
    void refresh_running_divider() {
        bool first = true;
        for (auto& icon : icons_) {
            if (icon.pinned) {
                continue;
            }
            const bool want = first;
            first = false;
            if (icon.divider_before == want) {
                continue;
            }
            icon.divider_before = want;
            if (want && icon.divider == nullptr) {
                icon.divider = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_add_css_class(icon.divider, "dock-divider");
                gtk_fixed_put(GTK_FIXED(panel_fixed_), icon.divider, 0.0, 0.0);
            } else if (!want && icon.divider != nullptr) {
                gtk_fixed_remove(GTK_FIXED(panel_fixed_), icon.divider);
                icon.divider = nullptr;
            }
        }
        queue_layout();
    }

    // Rewrite Pinned from the on-screen order. Anything in the config that has
    // no icon -- an app that was uninstalled since the file was written -- is
    // kept at the end rather than silently dropped, so pinning something,
    // uninstalling it and reinstalling it does not lose its place.
    void persist_order() {
        std::vector<std::string> order;
        order.reserve(icons_.size());
        for (const auto& icon : icons_) {
            // Running-but-unpinned icons are on the dock transiently and must
            // not be written to Pinned, or quitting an app would leave it
            // pinned behind and the dock would only ever grow.
            if (icon.pinned && !icon.desktop_id.empty()) {
                order.push_back(icon.desktop_id);
            }
        }
        for (const auto& id : config_.pinned) {
            if (std::find(order.begin(), order.end(), id) == order.end()) {
                order.push_back(id);
            }
        }
        config_.pinned = std::move(order);

        // Dividers are stored as "the item this one sits before", so they
        // follow the icons automatically and need no separate fixing up.
        write_config(config_);
    }

    void refresh_running_indicators() {
        ScopedWork timer(this, "refresh_running_indicators");
        std::unordered_set<std::string> running;
        {
            ScopedWork inner(this, "  collect_running_process_names");
            running = collect_running_process_names();
        }

        // Compare only the names the dock could possibly care about. The raw
        // /proc set churns every few seconds -- short-lived helpers, workers,
        // shells -- so comparing it wholesale almost never matches and the
        // skip never fires. The set of *our* candidates that are running is
        // stable, because it only changes when an application the dock can show
        // starts or stops.
        std::set<std::string> relevant;
        ScopedWork relevant_timer(this, "  match relevant candidates");
        for (const auto& [desktop_id, candidates] : candidates()) {
            for (const auto& candidate : candidates) {
                if (running.find(candidate) != running.end()) {
                    relevant.insert(candidate);
                    break;
                }
            }
        }
        if (relevant == last_relevant_) {
            return;
        }
        if (g_getenv("LUCID_DOCK_TIMERS") != nullptr && !last_relevant_.empty()) {
            std::string added, removed;
            for (const auto& r : relevant) {
                if (!last_relevant_.count(r)) { added += r; added += " "; }
            }
            for (const auto& r : last_relevant_) {
                if (!relevant.count(r)) { removed += r; removed += " "; }
            }
            g_message("running set changed: +[%s] -[%s]", added.c_str(), removed.c_str());
        }
        last_relevant_ = std::move(relevant);
        for (auto& icon : icons_) {
            bool is_open = false;
            for (const auto& name : icon.process_candidates) {
                if (running.find(name) != running.end()) {
                    is_open = true;
                    break;
                }
            }
            icon.is_open = is_open;
            gtk_widget_set_visible(icon.dot, is_open ? TRUE : FALSE);

            // Appearing in the process list is what ends a launch. The bounce
            // stops at the end of its current cycle rather than mid-air.
            if (is_open) {
                icon.launch_pending_since.reset();
            }
        }

        if (config_.show_running) {
            sync_running_items(running);
        }
    }

    // An application starting or quitting changes which icons exist, which is
    // a rebuild. Comparing the set first matters: rebuilding unconditionally
    // every four seconds would restart every animation and make the dock twitch
    // on a timer.
    void reload_catalog() {
        ScopedWork timer(this, "reload_catalog");
        catalog_ = load_desktop_catalog();
        candidates_.clear();
        for (const auto& [desktop_id, entry] : catalog_) {
            candidates_[desktop_id] = exec_candidates(entry.exec_line, desktop_id);
        }
    }

    const std::map<std::string, DesktopCatalogEntry>& catalog() {
        if (catalog_.empty()) {
            reload_catalog();
        }
        return catalog_;
    }

    const std::map<std::string, std::vector<std::string>>& candidates() {
        if (catalog_.empty()) {
            reload_catalog();
        }
        return candidates_;
    }

    void sync_running_items(const std::unordered_set<std::string>& running) {
        ScopedWork timer(this, "sync_running_items");

        std::vector<std::string> want;
        {
            ScopedWork inner(this, "  load_dock_apps");
            for (const auto& app : load_dock_apps(config_, catalog(), &candidates_, &running)) {
                want.push_back(app.desktop_path.filename().string());
            }
        }

        std::vector<std::string> have;
        have.reserve(icons_.size());
        for (const auto& icon : icons_) {
            have.push_back(icon.desktop_id);
        }
        if (want == have) {
            return;
        }

        // Carry the animated width across the rebuild so icons that survive it
        // do not snap back to rest under the pointer.
        std::map<std::string, std::pair<double, double>> carried;
        for (const auto& icon : icons_) {
            carried[icon.desktop_id] = {icon.current_width, icon.width_velocity};
        }

        rebuild_items();

        for (auto& icon : icons_) {
            const auto it = carried.find(icon.desktop_id);
            if (it != carried.end()) {
                icon.current_width = it->second.first;
                icon.width_velocity = it->second.second;
            }
        }
        queue_layout();
    }

    // While something is starting, poll far more often than the idle 4 s
    // refresh -- otherwise an app that opens in half a second still bounces for
    // four. The fast timer stops itself as soon as nothing is pending.
    static gboolean on_launch_poll_static(gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->refresh_running_indicators();

        const double now = monotonic_seconds();
        bool pending = false;
        for (const auto& icon : self->icons_) {
            if (icon.launch_pending_since.has_value() &&
                (now - *icon.launch_pending_since) < LAUNCH_TIMEOUT) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            self->launch_poll_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        self->ensure_tick();
        return G_SOURCE_CONTINUE;
    }

    void ensure_launch_poll() {
        if (launch_poll_id_ == 0) {
            launch_poll_id_ = g_timeout_add(LAUNCH_POLL_MS,
                                            &DockEngine::on_launch_poll_static, this);
        }
    }

    // -----------------------------------------------------------------------
    // Configuration menu and live reload
    //
    // The config file is the only source of truth. The menu does not hold
    // settings in memory and hand them out -- it writes the file, and the file
    // is what everything reads. That is what keeps a settings application
    // honest later: it edits the same file, the dock notices, and neither side
    // needs to know the other exists. No IPC, no D-Bus, no protocol.
    //
    // Reload never writes, so a write cannot trigger a write. No loop, and no
    // need to suppress our own file-monitor events.
    // -----------------------------------------------------------------------

    void install_menu() {
        actions_ = g_simple_action_group_new();

        GSimpleAction* magnification = g_simple_action_new_stateful(
            "magnification", nullptr, g_variant_new_boolean(config_.magnification ? TRUE : FALSE));
        g_signal_connect(magnification, "change-state",
                         G_CALLBACK(&DockEngine::on_magnification_action_static), this);
        g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(magnification));
        g_object_unref(magnification);

        for (const auto& [name, handler] : {
                 std::pair<const char*, GCallback>{"settings", G_CALLBACK(&DockEngine::on_settings_static)},
                 std::pair<const char*, GCallback>{"edit-config", G_CALLBACK(&DockEngine::on_edit_config_static)},
                 std::pair<const char*, GCallback>{"reload-config", G_CALLBACK(&DockEngine::on_reload_config_static)},
                 std::pair<const char*, GCallback>{"quit", G_CALLBACK(&DockEngine::on_quit_action_static)}}) {
            GSimpleAction* action = g_simple_action_new(name, nullptr);
            g_signal_connect(action, "activate", handler, this);
            g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(action));
            g_object_unref(action);
        }

        gtk_widget_insert_action_group(window_, "dock", G_ACTION_GROUP(actions_));

        GMenu* menu = g_menu_new();

        GMenu* appearance = g_menu_new();
        g_menu_append(appearance, "Magnification", "dock.magnification");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(appearance));
        g_object_unref(appearance);

        GMenu* configuration = g_menu_new();
        g_menu_append(configuration, "Dock Settings\u2026", "dock.settings");
        g_menu_append(configuration, "Edit Configuration File\u2026", "dock.edit-config");
        g_menu_append(configuration, "Reload Configuration", "dock.reload-config");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(configuration));
        g_object_unref(configuration);

        GMenu* quit_section = g_menu_new();
        g_menu_append(quit_section, "Quit Dock", "dock.quit");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(quit_section));
        g_object_unref(quit_section);

        menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        g_object_unref(menu);

        gtk_popover_set_has_arrow(GTK_POPOVER(menu_), FALSE);
        gtk_widget_set_halign(menu_, GTK_ALIGN_START);
        gtk_widget_set_parent(menu_, root_fixed_);
    }

    static void on_secondary_press_static(GtkGestureClick* gesture, gint, double x, double y,
                                          gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        if (self->menu_ == nullptr || !self->pointer_is_on_panel(x, y)) {
            return;
        }

        const GdkRectangle at{static_cast<int>(x), static_cast<int>(y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(self->menu_), &at);
        gtk_popover_popup(GTK_POPOVER(self->menu_));
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }

    static void on_magnification_action_static(GSimpleAction* action, GVariant* state,
                                               gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        const bool enabled = g_variant_get_boolean(state) != FALSE;
        g_simple_action_set_state(action, g_variant_new_boolean(enabled ? TRUE : FALSE));

        self->config_.magnification = enabled;
        write_config(self->config_);
        self->queue_layout();
    }

    // Launch lucid-dock-settings. Looked up next to this binary first so a
    // build tree works without installing, then on PATH. Falls back to opening
    // the config file if the settings binary is not there at all -- the dock
    // must not lose its settings entry point just because one of two binaries
    // was deployed.
    static void on_settings_static(GSimpleAction* action, GVariant* variant, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);

        std::string program;
        gchar* self_path = g_file_read_link("/proc/self/exe", nullptr);
        if (self_path != nullptr) {
            const std::filesystem::path sibling =
                std::filesystem::path(self_path).parent_path() / "lucid_dock_settings";
            if (std::filesystem::is_regular_file(sibling)) {
                program = sibling.string();
            }
            g_free(self_path);
        }
        if (program.empty()) {
            gchar* found = g_find_program_in_path("lucid_dock_settings");
            if (found != nullptr) {
                program = found;
                g_free(found);
            }
        }
        if (program.empty()) {
            g_message("lucid_dock_settings not found; opening the configuration file instead.");
            on_edit_config_static(action, variant, user_data);
            return;
        }

        const gchar* argv[] = {program.c_str(), nullptr};
        GError* error = nullptr;
        if (!g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr, G_SPAWN_DEFAULT,
                           nullptr, nullptr, nullptr, &error)) {
            g_warning("Could not launch %s: %s", program.c_str(),
                      error != nullptr ? error->message : "unknown error");
            g_clear_error(&error);
            on_edit_config_static(action, variant, user_data);
        }
        (void)self;
    }

    static void on_edit_config_static(GSimpleAction*, GVariant*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        // Make sure there is something to open before handing it to an editor.
        if (!std::filesystem::exists(config_file_path())) {
            write_config(self->config_);
        }

        gchar* uri = g_filename_to_uri(config_file_path().c_str(), nullptr, nullptr);
        if (uri != nullptr) {
            GError* error = nullptr;
            if (!g_app_info_launch_default_for_uri(uri, nullptr, &error) && error != nullptr) {
                g_warning("Could not open %s: %s", uri, error->message);
                g_error_free(error);
            }
            g_free(uri);
        }
    }

    static void on_reload_config_static(GSimpleAction*, GVariant*, gpointer user_data) {
        static_cast<DockEngine*>(user_data)->reload_config();
    }

    static void on_quit_action_static(GSimpleAction*, GVariant*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        if (self->window_ != nullptr) {
            gtk_window_close(GTK_WINDOW(self->window_));
        }
    }

    void watch_config() {
        GFile* file = g_file_new_for_path(config_file_path().c_str());
        config_monitor_ = g_file_monitor_file(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
        g_object_unref(file);

        if (config_monitor_ != nullptr) {
            g_signal_connect(config_monitor_, "changed",
                             G_CALLBACK(&DockEngine::on_config_file_changed_static), this);
        }
    }

    static void on_config_file_changed_static(GFileMonitor*, GFile*, GFile*,
                                              GFileMonitorEvent event, gpointer user_data) {
        if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
            event != G_FILE_MONITOR_EVENT_CREATED &&
            event != G_FILE_MONITOR_EVENT_RENAMED) {
            return;
        }
        static_cast<DockEngine*>(user_data)->reload_config();
    }

    void reload_config() {
        DockConfig config;
        if (!read_config(config)) {
            return;
        }

        // Only the item list justifies tearing widgets down; everything else is
        // a value change and can be applied in place.
        const bool items_changed = config.pinned != config_.pinned ||
                                   config.dividers_before != config_.dividers_before;
        config_ = config;

        if (GAction* action = g_action_map_lookup_action(G_ACTION_MAP(actions_), "magnification")) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action),
                                      g_variant_new_boolean(config_.magnification ? TRUE : FALSE));
        }

        apply_config_geometry();
        geom_logged_ = false;   // geometry may have moved; let it print again
        apply_window_size();

        if (items_changed) {
            rebuild_items();
        }
        queue_layout();
    }

    void rebuild_items() {
        ScopedWork timer(this, "rebuild_items");
        rebuild_items_impl();
    }

    void rebuild_items_impl() {
        for (auto& icon : icons_) {
            if (icon.divider != nullptr) {
                gtk_fixed_remove(GTK_FIXED(panel_fixed_), icon.divider);
            }
            gtk_fixed_remove(GTK_FIXED(panel_fixed_), icon.button);
        }
        icons_.clear();
        last_applied_pixel_widths_.clear();

        build_icons(load_dock_apps(config_, catalog(), &candidates_));
        last_icon_scale_ = 0;
    }

    // Empty IconTheme means "whatever the desktop is set to", which is what a
    // dock should do by default. Setting it overrides that for this process
    // only -- the dock does not reach into the user's desktop settings to get
    // the icons it wants.
    void apply_icon_theme() {
        GdkDisplay* display = window_ != nullptr ? gtk_widget_get_display(window_)
                                                 : gdk_display_get_default();
        if (display == nullptr) {
            return;
        }
        GtkIconTheme* desktop_theme = gtk_icon_theme_get_for_display(display);

        if (config_.icon_theme.empty()) {
            const char* name =
                desktop_theme != nullptr ? gtk_icon_theme_get_theme_name(desktop_theme) : nullptr;
            g_message("icon theme: %s (from the desktop)", name != nullptr ? name : "(none)");
            return;
        }

        // An owned GtkIconTheme, not the display's. The display's tracks
        // GtkSettings' gtk-icon-theme-name and overwrites anything set on it,
        // so asking for a theme there silently does nothing. This one is ours
        // and answers for itself -- and using it keeps the choice local to the
        // dock rather than changing the icons of every application.
        if (icon_theme_ != nullptr) {
            g_object_unref(icon_theme_);
            icon_theme_ = nullptr;
        }
        icon_theme_ = gtk_icon_theme_new();
        if (desktop_theme != nullptr) {
            if (char** path = gtk_icon_theme_get_search_path(desktop_theme)) {
                gtk_icon_theme_set_search_path(icon_theme_, path);
                g_strfreev(path);
            }
        }
        gtk_icon_theme_set_theme_name(icon_theme_, config_.icon_theme.c_str());

        // Ask whether it actually resolved, rather than whether it was set:
        // GTK falls back to hicolor for a theme that is not installed and says
        // nothing. Not checking is how the dock spent months pointing at a
        // theme that was not on the machine.
        if (!gtk_icon_theme_has_icon(icon_theme_, "folder")) {
            g_warning("icon theme '%s' is not installed, or has no 'folder' icon -- "
                      "falling back to the desktop's. Install it under "
                      "/usr/share/icons or ~/.local/share/icons, in a directory "
                      "whose index.theme Name matches.",
                      config_.icon_theme.c_str());
            g_object_unref(icon_theme_);
            icon_theme_ = nullptr;
            return;
        }
        g_message("icon theme: %s (from dock.conf)", config_.icon_theme.c_str());
    }

    GtkIconTheme* lookup_theme() {
        if (icon_theme_ != nullptr) {
            return icon_theme_;
        }
        GdkDisplay* display = window_ != nullptr ? gtk_widget_get_display(window_)
                                                 : gdk_display_get_default();
        return display != nullptr ? gtk_icon_theme_get_for_display(display) : nullptr;
    }

    void install_css() {
        static const char* kCss = R"CSS(
window {
    background: transparent;
}

.dock-root {
    background: transparent;
}

/* GtkButton brings Adwaita's background, border and shadow with it. On a
   128 px item slot that draws a visible vertical band behind every icon --
   harmless while the panel was tall enough to hide them, glaring now that the
   slots stick out above it. The dock item is a hit target, not a button. */
.dock-item,
.dock-item:hover,
.dock-item:active,
.dock-item:checked,
.dock-item:focus {
    background-color: transparent;
    background-image: none;
    border-width: 0;
    border-color: transparent;
    box-shadow: none;
    outline-style: none;
    min-width: 0;
    min-height: 0;
    padding: 0;
    margin: 0;
}

/* The reference has no transition on this: display none to display block, so
   it is up on the same frame the pointer arrives. GTK has no backdrop-filter,
   so the translucency is raised slightly to stand in for the 5px blur. */
.dock-tooltip {
    background-color: rgba(252, 252, 252, 0.72);
    color: rgba(16, 16, 16, 0.95);
    border-radius: 6px;
    padding: 6px 12px;
    box-shadow:
        inset 0 0 0 1px rgba(255, 255, 255, 0.30),
        0 1px 5px 2px rgba(0, 0, 0, 0.30);
    font-size: 0.95em;
}

.dock-divider {
    background-color: rgba(20, 20, 20, 0.30);
}

.dock-panel {
    background: rgba(255, 255, 255, 0.4);
    border-radius: 19px;
    box-shadow:
        inset 0 0 0 1px rgba(255, 255, 255, 0.28),
        0 0 0 1px rgba(20, 20, 20, 0.22),
        2px 5px 19px 7px rgba(0, 0, 0, 0.3);
}
)CSS";


        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, kCss);

        GdkDisplay* display = gdk_display_get_default();
        if (display != nullptr) {
            gtk_style_context_add_provider_for_display(
                display,
                GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }

        g_object_unref(provider);
    }

    static double monotonic_seconds() {
        return static_cast<double>(g_get_monotonic_time()) / 1000000.0;
    }

  private:
    GtkWidget* window_ = nullptr;
    GtkWidget* root_fixed_ = nullptr;
    GtkWidget* panel_bg_ = nullptr;
    GtkWidget* panel_fixed_ = nullptr;
    GtkWidget* tooltip_ = nullptr;
    std::string tooltip_text_;
    int tooltip_w_ = -1;   // cached natural size; -1 means re-measure
    int tooltip_h_ = 0;
    std::optional<std::size_t> hovered_index_;

    std::vector<IconRuntime> icons_;
    MagnificationProfile magnifier_;
    DockConfig config_;
    double base_width_ = BASE_WIDTH;
    GtkWidget* menu_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;
    GFileMonitor* config_monitor_ = nullptr;
    guint launch_poll_id_ = 0;
    GtkIconTheme* icon_theme_ = nullptr;   // owned only when IconTheme is set
    std::map<std::string, WorkStat> work_;

    // Parsing 148 .desktop files costs ~15 ms. The set of installed
    // applications does not change between two ticks of a 4 s timer, so read it
    // once and reuse it. reload_catalog() exists for when it genuinely changes;
    // nothing calls it on a timer.
    std::map<std::string, DesktopCatalogEntry> catalog_;

    // exec_candidates() over the whole catalog is another ~8 ms, and it is a
    // pure function of the entry, so it is cached beside the catalog and
    // invalidated with it.
    std::map<std::string, std::vector<std::string>> candidates_;
    std::set<std::string> last_relevant_;

    std::optional<std::size_t> drag_from_;
    double drag_origin_x_ = 0.0;
    double drag_pointer_x_ = 0.0;
    bool dragging_ = false;
    std::map<std::string, double> pending_flip_;
    std::vector<int> last_applied_pixel_widths_;

    std::optional<double> current_mouse_x_;
    // Where the pointer was when it left. Kept until the envelope has finished
    // decaying, so the dock shrinks from the shape it had rather than snapping
    // flat and then easing nothing.
    std::optional<double> last_pointer_x_;
    double envelope_ = 0.0;
    double envelope_velocity_ = 0.0;

    guint tick_id_ = 0;
    guint running_refresh_id_ = 0;
    bool layout_dirty_ = false;
    // True only when the compositor actually implements wlr-layer-shell.
    bool layer_shell_active_ = false;
    int last_icon_scale_ = 0;
    double last_tick_at_ = 0.0;

    int bench_total_ = 0;
    int bench_remaining_ = 0;
    gint64 bench_last_frame_us_ = 0;
    std::vector<gint64> bench_frame_us_;
    std::vector<gint64> bench_layout_us_;

    double panel_x_ = 0.0;
    int panel_y_ = 0;
    bool geom_logged_ = false;
    int panel_bg_y_ = 0;
    int icon_baseline_y_ = 0;
    double panel_content_width_ = 0.0;
    int last_applied_panel_width_ = -1;
    cairo_rectangle_int_t last_input_region_{-1, -1, -1, -1};
};

void on_activate(GtkApplication* app, gpointer) {
    static DockEngine engine;
    const auto catalog = load_desktop_catalog();
    const DockConfig config = ensure_config(catalog);
    engine.build_ui(app, config, load_dock_apps(config, catalog));
}

}  // namespace

#ifndef LUCID_BUILD_STAMP
#define LUCID_BUILD_STAMP "unknown"
#endif
#ifndef LUCID_GIT_REV
#define LUCID_GIT_REV "unknown"
#endif

int main(int argc, char** argv) {
    // Printed unconditionally. Knowing which build is running has repeatedly
    // been the difference between a real measurement and a wasted afternoon,
    // and it costs one line of output.
    g_message("lucid-dock %s built %s", LUCID_GIT_REV, LUCID_BUILD_STAMP);
    if (argc > 1 && (g_strcmp0(argv[1], "--version") == 0)) {
        return 0;
    }

    // Renderer choice has to happen before GTK initialises, which is before the
    // dock reads anything else, so this reads the config file directly rather
    // than going through the engine. g_setenv with overwrite=FALSE so an
    // explicit GSK_RENDERER in the environment still wins.
    {
        lucid::DockConfig early;
        if (lucid::read_config(early) && !early.renderer.empty()) {
            const bool applied = g_setenv("GSK_RENDERER", early.renderer.c_str(), FALSE) != FALSE;
            const char* effective = g_getenv("GSK_RENDERER");
            // Worth printing: which renderer is in use is the first question
            // for any "the dock is slow" report, and it is otherwise invisible.
            g_message("renderer: config asked for '%s', GSK_RENDERER is now '%s'%s",
                      early.renderer.c_str(), effective != nullptr ? effective : "(unset)",
                      applied ? "" : " (setenv failed)");
        }
    }

    GtkApplication* app = gtk_application_new("dev.lucidos.DockCpp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
