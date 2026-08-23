#include <gtk/gtk.h>

#if defined(HAVE_GTK4_LAYER_SHELL)
#if !__has_include(<gtk4-layer-shell.h>)
#error "HAVE_GTK4_LAYER_SHELL is defined but <gtk4-layer-shell.h> is missing. Install gtk4-layer-shell (NOT libgtk-layer-shell-dev, which is the GTK3 build)."
#endif
#include <gtk4-layer-shell.h>
#endif

#include <gio/gdesktopappinfo.h>

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

constexpr double BASE_WIDTH = 57.6;
constexpr double MAX_WIDTH = BASE_WIDTH * 2.0;
constexpr double DISTANCE_LIMIT = BASE_WIDTH * 6.0;

constexpr int PANEL_PADDING_X = 10;
constexpr int PANEL_PADDING_Y = 8;
constexpr int ITEM_GAP = 10;
constexpr int ITEM_SLOT_HEIGHT = 128;
constexpr int DOT_SIZE = 4;
constexpr int BOTTOM_MARGIN = 8;
constexpr int WINDOW_HEIGHT = 160;

// Icons are rasterised once at the largest size magnification can reach, then
// scaled DOWN as they shrink. Loading at BASE_WIDTH and scaling up was the
// cause of soft icons under magnification -- and, multiplied by the display
// scale factor, of soft icons on HiDPI.
constexpr int ICON_SOURCE_SIZE = static_cast<int>(MAX_WIDTH) + 1;

// Time constant for magnification easing. current_width converges on
// target_width exponentially, which is frame-rate independent and never
// overshoots. ~55 ms reads as responsive while tracking and still gives a
// visible settle when the pointer leaves -- the shrink animation that a
// direct pointer-to-width mapping cannot produce.
constexpr double MAGNIFY_TAU = 0.055;
// Releasing uses a slower time constant than tracking. Following the pointer
// wants to feel immediate; letting go wants to feel like the dock settles.
// One tau for both made the shrink read as abrupt.
constexpr double RELEASE_TAU = 0.135;
constexpr double WIDTH_SETTLE_EPSILON = 0.25;

constexpr double BOUNCE_HEIGHT = 40.0;
constexpr double BOUNCE_DURATION = 0.4;

struct DesktopCatalogEntry {
    std::filesystem::path desktop_path;
    std::string title;
    std::string icon_name;
    std::string exec_line;
};

struct DesktopApp {
    std::string app_id;
    std::string title;
    std::filesystem::path desktop_path;
    std::string icon_name;
    std::string exec_line;
    std::vector<std::string> process_candidates;
    bool is_open = false;
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
    double target_width = BASE_WIDTH;    // where the pointer says it should be
    double bounce_offset = 0.0;
    std::optional<double> bounce_started_at;

    double last_image_y = -1.0;
    int last_dot_x = -1;
    int last_dot_y = -1;
    int panel_x = -1;
    bool is_open = false;

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

std::optional<std::string> load_keyfile_value(GKeyFile* key_file, const char* key) {
    GError* error = nullptr;
    gchar* value = g_key_file_get_string(key_file, "Desktop Entry", key, &error);
    if (error != nullptr) {
        g_error_free(error);
        return std::nullopt;
    }
    std::string result = value != nullptr ? value : "";
    g_free(value);
    return result;
}

bool load_desktop_info(const std::filesystem::path& desktop_path, DesktopCatalogEntry& entry) {
    GKeyFile* key_file = g_key_file_new();
    GError* error = nullptr;
    if (!g_key_file_load_from_file(key_file, desktop_path.c_str(), G_KEY_FILE_NONE, &error)) {
        if (error != nullptr) {
            g_error_free(error);
        }
        g_key_file_unref(key_file);
        return false;
    }

    GError* type_error = nullptr;
    gchar* type_value = g_key_file_get_string(key_file, "Desktop Entry", "Type", &type_error);
    const bool is_application = type_value != nullptr && std::string(type_value) == "Application";
    g_free(type_value);
    if (type_error != nullptr) {
        g_error_free(type_error);
    }
    if (!is_application) {
        g_key_file_unref(key_file);
        return false;
    }

    GError* hidden_error = nullptr;
    const gboolean nodisplay = g_key_file_get_boolean(key_file, "Desktop Entry", "NoDisplay", &hidden_error);
    if (hidden_error != nullptr) {
        g_error_free(hidden_error);
    }
    if (nodisplay) {
        g_key_file_unref(key_file);
        return false;
    }

    const auto title = load_keyfile_value(key_file, "Name");
    const auto icon_name = load_keyfile_value(key_file, "Icon");
    const auto exec_line = load_keyfile_value(key_file, "Exec");

    entry.desktop_path = desktop_path;
    entry.title = title.value_or(desktop_path.stem().string());
    entry.icon_name = icon_name.value_or("");
    entry.exec_line = exec_line.value_or("");

    g_key_file_unref(key_file);
    return true;
}

void scan_desktop_directory(const std::filesystem::path& directory, std::map<std::string, DesktopCatalogEntry>& catalog) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".desktop") {
            continue;
        }

        DesktopCatalogEntry desktop_entry;
        if (!load_desktop_info(entry.path(), desktop_entry)) {
            continue;
        }

        catalog[entry.path().filename().string()] = std::move(desktop_entry);
    }
}

