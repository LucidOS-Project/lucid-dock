// Inject pointer events into a compositor, for testing what the dock does with
// them.
//
// This exists because the obvious method is a coin flip. `swaymsg seat - cursor
// set` warps sway's own cursor, and under a *headless* backend the seat has no
// pointer device at all -- capabilities 0, devices [] -- so the request returns
// success and moves nothing. Nested inside a host session there *is* a device,
// but motion only reaches clients while the host's real pointer happens to be
// over the nested window, so the same script passes or fails depending on where
// the mouse was left. Two identical runs of the icon-menu test disagreed, which
// is how this was found.
//
// zwlr_virtual_pointer_manager_v1 creates a real input device in the
// compositor. It works headless, it does not care where anything else's cursor
// is, and both sway and labwc advertise it.
//
//   virtual_pointer move <x> <y> <w> <h>   absolute, in output coordinates
//   virtual_pointer click <button>          left|right|middle
//   virtual_pointer move ... click right    both, in order
#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(registry, name,
                                   &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
        fprintf(stderr, "virtual_pointer: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (manager == NULL) {
        fprintf(stderr, "virtual_pointer: compositor does not offer "
                        "zwlr_virtual_pointer_manager_v1\n");
        return 2;
    }

    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    if (pointer == NULL) {
        fprintf(stderr, "virtual_pointer: could not create a pointer\n");
        return 3;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "move") == 0 && i + 4 < argc) {
            const uint32_t x = (uint32_t)atoi(argv[i + 1]);
            const uint32_t y = (uint32_t)atoi(argv[i + 2]);
            const uint32_t w = (uint32_t)atoi(argv[i + 3]);
            const uint32_t h = (uint32_t)atoi(argv[i + 4]);
            zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), x, y, w, h);
            zwlr_virtual_pointer_v1_frame(pointer);
            i += 4;
        } else if (strcmp(argv[i], "click") == 0 && i + 1 < argc) {
            const char *which = argv[i + 1];
            uint32_t button = BTN_LEFT;
            if (strcmp(which, "right") == 0) button = BTN_RIGHT;
            else if (strcmp(which, "middle") == 0) button = BTN_MIDDLE;
            zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
                                           WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(pointer);
            wl_display_flush(display);
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 60 * 1000 * 1000};
            nanosleep(&pause, NULL);
            zwlr_virtual_pointer_v1_button(pointer, now_ms(), button,
                                           WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(pointer);
            i += 1;
        } else {
            fprintf(stderr, "virtual_pointer: unrecognised argument '%s'\n", argv[i]);
            return 4;
        }
        wl_display_flush(display);
    }

    wl_display_roundtrip(display);
    // Deliberately not destroyed immediately: destroying the virtual pointer
    // removes the input device, and a compositor that processes the removal
    // before the events can drop them. The process exiting does it.
    return 0;
}
