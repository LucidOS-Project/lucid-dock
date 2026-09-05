#include "dock_settings_page.h"

#include "dock_config.h"
#include "lucid/tokens.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using lucid::DesktopCatalogEntry;
using lucid::DockConfig;

namespace {

// Everything the page needs, owned by the page widget and freed with it.
struct SettingsPage {
    DockConfig config;
    std::map<std::string, DesktopCatalogEntry> catalog;

    GtkWidget* root = nullptr;
    GtkWidget* app_list = nullptr;
    GtkWidget* add_button = nullptr;

    // Held so "Reset to Defaults" can put the displayed values back. Without
    // these the config would change underneath controls still showing the old
    // numbers.
    GtkWidget* magnify_switch = nullptr;

    // The token-backed controls. A row knows its key, so provenance and reset
    // are the same code for every setting rather than three copies.
    struct TokenRow {
        std::string key;
        GtkWidget* scale = nullptr;
        GtkWidget* provenance = nullptr;   // "user", "theme", ... hidden at default
        GtkWidget* reset = nullptr;        // resets this one key
    };
    std::vector<TokenRow> token_rows;
    std::unique_ptr<lucid::Config> tokens;

    // Set while a control is being populated from the config, so that
    // programmatic changes do not write the file back and fight the user.
    bool loading = false;
};

void save(SettingsPage* page) {
    if (!page->loading) {
        lucid::write_config(page->config);
    }
}

std::string title_for(SettingsPage* page, const std::string& desktop_id) {
    const auto it = page->catalog.find(desktop_id);
    if (it != page->catalog.end() && !it->second.title.empty()) {
        return it->second.title;
    }
    return desktop_id;
}

GtkWidget* icon_for(SettingsPage* page, const std::string& desktop_id, int size) {
    const auto it = page->catalog.find(desktop_id);
    if (it == page->catalog.end() || it->second.icon_name.empty()) {
        return gtk_image_new_from_icon_name("application-x-executable");
    }

    GtkWidget* image = nullptr;
    if (std::filesystem::is_regular_file(it->second.icon_name)) {
        image = gtk_image_new_from_file(it->second.icon_name.c_str());
    } else {
        image = gtk_image_new_from_icon_name(it->second.icon_name.c_str());
    }
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

void rebuild_app_list(SettingsPage* page);

// ---------------------------------------------------------------------------
// Pinned application rows
// ---------------------------------------------------------------------------

void move_app(SettingsPage* page, std::size_t index, int delta) {
    const std::size_t target = static_cast<std::size_t>(static_cast<int>(index) + delta);
    if (target >= page->config.pinned.size()) {
        return;
    }
    std::swap(page->config.pinned[index], page->config.pinned[target]);
    save(page);
    rebuild_app_list(page);
}

void remove_app(SettingsPage* page, std::size_t index) {
    if (index >= page->config.pinned.size()) {
        return;
    }
    const std::string removed = page->config.pinned[index];
    page->config.pinned.erase(page->config.pinned.begin() + static_cast<long>(index));

    auto& dividers = page->config.dividers_before;
    dividers.erase(std::remove(dividers.begin(), dividers.end(), removed), dividers.end());

    save(page);
    rebuild_app_list(page);
}

void set_divider(SettingsPage* page, const std::string& desktop_id, bool wanted) {
    auto& dividers = page->config.dividers_before;
    const auto it = std::find(dividers.begin(), dividers.end(), desktop_id);
    const bool present = it != dividers.end();

    if (wanted && !present) {
        dividers.push_back(desktop_id);
    } else if (!wanted && present) {
        dividers.erase(it);
    } else {
        return;
    }
    save(page);
}

struct RowContext {
    SettingsPage* page;
    std::size_t index;
    std::string desktop_id;
};

void row_context_free(gpointer data, GClosure*) {
    delete static_cast<RowContext*>(data);
}

GtkWidget* build_app_row(SettingsPage* page, std::size_t index) {
    const std::string& desktop_id = page->config.pinned[index];

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 10);
    gtk_widget_set_margin_end(row, 10);

    gtk_box_append(GTK_BOX(row), icon_for(page, desktop_id, 28));

    GtkWidget* label = gtk_label_new(title_for(page, desktop_id).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(label, desktop_id.c_str());
    gtk_box_append(GTK_BOX(row), label);

    // A separator belongs to the app it precedes, so the natural place to
    // control it is that app's row -- rather than a second list that has to be
    // kept in step with this one by hand.
    GtkWidget* divider_toggle = gtk_check_button_new_with_label("Separator before");
    const auto& dividers = page->config.dividers_before;
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(divider_toggle),
        std::find(dividers.begin(), dividers.end(), desktop_id) != dividers.end());
    gtk_widget_set_sensitive(divider_toggle, index > 0);
    g_signal_connect_data(
        divider_toggle, "toggled",
        G_CALLBACK(+[](GtkCheckButton* button, gpointer data) {
            auto* ctx = static_cast<RowContext*>(data);
            set_divider(ctx->page, ctx->desktop_id,
                        gtk_check_button_get_active(button) != FALSE);
        }),
        new RowContext{page, index, desktop_id}, row_context_free, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(row), divider_toggle);

    struct ButtonSpec {
        const char* icon;
        const char* tip;
        int delta;
    };
    static const ButtonSpec kMovers[] = {
        {"go-up-symbolic", "Move earlier", -1},
        {"go-down-symbolic", "Move later", 1},
    };

    for (const auto& spec : kMovers) {
        GtkWidget* button = gtk_button_new_from_icon_name(spec.icon);
        gtk_widget_set_tooltip_text(button, spec.tip);
        gtk_widget_set_sensitive(
            button, spec.delta < 0 ? index > 0 : index + 1 < page->config.pinned.size());
        g_signal_connect_data(
            button, "clicked",
            G_CALLBACK(+[](GtkButton* b, gpointer data) {
                auto* ctx = static_cast<RowContext*>(data);
                const int delta =
                    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lucid-delta"));
                move_app(ctx->page, ctx->index, delta);
            }),
            new RowContext{page, index, desktop_id}, row_context_free, G_CONNECT_DEFAULT);
        g_object_set_data(G_OBJECT(button), "lucid-delta", GINT_TO_POINTER(spec.delta));
        gtk_box_append(GTK_BOX(row), button);
    }

    GtkWidget* remove = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_set_tooltip_text(remove, "Remove from dock");
    g_signal_connect_data(
        remove, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* ctx = static_cast<RowContext*>(data);
            remove_app(ctx->page, ctx->index);
        }),
        new RowContext{page, index, desktop_id}, row_context_free, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(row), remove);

    return row;
}