std::map<std::string, DesktopCatalogEntry> load_desktop_catalog() {
    std::map<std::string, DesktopCatalogEntry> catalog;

    const char* user_data_dir = g_get_user_data_dir();
    if (user_data_dir != nullptr) {
        scan_desktop_directory(std::filesystem::path(user_data_dir) / "applications", catalog);
    }

    const char* const* system_data_dirs = g_get_system_data_dirs();
    if (system_data_dirs != nullptr) {
        for (const char* const* it = system_data_dirs; *it != nullptr; ++it) {
            scan_desktop_directory(std::filesystem::path(*it) / "applications", catalog);
        }
    }

    scan_desktop_directory(std::filesystem::path("/usr/share/applications"), catalog);
    return catalog;
}

std::vector<std::string> get_gnome_favorite_desktop_ids() {
    std::vector<std::string> favorites;

    GSettings* settings = g_settings_new("org.gnome.shell");
    if (settings == nullptr) {
        return favorites;
    }

    gchar** values = g_settings_get_strv(settings, "favorite-apps");
    if (values != nullptr) {
        for (gchar** it = values; *it != nullptr; ++it) {
            std::string desktop_id = *it;
            if (desktop_id.size() >= 8 && desktop_id.rfind(".desktop") == desktop_id.size() - 8) {
                favorites.push_back(std::move(desktop_id));
            }
        }
        g_strfreev(values);
    }

    g_object_unref(settings);
    return favorites;
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

std::vector<DesktopApp> load_system_dock_apps() {
    const auto catalog = load_desktop_catalog();
    const auto favorites = get_gnome_favorite_desktop_ids();
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

        apps.push_back(DesktopApp{
            .app_id = app_id,
            .title = entry.title,
            .desktop_path = entry.desktop_path,
            .icon_name = entry.icon_name,
            .exec_line = entry.exec_line,
            .process_candidates = candidates,
            .is_open = is_open,
        });
    }

    return apps;
}

class MagnificationProfile {
  public:
    MagnificationProfile()
        : distance_input_{
              -DISTANCE_LIMIT,
              -DISTANCE_LIMIT / 1.25,
              -DISTANCE_LIMIT / 2.0,
              0.0,
              DISTANCE_LIMIT / 2.0,
              DISTANCE_LIMIT / 1.25,
              DISTANCE_LIMIT,
          },
          width_output_{
              BASE_WIDTH,
              BASE_WIDTH * 1.1,
              BASE_WIDTH * 1.414,
              MAX_WIDTH,
              BASE_WIDTH * 1.414,
              BASE_WIDTH * 1.1,
              BASE_WIDTH,
          } {}

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

