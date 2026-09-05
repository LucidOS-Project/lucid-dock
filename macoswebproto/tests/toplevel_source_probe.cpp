// Runs the dock's real ToplevelSource against whatever compositor
// WAYLAND_DISPLAY points at, and prints the running key set every time it
// changes. Linked against lucid-wayland's source itself rather than a copy, so
// what is tested is what ships.
//
// Used by run_toplevel_source_tests.sh to exercise
// ext-foreign-toplevel-list-v1, which no compositor on a 24.04-era host
// implements.

#include "lucid/toplevel_source.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

lucid::ToplevelSource* g_source = nullptr;

// The key set, sorted, because it is a set and its order is not meaningful.
void print_keys(const char* when) {
    std::vector<std::string> keys(g_source->running_keys().begin(),
                                  g_source->running_keys().end());
    std::sort(keys.begin(), keys.end());

    std::string joined;
    for (const auto& key : keys) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += key;
    }
    // One line per state, on stdout, so the driver can diff the whole sequence
    // rather than grepping for individual events.
    std::printf("%s: [%s]\n", when, joined.c_str());
    std::fflush(stdout);
}

// The window list, deliberately *not* sorted: its order is part of the contract
// -- oldest first -- so sorting it here would hide the one property a taskbar
// would depend on.
void print_windows(const char* when) {
    std::printf("%s windows:", when);
    for (const auto& toplevel : g_source->toplevels()) {
        std::printf(" {%s|%s|%s}", toplevel.app_id.c_str(), toplevel.title.c_str(),
                    toplevel.identifier.c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

gboolean stop(gpointer loop) {
    g_main_loop_quit(static_cast<GMainLoop*>(loop));
    return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char** argv) {
    const int run_ms = argc > 1 ? atoi(argv[1]) : 6000;

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    auto source = lucid::make_toplevel_source([]() { print_keys("changed"); });
    g_source = source.get();

    std::printf("source: %s\n", lucid::toplevel_source_token(source->kind()));
    std::printf("reports windows: %s\n", source->reports_windows() ? "yes" : "no");
    print_keys("initial");
    print_windows("initial");

    // The window list changes on things the key set does not move for -- a
    // second window of a running application, a retitle -- and those changes
    // fire no callback by design. Sampling on a timer is how a test observes
    // that gap without inventing a signal the dock does not want.
    g_timeout_add(500, +[](gpointer) -> gboolean {
        static int tick = 0;
        char label[32];
        std::snprintf(label, sizeof(label), "t+%.1fs", 0.5 * ++tick);
        print_windows(label);
        return G_SOURCE_CONTINUE;
    }, nullptr);

    g_timeout_add(run_ms, &stop, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}
