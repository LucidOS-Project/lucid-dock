#include <gtk/gtk.h>

#if defined(HAVE_GTK4_LAYER_SHELL) && __has_include(<gtk4-layer-shell.h>)
#include <gtk4-layer-shell.h>
#endif

#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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
constexpr int WINDOW_WIDTH_FALLBACK = 1280;

constexpr double BOUNCE_HEIGHT = 40.0;
constexpr double BOUNCE_DURATION = 0.4;
constexpr double HOVER_MOTION_EPSILON = 1.25;
constexpr double HOVER_UPDATE_MIN_INTERVAL = 1.0 / 90.0;

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

    double current_width = BASE_WIDTH;
    double bounce_offset = 0.0;
    std::optional<double> bounce_started_at;

    int last_image_y = -1;
    int last_dot_x = -1;
    int last_dot_y = -1;
    int panel_x = -1;
    bool is_open = false;
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
        gtk_widget_set_size_request(window_, -1, WINDOW_HEIGHT);
        gtk_window_set_default_size(GTK_WINDOW(window_), WINDOW_WIDTH_FALLBACK, WINDOW_HEIGHT);

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
                  "anchor to the screen edge and will appear as a floating %dx%d "
                  "window in the wrong place. Rebuild with gtk4-layer-shell.",
                  WINDOW_WIDTH_FALLBACK, WINDOW_HEIGHT);
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

        gtk_window_present(GTK_WINDOW(window_));
    }

  private:
    static void on_map_static(GtkWidget*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        self->queue_layout();
    }

    static void on_motion_static(GtkEventControllerMotion*, double x, double, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        const double now = monotonic_seconds();
        if (self->current_mouse_x_.has_value() && std::abs(x - *self->current_mouse_x_) < HOVER_MOTION_EPSILON) {
            if ((now - self->last_hover_layout_at_) < HOVER_UPDATE_MIN_INTERVAL) {
                return;
            }
        }

        self->current_mouse_x_ = x;
        self->last_hover_layout_at_ = now;
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

    static gboolean on_tick_static(GtkWidget*, GdkFrameClock*, gpointer user_data) {
        auto* self = static_cast<DockEngine*>(user_data);
        return self->on_tick();
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

    gboolean on_tick() {
        const double now = monotonic_seconds();
        bool animations_running = false;

        for (auto& icon : icons_) {
            animations_running = update_bounce(icon, now) || animations_running;
        }

        if (layout_dirty_ || animations_running) {
            layout_panel();
            layout_dirty_ = false;
        }

        if (animations_running || layout_dirty_) {
            return G_SOURCE_CONTINUE;
        }

        tick_id_ = 0;
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
        auto widths = compute_item_widths();
        std::vector<int> pixel_widths;
        pixel_widths.reserve(widths.size());

        for (double width : widths) {
            pixel_widths.push_back(std::max(1, static_cast<int>(std::round(width))));
        }

        const bool widths_changed = pixel_widths != last_applied_pixel_widths_;

        if (widths_changed) {
            int panel_width = PANEL_PADDING_X * 2;
            for (std::size_t i = 0; i < icons_.size(); ++i) {
                const int pixel_width = pixel_widths[i];
                icons_[i].current_width = static_cast<double>(pixel_width);
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

            const int win_w = std::max(gtk_widget_get_width(window_), WINDOW_WIDTH_FALLBACK);
            const int win_h = std::max(gtk_widget_get_height(window_), WINDOW_HEIGHT);

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
        const int base_y = ITEM_SLOT_HEIGHT - pixel_width - 13;
        const int image_y = std::max(0, base_y + static_cast<int>(std::round(icon.bounce_offset)));
        const int dot_x = std::max(0, (pixel_width - DOT_SIZE) / 2);
        const int dot_y = ITEM_SLOT_HEIGHT - 12;

        if (icon.last_image_y != image_y) {
            icon.last_image_y = image_y;
            gtk_fixed_move(GTK_FIXED(icon.fixed), icon.image, 0.0, static_cast<double>(image_y));
        }
        if (icon.last_dot_x != dot_x || icon.last_dot_y != dot_y) {
            icon.last_dot_x = dot_x;
            icon.last_dot_y = dot_y;
            gtk_fixed_move(GTK_FIXED(icon.fixed), icon.dot, static_cast<double>(dot_x), static_cast<double>(dot_y));
        }
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

            GtkImage* image = nullptr;
            if (!icon.icon_name.empty() && std::filesystem::is_regular_file(icon.icon_name)) {
                image = GTK_IMAGE(gtk_image_new_from_file(icon.icon_name.c_str()));
            } else {
                std::filesystem::path theme_file = find_icon_in_lucid_theme(icon.icon_name);
                if (theme_file.empty() && !icon.icon_name.empty()) {
                    const std::filesystem::path stem = std::filesystem::path(icon.icon_name).stem();
                    if (!stem.empty() && stem.string() != icon.icon_name) {
                        theme_file = find_icon_in_lucid_theme(stem.string());
                    }
                }

                if (!theme_file.empty()) {
                    image = GTK_IMAGE(gtk_image_new_from_file(theme_file.c_str()));
                } else if (!icon.icon_name.empty()) {
                    image = GTK_IMAGE(gtk_image_new_from_icon_name(icon.icon_name.c_str()));
                } else {
                    image = GTK_IMAGE(gtk_image_new_from_icon_name("application-x-executable"));
                }
            }

            icon.image = GTK_WIDGET(image);
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

.dock-item,
.dock-item:hover,
.dock-item:active,
.dock-item:focus-visible {
    padding: 0;
    margin: 0;
    border: none;
    background: transparent;
    box-shadow: none;
    outline: none;
}

.dock-dot {
    border-radius: 999px;
    background: rgba(20, 20, 20, 0.85);
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
    double last_hover_layout_at_ = 0.0;

    guint tick_id_ = 0;
    guint running_refresh_id_ = 0;
    bool layout_dirty_ = false;

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
