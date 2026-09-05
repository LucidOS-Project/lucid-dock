// The standalone wrapper. Deliberately trivial: every line of actual settings
// UI lives in dock_settings_page.cpp so that LucidOS Settings can host the same
// widget without touching it. If this file ever grows, something has been put
// in the wrong place.

#include "dock_settings_page.h"
#include "build_stamp.h"

#include <gtk/gtk.h>

namespace {

void on_activate(GtkApplication* app, gpointer) {
    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Dock Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 620, 700);
    gtk_window_set_child(GTK_WINDOW(window), lucid_dock_settings_page_new());
    gtk_window_present(GTK_WINDOW(window));
}

}  // namespace


int main(int argc, char** argv) {
    g_message("lucid-dock-settings %s built %s", lucid::kGitRev, lucid::kBuildStamp);
    if (argc > 1 && g_strcmp0(argv[1], "--version") == 0) {
        return 0;
    }

    GtkApplication* app = gtk_application_new("dev.lucidos.DockSettings", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
