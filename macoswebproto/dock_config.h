// Shared between lucid-dock and lucid-dock-settings.
//
// Header-only on purpose: the shared surface is a config struct, a .desktop
// catalogue and the functions that read and write them. That is small enough
// that a static library would be more build system than it is worth, and both
// binaries want exactly the same code with no ABI between them.

#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace lucid {

struct DesktopCatalogEntry {
    std::filesystem::path desktop_path;
    std::string title;
    std::string icon_name;
    std::string exec_line;
};


inline std::optional<std::string> load_keyfile_value(GKeyFile* key_file, const char* key) {
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

inline bool load_desktop_info(const std::filesystem::path& desktop_path, DesktopCatalogEntry& entry) {
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

inline void scan_desktop_directory(const std::filesystem::path& directory, std::map<std::string, DesktopCatalogEntry>& catalog) {
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

inline std::map<std::string, DesktopCatalogEntry> load_desktop_catalog() {
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

// ---------------------------------------------------------------------------
// Configuration
//
// GKeyFile rather than TOML or JSON: it needs no dependency GLib does not
// already provide, and it is the syntax .desktop files are already written in,
// so it is the one config format a Linux desktop component can assume is
// already understood. ~/.config/lucid/ is shared with the other LucidOS
// components so a settings app has a single directory to look in.
// ---------------------------------------------------------------------------

constexpr const char* kConfigGroup = "Dock";

struct DockConfig {
    std::vector<std::string> pinned;
    std::vector<std::string> dividers_before;
    bool magnification = true;
    // Peak magnification, as a multiple of the icon size. The reference uses 2.
    double max_scale = 2.0;
    // How far the magnification reaches, in icon widths either side of the
    // pointer. This is what decides how much the icon under the pointer stands
    // out from its neighbours: a small spread lifts almost only the hovered
    // icon, a large one raises a broad gentle hill. The reference uses 6.
    double spread = 6.0;

    // Parsed, round-tripped and written into a fresh config file so the keys
    // are discoverable, but not acted on yet. A settings UI needs somewhere to
    // put these before the dock can honour them.
    std::string layout_mode = "dock";   // dock | taskbar
    std::string position = "bottom";    // bottom | left | right | top
    int icon_size = 0;                  // 0 = the built-in 57.6 px
};

inline std::filesystem::path config_dir() {
    const char* base = g_get_user_config_dir();
    return std::filesystem::path(base != nullptr ? base : "") / "lucid";
}

inline std::filesystem::path config_file_path() {
    return config_dir() / "dock.conf";
}

inline std::vector<std::string> key_string_list(GKeyFile* keyfile, const char* key) {
    std::vector<std::string> out;
    gsize count = 0;
    gchar** values = g_key_file_get_string_list(keyfile, kConfigGroup, key, &count, nullptr);
    if (values != nullptr) {
        for (gsize i = 0; i < count; ++i) {
            if (values[i] != nullptr && *values[i] != '\0') {
                out.emplace_back(values[i]);
            }
        }
        g_strfreev(values);
    }
    return out;
}

inline bool read_config(DockConfig& config) {
    GKeyFile* keyfile = g_key_file_new();
    const std::string path = config_file_path().string();

    if (!g_key_file_load_from_file(keyfile, path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr)) {
        g_key_file_free(keyfile);
        return false;
    }

    config.pinned = key_string_list(keyfile, "Pinned");
    config.dividers_before = key_string_list(keyfile, "DividersBefore");

    GError* error = nullptr;
    const gboolean magnification =
        g_key_file_get_boolean(keyfile, kConfigGroup, "Magnification", &error);
    if (error == nullptr) {
        config.magnification = magnification != FALSE;
    } else {
        g_clear_error(&error);
    }

    for (const auto& [key, target, lo, hi] : {
             std::tuple<const char*, double*, double, double>{"MaxScale", &config.max_scale, 1.0, 3.0},
             std::tuple<const char*, double*, double, double>{"Spread", &config.spread, 1.5, 8.0}}) {
        const double value = g_key_file_get_double(keyfile, kConfigGroup, key, &error);
        if (error == nullptr) {
            *target = std::clamp(value, lo, hi);
        } else {
            g_clear_error(&error);
        }
    }

    const int icon_size = g_key_file_get_integer(keyfile, kConfigGroup, "IconSize", &error);
    if (error == nullptr) {
        config.icon_size = icon_size;
    } else {
        g_clear_error(&error);
    }

    for (const auto& [key, target] : {std::pair<const char*, std::string*>{"LayoutMode", &config.layout_mode},
                                      std::pair<const char*, std::string*>{"Position", &config.position}}) {
        gchar* value = g_key_file_get_string(keyfile, kConfigGroup, key, nullptr);
        if (value != nullptr) {
            *target = value;
            g_free(value);
        }
    }

    g_key_file_free(keyfile);
    return true;
}

inline void set_key_string_list(GKeyFile* keyfile, const char* key, const std::vector<std::string>& values) {
    std::vector<const gchar*> raw;
    raw.reserve(values.size());
    for (const auto& value : values) {
        raw.push_back(value.c_str());
    }
    g_key_file_set_string_list(keyfile, kConfigGroup, key, raw.data(), raw.size());
}

inline bool write_config(const DockConfig& config) {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    if (ec) {
        g_warning("Could not create %s: %s", config_dir().c_str(), ec.message().c_str());
        return false;
    }

    GKeyFile* keyfile = g_key_file_new();
    set_key_string_list(keyfile, "Pinned", config.pinned);
    set_key_string_list(keyfile, "DividersBefore", config.dividers_before);
    g_key_file_set_boolean(keyfile, kConfigGroup, "Magnification", config.magnification ? TRUE : FALSE);
    g_key_file_set_double(keyfile, kConfigGroup, "MaxScale", config.max_scale);
    g_key_file_set_double(keyfile, kConfigGroup, "Spread", config.spread);
    g_key_file_set_integer(keyfile, kConfigGroup, "IconSize", config.icon_size);
    g_key_file_set_string(keyfile, kConfigGroup, "LayoutMode", config.layout_mode.c_str());
    g_key_file_set_string(keyfile, kConfigGroup, "Position", config.position.c_str());

    g_key_file_set_comment(keyfile, kConfigGroup, "Pinned",
        " Desktop file IDs, in the order they appear on the dock.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "DividersBefore",
        " Draw a separator immediately before each of these.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "MaxScale",
        " Peak magnification, 1.0 to 3.0. The reference dock uses 2.0.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "Spread",
        " How far magnification reaches, in icon widths, 1.5 to 8.0. Lower\n"
        " makes the icon under the pointer stand out more from its neighbours.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "IconSize",
        " Idle icon size in pixels, 24 to 80. 0 means the default of 58.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "LayoutMode",
        " dock | taskbar. NOT YET HONOURED -- reserved.", nullptr);
    g_key_file_set_comment(keyfile, kConfigGroup, "Position",
        " bottom | left | right | top. NOT YET HONOURED -- reserved.", nullptr);
    g_key_file_set_comment(keyfile, nullptr, nullptr,
        " lucid-dock configuration. Edited live: saving this file re-reads it.", nullptr);

    GError* error = nullptr;
    const std::string path = config_file_path().string();
    const gboolean ok = g_key_file_save_to_file(keyfile, path.c_str(), &error);
    if (!ok && error != nullptr) {
        g_warning("Could not write %s: %s", path.c_str(), error->message);
        g_error_free(error);
    }

    g_key_file_free(keyfile);
    return ok != FALSE;
}

// Is a GSettings schema actually installed? g_settings_new() on a missing
// schema is a g_error(), which aborts the process -- so this is not a style
// preference, it is the difference between running and not. The dock used to
// call g_settings_new("org.gnome.shell") unguarded, which meant it died on
// launch on KDE, sway, Hyprland and COSMIC: every compositor where layer-shell
// actually works.
inline bool gsettings_schema_installed(const char* schema_id) {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    if (source == nullptr) {
        return false;
    }

    GSettingsSchema* schema = g_settings_schema_source_lookup(source, schema_id, TRUE);
    if (schema == nullptr) {
        return false;
    }

    g_settings_schema_unref(schema);
    return true;
}

inline std::vector<std::string> get_gnome_favorite_desktop_ids() {
    std::vector<std::string> favorites;

    if (!gsettings_schema_installed("org.gnome.shell")) {
        return favorites;
    }

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


inline std::vector<std::string> builtin_default_pinned(
    const std::map<std::string, DesktopCatalogEntry>& catalog) {
    static const std::vector<std::vector<std::string>> kRoles = {
        {"org.gnome.Nautilus.desktop", "nautilus.desktop", "org.kde.dolphin.desktop",
         "thunar.desktop", "nemo.desktop", "pcmanfm.desktop"},
        {"firefox.desktop", "firefox_firefox.desktop", "org.mozilla.firefox.desktop",
         "chromium.desktop", "chromium-browser.desktop", "google-chrome.desktop"},
        {"org.gnome.Terminal.desktop", "org.gnome.Console.desktop", "org.kde.konsole.desktop",
         "alacritty.desktop", "kitty.desktop", "xterm.desktop"},
        {"org.gnome.TextEditor.desktop", "gedit.desktop", "org.kde.kate.desktop",
         "code.desktop"},
        {"org.gnome.Software.desktop", "org.kde.discover.desktop", "snap-store.desktop"},
        {"org.gnome.Settings.desktop", "gnome-control-center.desktop", "systemsettings.desktop"},
    };

    std::vector<std::string> pinned;
    for (const auto& role : kRoles) {
        for (const auto& candidate : role) {
            if (catalog.find(candidate) != catalog.end()) {
                pinned.push_back(candidate);
                break;
            }
        }
    }
    return pinned;
}

// Read the config, creating it on first run. Seeded from GNOME's favourites
// when that schema exists -- so the dock keeps looking the way it did on a
// GNOME box -- and from builtin_default_pinned() when it does not. Either way
// the result is written to disk, so from the second run on there is exactly
// one source of truth and GNOME is no longer consulted at all.
inline DockConfig ensure_config(const std::map<std::string, DesktopCatalogEntry>& catalog) {
    DockConfig config;
    if (read_config(config)) {
        return config;
    }

    config.pinned = get_gnome_favorite_desktop_ids();
    const char* source = "GNOME favourites";
    if (config.pinned.empty()) {
        config.pinned = builtin_default_pinned(catalog);
        source = "installed-application defaults";
    }

    // Drop anything that is not actually installed, so a first run never
    // produces slots for applications that cannot launch.
    std::vector<std::string> present;
    for (const auto& desktop_id : config.pinned) {
        if (catalog.find(desktop_id) != catalog.end()) {
            present.push_back(desktop_id);
        }
    }
    config.pinned = std::move(present);

    if (write_config(config)) {
        g_message("Created %s from %s (%zu applications).",
                  config_file_path().c_str(), source, config.pinned.size());
    }
    return config;
}


}  // namespace lucid
