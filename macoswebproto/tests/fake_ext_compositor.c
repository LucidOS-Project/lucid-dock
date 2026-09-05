// A compositor that implements ext-foreign-toplevel-list-v1 and nothing else.
//
// It exists because no compositor available on a 24.04-era host implements that
// protocol: ext-foreign-toplevel-list landed in wlroots 0.18 / sway 1.10, and
// noble ships sway 1.9. Without this the dock's primary running-app source
// would ship having never once been run.
//
// It is not a compositor in any useful sense -- no surfaces, no seat, no
// rendering. It advertises one global and plays a fixed script of toplevels at
// it, which is exactly enough to exercise the client side of the protocol:
// initial enumeration, a later toplevel, two toplevels sharing an app_id, an
// app_id changed under `done`, and `closed`.
//
// Build and run from run_toplevel_source_tests.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include "ext-foreign-toplevel-list-v1-server-protocol.h"

struct fake_toplevel {
    struct wl_list link;        // in `toplevels`
    struct wl_list handles;     // wl_resource links
    char *app_id;
    char *identifier;
    int id;
};

static struct wl_display *display;
static struct wl_list toplevels;
static struct wl_list lists;    // bound ext_foreign_toplevel_list_v1 resources
static int step;

// --- ext_foreign_toplevel_handle_v1 -----------------------------------------

static void handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_foreign_toplevel_handle_v1_interface handle_impl = {
    .destroy = handle_destroy,
};

static void handle_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void send_toplevel_to(struct wl_resource *list_resource, struct fake_toplevel *toplevel) {
    struct wl_client *client = wl_resource_get_client(list_resource);
    struct wl_resource *handle = wl_resource_create(
        client, &ext_foreign_toplevel_handle_v1_interface,
        wl_resource_get_version(list_resource), 0);
    if (handle == NULL) {
        return;
    }
    wl_resource_set_implementation(handle, &handle_impl, toplevel, handle_resource_destroy);
    wl_list_insert(&toplevel->handles, wl_resource_get_link(handle));

    ext_foreign_toplevel_list_v1_send_toplevel(list_resource, handle);
    ext_foreign_toplevel_handle_v1_send_identifier(handle, toplevel->identifier);
    ext_foreign_toplevel_handle_v1_send_app_id(handle, toplevel->app_id);
    ext_foreign_toplevel_handle_v1_send_done(handle);
}

// --- ext_foreign_toplevel_list_v1 -------------------------------------------

static void list_stop(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    ext_foreign_toplevel_list_v1_send_finished(resource);
}

static void list_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_foreign_toplevel_list_v1_interface list_impl = {
    .stop = list_stop,
    .destroy = list_destroy,
};

static void list_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void list_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource =
        wl_resource_create(client, &ext_foreign_toplevel_list_v1_interface, version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &list_impl, NULL, list_resource_destroy);
    wl_list_insert(&lists, wl_resource_get_link(resource));
    fprintf(stderr, "[fake] client bound the list\n");

    // "All currently mapped toplevels are sent immediately after the bind."
    struct fake_toplevel *toplevel;
    wl_list_for_each(toplevel, &toplevels, link) {
        send_toplevel_to(resource, toplevel);
    }
}

// --- the script -------------------------------------------------------------

static struct fake_toplevel *add_toplevel(const char *app_id, const char *identifier) {
    struct fake_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->app_id = strdup(app_id);
    toplevel->identifier = strdup(identifier);
    wl_list_init(&toplevel->handles);
    wl_list_insert(toplevels.prev, &toplevel->link);

    struct wl_resource *list_resource;
    wl_list_for_each(list_resource, &lists, link) {
        send_toplevel_to(list_resource, toplevel);
    }
    fprintf(stderr, "[fake] + toplevel %s (%s)\n", app_id, identifier);
    return toplevel;
}

static struct fake_toplevel *find_toplevel(const char *identifier) {
    struct fake_toplevel *toplevel;
    wl_list_for_each(toplevel, &toplevels, link) {
        if (strcmp(toplevel->identifier, identifier) == 0) {
            return toplevel;
        }
    }
    return NULL;
}

static void close_toplevel(const char *identifier) {
    struct fake_toplevel *toplevel = find_toplevel(identifier);
    if (toplevel == NULL) {
        return;
    }
    struct wl_resource *handle, *tmp;
    wl_resource_for_each_safe(handle, tmp, &toplevel->handles) {
        ext_foreign_toplevel_handle_v1_send_closed(handle);
    }
    wl_list_remove(&toplevel->link);
    fprintf(stderr, "[fake] - toplevel %s (%s)\n", toplevel->app_id, identifier);
}

// An app_id change is double-buffered: it is not in effect until `done`.
static void rename_toplevel(const char *identifier, const char *app_id) {
    struct fake_toplevel *toplevel = find_toplevel(identifier);
    if (toplevel == NULL) {
        return;
    }
    free(toplevel->app_id);
    toplevel->app_id = strdup(app_id);
    struct wl_resource *handle;
    wl_list_for_each(handle, &toplevel->handles, link) {
        ext_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
        ext_foreign_toplevel_handle_v1_send_done(handle);
    }
    fprintf(stderr, "[fake] ~ toplevel %s is now %s\n", identifier, app_id);
}

static struct wl_event_source *script_timer;

static int advance(void *data) {
    (void)data;

    switch (step++) {
        case 0: add_toplevel("firefox", "b"); break;
        case 1: add_toplevel("firefox", "c"); break;         // second window, same app_id
        case 2: close_toplevel("b"); break;                  // one of two: key must survive
        case 3: close_toplevel("c"); break;                  // last one: key must go
        case 4: rename_toplevel("a", "org.gnome.Console"); break;
        case 5: close_toplevel("a"); break;
        default:
            fprintf(stderr, "[fake] script finished\n");
            wl_display_terminate(display);
            return 0;
    }

    wl_display_flush_clients(display);
    wl_event_source_timer_update(script_timer, 700);
    return 0;
}

int main(int argc, char **argv) {
    const char *socket_name = argc > 1 ? argv[1] : "lucid-fake-ext";

    display = wl_display_create();
    wl_list_init(&toplevels);
    wl_list_init(&lists);

    if (wl_display_add_socket(display, socket_name) != 0) {
        fprintf(stderr, "[fake] cannot create socket '%s'\n", socket_name);
        return 1;
    }
    if (wl_global_create(display, &ext_foreign_toplevel_list_v1_interface, 1, NULL, list_bind) ==
        NULL) {
        fprintf(stderr, "[fake] cannot advertise the global\n");
        return 1;
    }

    // One toplevel is already open before the client connects, so the initial
    // enumeration path is exercised and not just the incremental one.
    add_toplevel("org.gnome.TextEditor", "a");

    // One timer, re-armed by its own callback. It is kept in a file static
    // because the callback has to re-arm the source it is running on, and that
    // source does not exist yet at the point it would be passed as user data.
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    script_timer = wl_event_loop_add_timer(loop, advance, NULL);
    wl_event_source_timer_update(script_timer, 700);

    fprintf(stderr, "[fake] listening on WAYLAND_DISPLAY=%s\n", socket_name);
    wl_display_run(display);
    wl_display_destroy(display);
    return 0;
}