void rebuild_app_list(SettingsPage* page) {
    GtkWidget* child = gtk_widget_get_first_child(page->app_list);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(page->app_list), child);
        child = next;
    }

    for (std::size_t i = 0; i < page->config.pinned.size(); ++i) {
        gtk_list_box_append(GTK_LIST_BOX(page->app_list), build_app_row(page, i));
    }
}

// ---------------------------------------------------------------------------
// Adding an application
// ---------------------------------------------------------------------------

void show_add_dialog(SettingsPage* page) {
    std::vector<std::string> available;
    for (const auto& [desktop_id, entry] : page->catalog) {
        const auto& pinned = page->config.pinned;
        if (std::find(pinned.begin(), pinned.end(), desktop_id) == pinned.end()) {
            available.push_back(desktop_id);
        }
    }
    std::sort(available.begin(), available.end(),
              [&](const std::string& a, const std::string& b) {
                  return title_for(page, a) < title_for(page, b);
              });

    GtkWidget* window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Add to Dock");
    gtk_window_set_default_size(GTK_WINDOW(window), 380, 480);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    if (GtkRoot* root = gtk_widget_get_root(page->root)) {
        gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(root));
    }

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    for (const auto& desktop_id : available) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_box_append(GTK_BOX(row), icon_for(page, desktop_id, 24));

        GtkWidget* label = gtk_label_new(title_for(page, desktop_id).c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(row), label);

        GtkWidget* add = gtk_button_new_with_label("Add");
        g_object_set_data(G_OBJECT(add), "lucid-window", window);
        g_signal_connect_data(
            add, "clicked",
            G_CALLBACK(+[](GtkButton* b, gpointer data) {
                auto* ctx = static_cast<RowContext*>(data);
                ctx->page->config.pinned.push_back(ctx->desktop_id);
                save(ctx->page);
                rebuild_app_list(ctx->page);
                auto* win = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(b), "lucid-window"));
                gtk_window_destroy(GTK_WINDOW(win));
            }),
            new RowContext{page, 0, desktop_id}, row_context_free, G_CONNECT_DEFAULT);
        gtk_box_append(GTK_BOX(row), add);

        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_window_set_child(GTK_WINDOW(window), scroller);
    gtk_window_present(GTK_WINDOW(window));
}

