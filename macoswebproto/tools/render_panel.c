// Renders the dock panel through GTK's own renderer and writes a PNG.
//
// The dock's appearance has now twice been reasoned about with a model of what
// CSS says instead of what GSK does, and the second time shipped a shadow that
// was still blocky. A model of box-shadow is not GTK's box-shadow: GSK's
// renderers implement blur their own way, and the only way to know what a
// stylesheet actually looks like is to have GTK draw it.
//
// Takes a CSS file and writes a PNG, so variants can be compared without
// rebuilding anything. Run it under a headless nested compositor
// (WLR_BACKENDS=headless) so it never puts a window on a real screen.
//
//   render_panel <style.css> <out.png> [panel_w] [panel_h]

#include <gtk/gtk.h>

static const char *css_path, *out_path;
static int panel_w = 620, panel_h = 79;
static int img_w, img_h;
static GtkWidget *window, *sheet, *panel_widget;

static gboolean place(gpointer data) {
    (void)data;
    // Centre the panel in whatever the compositor actually gave us.
    gtk_fixed_move(GTK_FIXED(sheet), panel_widget,
                   (gtk_widget_get_width(sheet) - panel_w) / 2.0,
                   (gtk_widget_get_height(sheet) - panel_h) / 2.0);
    return G_SOURCE_REMOVE;
}

static gboolean capture(gpointer data) {
    (void)data;
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    if (renderer == NULL) {
        g_printerr("no renderer -- window not realized\n");
        gtk_window_destroy(GTK_WINDOW(window));
        return G_SOURCE_REMOVE;
    }
    g_print("renderer: %s\n", G_OBJECT_TYPE_NAME(renderer));

    // Snapshot at the widget's real allocation, not at the size we asked for.
    // A compositor that tiles the toplevel gives it the whole output, and
    // snapshotting a 1920x1080 widget into an 800x259 paintable silently
    // rescales everything -- which is exactly what made the first attempt at
    // this render a shrunken panel in the corner.
    img_w = gtk_widget_get_width(sheet);
    img_h = gtk_widget_get_height(sheet);
    g_print("allocation: %dx%d, panel %dx%d at (%d,%d)\n", img_w, img_h, panel_w, panel_h,
            (img_w - panel_w) / 2, (img_h - panel_h) / 2);

    GdkPaintable *paintable = gtk_widget_paintable_new(sheet);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, img_w, img_h);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (node == NULL) {
        g_printerr("empty render node\n");
        gtk_window_destroy(GTK_WINDOW(window));
        return G_SOURCE_REMOVE;
    }

    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, NULL);
    GError *error = NULL;
    if (!gdk_texture_save_to_png(texture, out_path)) {
        g_printerr("could not write %s\n", out_path);
    } else {
        g_print("wrote %s (%dx%d)\n", out_path, img_w, img_h);
    }
    if (error) g_error_free(error);

    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    gtk_window_destroy(GTK_WINDOW(window));
    return G_SOURCE_REMOVE;
}

static void on_map(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    // One frame later, so the renderer is realized and the CSS is resolved.
    g_timeout_add(60, place, NULL);
    g_timeout_add(220, capture, NULL);
}

static void activate(GtkApplication *app, gpointer d) {
    (void)d;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, css_path);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    window = gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), img_w, img_h);

    sheet = gtk_fixed_new();
    gtk_widget_add_css_class(sheet, "sheet");
    gtk_widget_set_size_request(sheet, img_w, img_h);

    panel_widget = gtk_fixed_new();
    gtk_widget_add_css_class(panel_widget, "dock-panel");
    gtk_widget_set_size_request(panel_widget, panel_w, panel_h);
    gtk_fixed_put(GTK_FIXED(sheet), panel_widget, 0, 0);   // placed for real once allocated

    gtk_window_set_child(GTK_WINDOW(window), sheet);
    g_signal_connect(window, "map", G_CALLBACK(on_map), NULL);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        g_printerr("usage: render_panel <style.css> <out.png> [panel_w] [panel_h]\n");
        return 2;
    }
    css_path = argv[1];
    out_path = argv[2];
    if (argc > 3) panel_w = atoi(argv[3]);
    if (argc > 4) panel_h = atoi(argv[4]);
    img_w = panel_w + 180;
    img_h = panel_h + 180;

    GtkApplication *app = gtk_application_new("dev.lucidos.RenderPanel",
                                              G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
