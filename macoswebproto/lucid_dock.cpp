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
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace lucid;

constexpr double BASE_WIDTH = 57.6;
constexpr double DEFAULT_MAX_SCALE = 2.0;
constexpr double MAX_WIDTH = BASE_WIDTH * DEFAULT_MAX_SCALE;
constexpr double DISTANCE_LIMIT = BASE_WIDTH * 6.0;

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

constexpr int BASE_ICON_PX = static_cast<int>(BASE_WIDTH + 0.5);

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
constexpr int PANEL_BG_HEIGHT = BASE_ICON_PX + DOT_SIZE + 1 + PANEL_PADDING_Y * 2;


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
constexpr double WIDTH_SETTLE_EPSILON = 0.25;   // px
constexpr double WIDTH_SETTLE_VELOCITY = 2.0;   // px/s

constexpr double BOUNCE_HEIGHT = 40.0;
constexpr double BOUNCE_DURATION = 0.4;

struct DesktopApp {
    std::string app_id;
    std::string title;
    std::filesystem::path desktop_path;
    std::string icon_name;
    std::string exec_line;
    std::vector<std::string> process_candidates;
    bool is_open = false;
    bool divider_before = false;
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
    double target_width = BASE_WIDTH;    // where the pointer says it should be
    double bounce_offset = 0.0;
    std::optional<double> bounce_started_at;

    double last_image_y = -1.0;
    int last_dot_x = -1;
    int last_dot_y = -1;
    double panel_x = -1.0;
    bool is_open = false;

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

std::filesystem::path find_icon_in_lucid_theme(const std::string& icon_name) {
    if (icon_name.empty()) {
        return {};
    }

    static const std::vector<std::string> kSearchSubdirs = {
        "scalable/apps",
        "256x256/apps",
        "128x128/apps",
        "64x64/apps",
        "512x512/apps",
    };
    static const std::vector<std::string> kExtensions = {"svg", "png", "xpm"};

    const std::filesystem::path theme_dir("/usr/share/icons/Lucid-Light");
    for (const auto& subdir : kSearchSubdirs) {
        for (const auto& ext : kExtensions) {
            const std::filesystem::path candidate = theme_dir / subdir / (icon_name + "." + ext);
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }

    return {};
}

// What to pin when there is no config file and no GNOME to borrow from. One
// entry per role, first match wins, anything not installed is skipped -- so
// this produces a short, sane dock on KDE or sway rather than an empty one.
std::vector<DesktopApp> load_dock_apps(const DockConfig& config,
                                       const std::map<std::string, DesktopCatalogEntry>& catalog) {
    const auto& favorites = config.pinned;
    const auto running_names = collect_running_process_names();

    std::vector<DesktopApp> apps;
    apps.reserve(favorites.size());

    for (const auto& desktop_id : favorites) {
        const auto it = catalog.find(desktop_id);
        if (it == catalog.end()) {
            continue;
        }

        const DesktopCatalogEntry& entry = it->second;
        const std::string app_id = std::filesystem::path(desktop_id).stem().string();
        const auto candidates = exec_candidates(entry.exec_line, desktop_id);

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
        });
    }

    return apps;
}

class MagnificationProfile {
  public:
    MagnificationProfile() { configure(DEFAULT_MAX_SCALE); }

    // The reference's width curve is BASE * {1, 1.1, 1.414, 2} across the
    // half-width of the falloff. Expressed as a fraction of the peak excursion
    // that is {0, 0.1, 0.414, 1}, which generalises to any peak scale and
    // reproduces the reference exactly at 2.0.
    void configure(double max_scale) {
        static const double kShape[4] = {0.0, 0.1, 0.414, 1.0};
        const double excursion = BASE_WIDTH * (max_scale - 1.0);

        distance_input_ = {
            -DISTANCE_LIMIT, -DISTANCE_LIMIT / 1.25, -DISTANCE_LIMIT / 2.0, 0.0,
            DISTANCE_LIMIT / 2.0, DISTANCE_LIMIT / 1.25, DISTANCE_LIMIT,
        };
        width_output_ = {
            BASE_WIDTH + kShape[0] * excursion,
            BASE_WIDTH + kShape[1] * excursion,
            BASE_WIDTH + kShape[2] * excursion,
            BASE_WIDTH + kShape[3] * excursion,
            BASE_WIDTH + kShape[2] * excursion,
            BASE_WIDTH + kShape[1] * excursion,
            BASE_WIDTH + kShape[0] * excursion,
        };
    }