// ---------------------------------------------------------------------------
// Behaviour controls
// ---------------------------------------------------------------------------

GtkWidget* section_heading(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_margin_top(label, 14);
    gtk_widget_set_margin_bottom(label, 4);
    return label;
}

GtkWidget* labelled_row(const char* title, const char* subtitle, GtkWidget* control) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 10);
    gtk_widget_set_margin_end(row, 10);

    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget* title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
    gtk_box_append(GTK_BOX(text), title_label);

    if (subtitle != nullptr) {
        GtkWidget* subtitle_label = gtk_label_new(subtitle);
        gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0.0);
        gtk_label_set_wrap(GTK_LABEL(subtitle_label), TRUE);
        gtk_widget_add_css_class(subtitle_label, "dim-label");
        gtk_widget_add_css_class(subtitle_label, "caption");
        gtk_box_append(GTK_BOX(text), subtitle_label);
    }

    gtk_box_append(GTK_BOX(row), text);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

// Show where this value came from, and offer to undo just this one.
//
// This is the whole argument for the token model made visible. A settings panel
// that stores only the final value cannot answer "where did this come from" or
// "put only this back", because it never knew. Here both are one lookup, so
// they cost a label and a button rather than a feature each.
void refresh_token_row(SettingsPage* page, const SettingsPage::TokenRow& row) {
    const lucid::Resolved r = page->tokens->resolve(row.key);
    const bool overridden = r.layer != lucid::Layer::Default;
    gtk_label_set_text(GTK_LABEL(row.provenance),
                       overridden ? lucid::layer_name(r.layer) : "");
    gtk_widget_set_visible(row.provenance, overridden);
    gtk_widget_set_visible(row.reset, overridden);
}

void refresh_all_token_rows(SettingsPage* page) {
    for (const auto& row : page->token_rows) {
        refresh_token_row(page, row);
    }
}