        void build_ui(GtkApplication* app, const std::vector<DesktopApp>& dock_apps) {
        window_ = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(window_), "Lucid Dock C++");
        gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
        // Pin the toplevel to the full monitor width. Without this the window
        // auto-sizes to the dock panel, and because the panel resizes on every
        // magnification frame the toplevel renegotiates its surface size with
        // the compositor every frame -- a round-trip per frame, which halves
        // the frame rate. Under layer-shell the compositor owns the size and
        // this cannot happen; this is the fallback path doing it by hand.
        int pinned_width = 1920;
        if (GdkDisplay* d = gdk_display_get_default()) {
            if (GListModel* monitors = gdk_display_get_monitors(d)) {
                if (auto* m = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, 0))) {
                    GdkRectangle geom;
                    gdk_monitor_get_geometry(m, &geom);
                    if (geom.width > 0) {
                        pinned_width = geom.width;
                    }
                    g_object_unref(m);
                }
            }
        }
        gtk_widget_set_size_request(window_, pinned_width, WINDOW_HEIGHT);
        // Width is not ours to choose: anchored left+right, the compositor gives
        // us the output width. -1 means "no opinion" rather than a guess that is
        // wrong on every display that is not 1280 logical pixels wide.
        gtk_window_set_default_size(GTK_WINDOW(window_), -1, WINDOW_HEIGHT);
        g_signal_connect(window_, "notify::scale-factor",
                         G_CALLBACK(&DockEngine::on_scale_changed_static), this);
        g_signal_connect(window_, "realize",
                         G_CALLBACK(&DockEngine::on_window_realize_static), this);

    #if defined(HAVE_GTK4_LAYER_SHELL)
        gtk_layer_init_for_window(GTK_WINDOW(window_));
        gtk_layer_set_layer(GTK_WINDOW(window_), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(GTK_WINDOW(window_), GTK_LAYER_SHELL_EDGE_BOTTOM, BOTTOM_MARGIN);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window_), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    #else
        g_warning("lucid-dock was built WITHOUT gtk4-layer-shell. The dock cannot "
                  "anchor to the screen edge and will float in the wrong place. "
                  "Rebuild with gtk4-layer-shell.");
    #endif

        install_css();

        root_fixed_ = gtk_fixed_new();
        gtk_widget_add_css_class(root_fixed_, "dock-root");
        gtk_window_set_child(GTK_WINDOW(window_), root_fixed_);

        panel_fixed_ = gtk_fixed_new();
        gtk_widget_add_css_class(panel_fixed_, "dock-panel");
        gtk_fixed_put(GTK_FIXED(root_fixed_), panel_fixed_, 0.0, 0.0);

        build_icons(dock_apps);

        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion", G_CALLBACK(&DockEngine::on_motion_static), this);
        g_signal_connect(motion, "leave", G_CALLBACK(&DockEngine::on_leave_static), this);
        gtk_widget_add_controller(root_fixed_, motion);

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

    static void on_motion_static(GtkEventControllerMotion*, double x, double, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        // No motion throttle any more. A motion event now only updates a
        // target; the tick callback does the work at frame rate. Throttling
        // here was what made pointer tracking feel steppy.
        self->current_mouse_x_ = x;
        self->layout_dirty_ = true;
        self->ensure_tick();
    }

    static void on_leave_static(GtkEventControllerMotion*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->current_mouse_x_.reset();
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

    // Exponential convergence toward target_width. dt-based, so the animation
    // runs at the same speed at 60 Hz and 120 Hz. Returns true while any icon
    // is still moving.
    bool step_widths(double dt) {
        const auto targets = compute_item_widths();
        if (targets.size() != icons_.size()) {
            return false;
        }

        const double tau = current_mouse_x_.has_value() ? MAGNIFY_TAU : RELEASE_TAU;
        const double alpha = 1.0 - std::exp(-dt / tau);
        bool moving = false;

        for (std::size_t i = 0; i < icons_.size(); ++i) {
            auto& icon = icons_[i];
            icon.target_width = targets[i];
            const double delta = icon.target_width - icon.current_width;

            if (std::abs(delta) <= WIDTH_SETTLE_EPSILON) {
                icon.current_width = icon.target_width;
            } else {
                icon.current_width += delta * alpha;
                moving = true;
            }
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
        if (!current_mouse_x_.has_value()) {
            return widths;
        }

        const double pointer_x = *current_mouse_x_ - panel_x_;
        if (pointer_x < -DISTANCE_LIMIT || pointer_x > panel_content_width_ + DISTANCE_LIMIT) {
            return widths;
        }

        for (int pass = 0; pass < 2; ++pass) {
            double cursor = PANEL_PADDING_X;
            std::vector<double> next(widths.size());
            next.reserve(widths.size());

            for (std::size_t i = 0; i < widths.size(); ++i) {
                const double center_x = cursor + widths[i] / 2.0;
                next[i] = magnifier_.interpolate_width(pointer_x - center_x);
                cursor += widths[i] + ITEM_GAP;
            }
            widths = std::move(next);
        }

        return widths;
    }

    void layout_panel() {
        std::vector<int> pixel_widths;
        pixel_widths.reserve(icons_.size());

        for (const auto& icon : icons_) {
            pixel_widths.push_back(std::max(1, static_cast<int>(std::round(icon.current_width))));
        }

        const bool widths_changed = pixel_widths != last_applied_pixel_widths_;

        if (widths_changed) {
            int panel_width = PANEL_PADDING_X * 2;
            for (std::size_t i = 0; i < icons_.size(); ++i) {
                const int pixel_width = pixel_widths[i];
                gtk_widget_set_size_request(icons_[i].button, pixel_width, ITEM_SLOT_HEIGHT);
                gtk_image_set_pixel_size(GTK_IMAGE(icons_[i].image), pixel_width);
                panel_width += pixel_width;
                if (i + 1 < icons_.size()) {
                    panel_width += ITEM_GAP;
                }
            }

            panel_content_width_ = std::max(0, panel_width - PANEL_PADDING_X * 2);

            const int panel_height = ITEM_SLOT_HEIGHT + PANEL_PADDING_Y * 2;
            gtk_widget_set_size_request(panel_fixed_, panel_width, panel_height);

            // Centre against the width we actually have. The old code took
            // max(actual, 1280), so on any display narrower than 1280 logical
            // pixels the dock centred itself against a window that did not
            // exist and items landed off-screen.
            const int win_w = gtk_widget_get_width(window_);
            const int win_h = std::max(gtk_widget_get_height(window_), WINDOW_HEIGHT);

            if (win_w <= 0) {
                // Not allocated yet. Re-run once the compositor has sized us
                // rather than laying out against a width of zero.
                layout_dirty_ = true;
                ensure_tick();
                return;
            }

            panel_x_ = std::max(0, (win_w - panel_width) / 2);
            panel_y_ = win_h - panel_height - BOTTOM_MARGIN;
            gtk_fixed_move(GTK_FIXED(root_fixed_), panel_fixed_, static_cast<double>(panel_x_), static_cast<double>(panel_y_));

            int cursor = PANEL_PADDING_X;
            for (std::size_t i = 0; i < icons_.size(); ++i) {
                auto& icon = icons_[i];
                if (icon.panel_x != cursor) {
                    icon.panel_x = cursor;
                    gtk_fixed_move(GTK_FIXED(panel_fixed_), icon.button, static_cast<double>(cursor), static_cast<double>(PANEL_PADDING_Y));
                }
                cursor += pixel_widths[i] + ITEM_GAP;
            }

            last_applied_pixel_widths_ = std::move(pixel_widths);
        }

        for (auto& icon : icons_) {
            update_icon_internal_layout(icon);
        }
    }

    void update_icon_internal_layout(IconRuntime& icon) {
        const int pixel_width = static_cast<int>(std::round(icon.current_width));
        const double base_y = static_cast<double>(ITEM_SLOT_HEIGHT - pixel_width - 13);
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

    // Rasterise at ICON_SOURCE_SIZE x scale so every magnification step and
    // every display scale is a downscale rather than an upscale.
    void apply_icon_paintable(IconRuntime& icon) {
        const int scale = current_scale();
        GtkIconPaintable* paintable = nullptr;

        if (!icon.resolved_file.empty()) {
            GFile* file = g_file_new_for_path(icon.resolved_file.c_str());
            paintable = gtk_icon_paintable_new_for_file(file, ICON_SOURCE_SIZE, scale);
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
                    ICON_SOURCE_SIZE,
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

            icon.button = gtk_button_new();
            gtk_widget_add_css_class(icon.button, "dock-item");
            gtk_widget_set_can_focus(icon.button, FALSE);
            gtk_widget_set_tooltip_text(icon.button, icon.title.c_str());

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

    void install_css() {
        static const char* kCss = R"CSS(
window {
    background: transparent;
}

.dock-root {
    background: transparent;
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
    GtkWidget* panel_fixed_ = nullptr;

    std::vector<IconRuntime> icons_;
    MagnificationProfile magnifier_;
    std::vector<int> last_applied_pixel_widths_;

    std::optional<double> current_mouse_x_;

    guint tick_id_ = 0;
    guint running_refresh_id_ = 0;
    bool layout_dirty_ = false;
    int last_icon_scale_ = 0;
    double last_tick_at_ = 0.0;

    int bench_total_ = 0;
    int bench_remaining_ = 0;
    gint64 bench_last_frame_us_ = 0;
    std::vector<gint64> bench_frame_us_;
    std::vector<gint64> bench_layout_us_;

    int panel_x_ = 0;
    int panel_y_ = 0;
    int panel_content_width_ = 0;
};

void on_activate(GtkApplication* app, gpointer) {
    static DockEngine engine;
    const auto dock_apps = load_system_dock_apps();
    engine.build_ui(app, dock_apps);
}

}  // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("dev.lucidos.DockCpp", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