    double interpolate_width(double distance) const {
        if (distance <= distance_input_.front() || distance >= distance_input_.back()) {
            return BASE_WIDTH;
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

        return BASE_WIDTH;
    }

  private:
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
        magnifier_.configure(config_.max_scale);
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
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_BOTTOM, BOTTOM_MARGIN);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window_), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        }
    #else
        g_warning("lucid-dock was built WITHOUT gtk4-layer-shell. The dock cannot "
                  "anchor to the screen edge and will float in the wrong place. "
                  "Rebuild with gtk4-layer-shell.");
    #endif

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
    double max_icon_width() const { return BASE_WIDTH * config_.max_scale; }
    int max_icon_px() const { return static_cast<int>(max_icon_width() + 0.5); }
    int icon_source_size() const { return max_icon_px() + 1; }
    int window_height() const {
        return PANEL_BG_HEIGHT + (max_icon_px() - BASE_ICON_PX) + BOTTOM_MARGIN + TOP_SLACK;
    }

    // Which item is under this x, in root_fixed_ coordinates. Uses the animated
    // width, so the label tracks the icon it belongs to as that icon grows.
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
        }
        gtk_widget_set_visible(tooltip_, TRUE);

        int natural_w = 0;
        int natural_h = 0;
        gtk_widget_measure(tooltip_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural_w,
                           nullptr, nullptr);
        gtk_widget_measure(tooltip_, GTK_ORIENTATION_VERTICAL, natural_w, nullptr, &natural_h,
                           nullptr, nullptr);

        const double centre = panel_x_ + icon.panel_x + icon.current_width / 2.0;
        double x = centre - natural_w / 2.0;
        // Keep it on screen for the first and last items rather than letting it
        // run off the edge.
        x = std::clamp(x, 0.0, std::max(0.0, static_cast<double>(win_w - natural_w)));

        const double y = icon_baseline_y_ - icon.current_width - TOOLTIP_GAP - natural_h;
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
        double tallest = BASE_ICON_PX;
        for (const auto& icon : icons_) {
            tallest = std::max(tallest, icon.current_width);
        }

        const double top = icon_baseline_y_ - tallest - PANEL_PADDING_Y;
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

    static void on_icon_clicked_static(GtkButton*, gpointer user_data) {
        auto* icon = static_cast<IconRuntime*>(user_data);
        icon->is_open = true;
        icon->bounce_started_at = monotonic_seconds();
        gtk_widget_set_visible(icon->dot, TRUE);

        if (!icon->desktop_path.empty()) {
            GDesktopAppInfo* app_info = g_desktop_app_info_new_from_filename(icon->desktop_path.c_str());
            if (app_info != nullptr) {
                GError* error = nullptr;
                if (!g_app_info_launch(G_APP_INFO(app_info), nullptr, nullptr, &error) && error != nullptr) {
                    g_warning("Failed to launch %s: %s", icon->title.c_str(), error->message);
                    g_error_free(error);
                }
                g_object_unref(app_info);
            }
        }
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

    // Advance the width spring by dt, using the closed-form solution of the
    // underdamped spring rather than a stepped integrator. That matters here:
    // a stepped integrator's behaviour depends on how often it is called, and
    // this dock demonstrably runs anywhere between 20 and 60 fps depending on
    // the GSK renderer, so a stepped spring would change feel with the renderer.
    // The closed form is exact at any dt and cannot go unstable at a low one.
    bool step_widths(double dt) {
        const auto targets = compute_item_widths();
        if (targets.size() != icons_.size()) {
            return false;
        }

        const double omega = SPRING_OMEGA * config_.tracking_speed;
        const double sigma = SPRING_ZETA * omega;                         // decay rate
        const double omega_d = omega * std::sqrt(1.0 - SPRING_ZETA * SPRING_ZETA);
        const double decay = std::exp(-sigma * dt);
        const double cos_wd = std::cos(omega_d * dt);
        const double sin_wd = std::sin(omega_d * dt);
        bool moving = false;

        for (std::size_t i = 0; i < icons_.size(); ++i) {
            auto& icon = icons_[i];
            icon.target_width = targets[i];

            // Displacement from the target, and the velocity that goes with it.
            const double a = icon.current_width - icon.target_width;
            const double v0 = icon.width_velocity;

            if (std::abs(a) <= WIDTH_SETTLE_EPSILON &&
                std::abs(v0) <= WIDTH_SETTLE_VELOCITY) {
                icon.current_width = icon.target_width;
                icon.width_velocity = 0.0;
                continue;
            }

            const double b = (v0 + sigma * a) / omega_d;
            icon.current_width = icon.target_width + decay * (a * cos_wd + b * sin_wd);
            icon.width_velocity =
                decay * ((omega_d * b - sigma * a) * cos_wd -
                         (omega_d * a + sigma * b) * sin_wd);
            moving = true;
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
        const double span = static_cast<double>(panel_content_width_ + 2 * PANEL_PADDING_X);
        current_mouse_x_ = panel_x_ + span * (0.5 - 0.5 * std::cos(phase * 2.0 * M_PI * 3.0));
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
        const double p50 = pct(bench_frame_us_, 0.50);
        if (p50 > 0.0) {
            g_print("effective fps    : %.1f (p50)\n", 1000.0 / p50);
        }
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
        animations_running = animations_running || widths_moving;

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
            icon.bounce_offset = -BOUNCE_HEIGHT * local;
        } else {
            const double local = sine_in_out((progress - 0.5) * 2.0);
            icon.bounce_offset = -BOUNCE_HEIGHT * (1.0 - local);
        }

        if (progress >= 1.0) {
            icon.bounce_started_at.reset();
            icon.bounce_offset = 0.0;
        }

        update_icon_internal_layout(icon);
        return icon.bounce_started_at.has_value();
    }

    // Targets only. The animated value is integrated separately in step_widths()
    // so that losing the pointer eases back to rest instead of snapping.
    std::vector<double> compute_item_widths() const {
        std::vector<double> widths(icons_.size(), BASE_WIDTH);
        if (!current_mouse_x_.has_value() || !config_.magnification) {
            return widths;
        }

        // No overhang allowance here any more. This used to admit pointers up to
        // DISTANCE_LIMIT (345 px) past either end of the panel, which is what
        // let the dock magnify for a cursor that was nowhere near it. Whether
        // the pointer counts at all is now pointer_is_on_panel()'s decision,
        // and it stops at the panel edge.
        const double pointer_x = *current_mouse_x_ - panel_x_;

        for (int pass = 0; pass < 2; ++pass) {
            double cursor = PANEL_PADDING_X;
            std::vector<double> next(widths.size());
            next.reserve(widths.size());

            for (std::size_t i = 0; i < widths.size(); ++i) {
                if (i > 0 && icons_[i].divider_before) {
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
            if (i > 0 && icons_[i].divider_before) {
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
                gtk_widget_set_size_request(panel_bg_, panel_width, PANEL_BG_HEIGHT);
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
            panel_bg_y_ = win_h - PANEL_BG_HEIGHT - BOTTOM_MARGIN;
            icon_baseline_y_ = panel_bg_y_ + PANEL_PADDING_Y + BASE_ICON_PX;
            panel_y_ = icon_baseline_y_ -
                       (PANEL_PADDING_Y + ITEM_SLOT_HEIGHT - ICON_BOTTOM_INSET);

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
                        panel_bg_y_, panel_bg_y_ + PANEL_BG_HEIGHT, PANEL_BG_HEIGHT);
                g_print("icon baseline     : y %d\n", icon_baseline_y_);
                g_print("idle icon top     : y %d  (inside the panel)\n",
                        icon_baseline_y_ - BASE_ICON_PX);
                g_print("magnified top     : y %d  (%d px above the panel, scale %.2f)\n",
                        icon_baseline_y_ - max_icon_px(),
                        panel_bg_y_ - (icon_baseline_y_ - max_icon_px()),
                        config_.max_scale);
                g_print("item row (no draw): y %d .. %d\n",
                        panel_y_, panel_y_ + panel_height);
                g_print("screen-edge gap   : %d px\n", win_h - (panel_bg_y_ + PANEL_BG_HEIGHT));
            }

            // Separators span the drawn panel's interior, not the item slot:
            // the slot is tall enough for a magnified icon, and a separator
            // that tall would stick out of the panel along with the icons.
            const int divider_y = (panel_bg_y_ - panel_y_) + PANEL_PADDING_Y;
            const int divider_height = PANEL_BG_HEIGHT - PANEL_PADDING_Y * 2;

            // A twentieth of a pixel is under the threshold of anything the
            // compositor can draw differently, and skipping those saves a
            // widget move per icon per frame.
            constexpr double kPositionEpsilon = 0.05;

            double cursor = PANEL_PADDING_X;
            for (std::size_t i = 0; i < icons_.size(); ++i) {
                auto& icon = icons_[i];

                if (i > 0 && icon.divider_before && icon.divider != nullptr) {
                    const double line_x = cursor + DIVIDER_MARGIN;
                    if (std::abs(icon.divider_x - line_x) > kPositionEpsilon) {
                        icon.divider_x = line_x;
                        gtk_widget_set_size_request(icon.divider, DIVIDER_WIDTH, divider_height);
                        gtk_fixed_move(GTK_FIXED(panel_fixed_), icon.divider,
                                       line_x, static_cast<double>(divider_y));
                    }
                    cursor += DIVIDER_SLOT;
                }

                if (std::abs(icon.panel_x - cursor) > kPositionEpsilon) {
                    icon.panel_x = cursor;
                    gtk_fixed_move(GTK_FIXED(panel_fixed_), icon.button, cursor,
                                   static_cast<double>(PANEL_PADDING_Y));
                }
                cursor += icon.current_width + ITEM_GAP;
            }

            last_applied_pixel_widths_ = std::move(pixel_widths);
        }

        for (auto& icon : icons_) {
            update_icon_internal_layout(icon);
        }

        update_tooltip(gtk_widget_get_width(window_));
    }

    void update_icon_internal_layout(IconRuntime& icon) {
        const int pixel_width = static_cast<int>(std::round(icon.current_width));
        const double base_y = static_cast<double>(ITEM_SLOT_HEIGHT - pixel_width - ICON_BOTTOM_INSET);
        const double image_y = std::max(0.0, base_y + icon.bounce_offset);
        const int dot_x = std::max(0, (pixel_width - DOT_SIZE) / 2);
        const int dot_y = ITEM_SLOT_HEIGHT - 12;

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
            GdkDisplay* display = window_ != nullptr ? gtk_widget_get_display(window_)
                                                     : gdk_display_get_default();
            if (display != nullptr) {
                GtkIconTheme* theme = gtk_icon_theme_get_for_display(display);
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

            if (!icon.icon_name.empty() && std::filesystem::is_regular_file(icon.icon_name)) {
                icon.resolved_file = icon.icon_name;
            } else {
                std::filesystem::path theme_file = find_icon_in_lucid_theme(icon.icon_name);
                if (theme_file.empty() && !icon.icon_name.empty()) {
                    const std::filesystem::path stem = std::filesystem::path(icon.icon_name).stem();
                    if (!stem.empty() && stem.string() != icon.icon_name) {
                        theme_file = find_icon_in_lucid_theme(stem.string());
                    }
                }

                if (!theme_file.empty()) {
                    icon.resolved_file = theme_file;
                } else {
                    icon.resolved_icon_name =
                        icon.icon_name.empty() ? "application-x-executable" : icon.icon_name;
                }
            }

            icon.image = gtk_image_new();
            apply_icon_paintable(icon);
            gtk_image_set_pixel_size(GTK_IMAGE(icon.image), static_cast<int>(std::round(BASE_WIDTH)));
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
            g_signal_connect(new_icon.button, "clicked", G_CALLBACK(&DockEngine::on_icon_clicked_static), &new_icon);
        }
    }

    void refresh_running_indicators() {
        const auto running = collect_running_process_names();
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

        magnifier_.configure(config_.max_scale);
        geom_logged_ = false;   // geometry may have moved; let it print again
        apply_window_size();

        if (items_changed) {
            rebuild_items();
        }
        queue_layout();
    }

    void rebuild_items() {
        for (auto& icon : icons_) {
            if (icon.divider != nullptr) {
                gtk_fixed_remove(GTK_FIXED(panel_fixed_), icon.divider);
            }
            gtk_fixed_remove(GTK_FIXED(panel_fixed_), icon.button);
        }
        icons_.clear();
        last_applied_pixel_widths_.clear();

        const auto catalog = load_desktop_catalog();
        build_icons(load_dock_apps(config_, catalog));
        last_icon_scale_ = 0;
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
    std::optional<std::size_t> hovered_index_;

    std::vector<IconRuntime> icons_;
    MagnificationProfile magnifier_;
    DockConfig config_;
    GtkWidget* menu_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;
    GFileMonitor* config_monitor_ = nullptr;
    std::vector<int> last_applied_pixel_widths_;

    std::optional<double> current_mouse_x_;

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
};

void on_activate(GtkApplication* app, gpointer) {
    static DockEngine engine;
    const auto catalog = load_desktop_catalog();
    const DockConfig config = ensure_config(catalog);
    engine.build_ui(app, config, load_dock_apps(config, catalog));
}

}  // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("dev.lucidos.DockCpp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