// A slider whose range and description come from the schema rather than from
// numbers repeated here.
//
// That is not tidiness. The range in the schema is what the dock can actually
// contain, and it is enforced by clamping on load -- so a UI that invented its
// own bounds could offer a value the resolver would then quietly clamp, and the
// slider would sit somewhere the dock is not. One source, and the control
// cannot ask for something impossible.
GtkWidget* build_token_row(SettingsPage* page, const char* title, const std::string& key,
                           double step) {
    const lucid::KeyDef* def = page->tokens->schema().find(key);
    if (def == nullptr) {
        return gtk_label_new("");   // schema and UI disagree; say nothing rather than guess
    }

    SettingsPage::TokenRow row;
    row.key = key;
    row.scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                         def->min.value_or(0.0), def->max.value_or(1.0), step);
    gtk_range_set_value(GTK_RANGE(row.scale), page->tokens->get_double(key));
    gtk_scale_set_draw_value(GTK_SCALE(row.scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(row.scale), GTK_POS_RIGHT);
    gtk_widget_set_size_request(row.scale, 200, -1);

    row.provenance = gtk_label_new("");
    gtk_widget_add_css_class(row.provenance, "dim-label");
    gtk_widget_add_css_class(row.provenance, "caption");
    gtk_widget_set_tooltip_text(row.provenance, "Which layer set this value");

    row.reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_add_css_class(row.reset, "flat");
    gtk_widget_set_tooltip_text(row.reset, "Reset just this setting");
    gtk_widget_set_valign(row.reset, GTK_ALIGN_CENTER);

    page->token_rows.push_back(row);
    const std::string stored_key = key;

    g_object_set_data_full(G_OBJECT(row.scale), "lucid-key", g_strdup(key.c_str()), g_free);
    g_signal_connect_data(
        row.scale, "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer data) {
            auto* p = static_cast<SettingsPage*>(data);
            if (p->loading) {
                return;
            }
            const char* k = static_cast<const char*>(
                g_object_get_data(G_OBJECT(range), "lucid-key"));
            p->tokens->set_user(k, gtk_range_get_value(range), lucid::default_user_dir());
            refresh_all_token_rows(p);
        }),
        page, nullptr, G_CONNECT_DEFAULT);

    g_object_set_data_full(G_OBJECT(row.reset), "lucid-key", g_strdup(key.c_str()), g_free);
    g_signal_connect_data(
        row.reset, "clicked",
        G_CALLBACK(+[](GtkButton* button, gpointer data) {
            auto* p = static_cast<SettingsPage*>(data);
            const char* k = static_cast<const char*>(
                g_object_get_data(G_OBJECT(button), "lucid-key"));
            p->tokens->reset_user(k, lucid::default_user_dir());
            // Falls back to whatever the layer beneath says, which is not
            // necessarily the default -- a theme may still be setting it.
            p->loading = true;
            for (const auto& r : p->token_rows) {
                if (r.key == k) {
                    gtk_range_set_value(GTK_RANGE(r.scale), p->tokens->get_double(k));
                }
            }
            p->loading = false;
            refresh_all_token_rows(p);
        }),
        page, nullptr, G_CONNECT_DEFAULT);

    GtkWidget* control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(control), row.provenance);
    gtk_box_append(GTK_BOX(control), row.scale);
    gtk_box_append(GTK_BOX(control), row.reset);

    // The description is the schema's, so a key is documented once, in the
    // place that also validates it.
    return labelled_row(title, def->summary.empty() ? nullptr : def->summary.c_str(), control);
}


// Appearance only. The pinned list is the user's own arrangement, not a
// setting with a sensible default, and quietly discarding it because someone
// wanted the sliders back where they started would be a bad trade.
//
// The defaults come from DockConfig's own member initialisers rather than being
// written out a second time here, so there is one place they can be wrong.
void reset_to_defaults(SettingsPage* page) {
    const DockConfig defaults;
    page->config.magnification = defaults.magnification;

    // The token-backed settings are reset by removing them from the user
    // layer, not by writing the default back over them. Writing the default
    // would record "the user chose 57.6", which is a different statement from
    // "the user has not chosen", and it would override a theme that had a
    // legitimate opinion. This is the same distinction that makes provenance
    // worth storing at all.
    for (const auto& row : page->token_rows) {
        page->tokens->reset_user(row.key, lucid::default_user_dir());
    }

    // Put the controls back without each one writing on its way.
    page->loading = true;
    gtk_switch_set_active(GTK_SWITCH(page->magnify_switch),
                          defaults.magnification ? TRUE : FALSE);
    for (const auto& row : page->token_rows) {
        gtk_range_set_value(GTK_RANGE(row.scale), page->tokens->get_double(row.key));
    }
    page->loading = false;

    refresh_all_token_rows(page);
    lucid::write_config(page->config);
}

void free_page(gpointer data) {
    delete static_cast<SettingsPage*>(data);
}

}  // namespace

GtkWidget* lucid_dock_settings_page_new(void) {
    auto* page = new SettingsPage();
    page->catalog = lucid::load_desktop_catalog();
    page->loading = true;
    page->tokens = std::make_unique<lucid::Config>(lucid::default_schema());
    page->tokens->load(lucid::default_user_dir(), lucid::default_distro_dir());

    if (!lucid::read_config(page->config)) {
        page->config = lucid::ensure_config(page->catalog);
    }

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(column, 12);
    gtk_widget_set_margin_bottom(column, 12);
    gtk_widget_set_margin_start(column, 12);
    gtk_widget_set_margin_end(column, 12);
    page->root = column;

    gtk_box_append(GTK_BOX(column), section_heading("Appearance"));

    GtkWidget* frame = gtk_frame_new(nullptr);
    GtkWidget* behaviour = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_frame_set_child(GTK_FRAME(frame), behaviour);

    gtk_box_append(GTK_BOX(behaviour), build_token_row(page, "Size", "dock.icon-size", 1.0));
    gtk_box_append(GTK_BOX(behaviour), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget* magnify = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(magnify), page->config.magnification ? TRUE : FALSE);
    g_signal_connect_data(
        magnify, "state-set",
        G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer data) -> gboolean {
            auto* p = static_cast<SettingsPage*>(data);
            p->config.magnification = state != FALSE;
            save(p);
            return FALSE;
        }),
        page, nullptr, G_CONNECT_DEFAULT);
    page->magnify_switch = magnify;
    gtk_box_append(GTK_BOX(behaviour),
                   labelled_row("Enlarge icons under the pointer", nullptr, magnify));

    gtk_box_append(GTK_BOX(behaviour), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(behaviour),
                   build_token_row(page, "Maximum size", "dock.magnify-scale", 0.05));

    gtk_box_append(GTK_BOX(behaviour), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(behaviour),
                   build_token_row(page, "Spread", "dock.magnify-range", 0.1));

    refresh_all_token_rows(page);
    gtk_box_append(GTK_BOX(column), frame);

    GtkWidget* reset = gtk_button_new_with_label("Reset to Defaults");
    gtk_widget_set_halign(reset, GTK_ALIGN_END);
    gtk_widget_set_margin_top(reset, 8);
    gtk_widget_set_tooltip_text(reset,
                                "Restores size, magnification and spread. Your pinned "
                                "applications are left alone.");
    g_signal_connect_data(
        reset, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            reset_to_defaults(static_cast<SettingsPage*>(data));
        }),
        page, nullptr, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(column), reset);

    gtk_box_append(GTK_BOX(column), section_heading("Applications"));

    page->app_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->app_list), GTK_SELECTION_NONE);
    rebuild_app_list(page);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page->app_list);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* list_frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(list_frame), scroller);
    gtk_box_append(GTK_BOX(column), list_frame);

    page->add_button = gtk_button_new_with_label("Add Application…");
    gtk_widget_set_halign(page->add_button, GTK_ALIGN_START);
    gtk_widget_set_margin_top(page->add_button, 8);
    g_signal_connect_data(
        page->add_button, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            show_add_dialog(static_cast<SettingsPage*>(data));
        }),
        page, nullptr, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(column), page->add_button);

    GtkWidget* note = gtk_label_new(
        "Changes are saved to ~/.config/lucid/dock.conf and applied to a running dock "
        "immediately.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_widget_add_css_class(note, "caption");
    gtk_widget_set_margin_top(note, 10);
    gtk_box_append(GTK_BOX(column), note);

    page->loading = false;
    g_object_set_data_full(G_OBJECT(column), "lucid-settings-page", page, free_page);
    return column;
}
