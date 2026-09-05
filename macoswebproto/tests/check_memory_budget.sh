#!/usr/bin/env bash
# Asserts what the dock costs in RAM, the way LUCID_DOCK_BENCH asserts what it
# costs in frame time.
#
# A dock runs for the whole session, so its resident cost is a promise to the
# user in the same way 60 fps is, and it regresses the same way: quietly, in a
# commit about something else. This is the check that makes that loud.
#
# Two things make the number meaningful rather than decorative:
#
#   PSS, not RSS. RSS counts every shared library page in full, so it charges
#   the dock for all of GTK whether or not anything else is using it. PSS
#   divides shared pages by the number of processes mapping them, which is the
#   only number that adds up correctly across a whole session.
#
#   The budget is on the dock's cost ABOVE an empty GTK4 window, not on the
#   absolute figure. Measured on this machine, an empty GTK4 window is ~41 MiB
#   and the dock is ~50: four fifths of the absolute number is toolkit and
#   driver, which this project does not control and which moves when GTK or
#   Mesa is upgraded. Budgeting the absolute number means a GTK release fails
#   this check and teaches everyone to raise the limit. Budgeting the delta
#   fails only when the dock's own code gets heavier, which is the thing worth
#   being told about. The absolute is still printed, because it is what a user
#   actually pays.
#
# Runs under a headless nested compositor so it needs no display, no GPU and no
# layer-shell support from the host session -- which also means it measures the
# anchored path, not GNOME's unanchored fallback. Those differ by ~35 MiB, so
# the environment is part of the measurement.
set -uo pipefail
cd "$(dirname "$0")/.."

# MiB of PSS the dock may cost above an empty GTK4 window. Raise this only with
# a measurement and a reason in the commit message.
#
# Measured 9.6-9.7 MiB, repeatable to 0.1 MiB across runs. Set at 15 so the
# check has room for a machine whose built-in default pinned list resolves to
# more applications -- icon rasterisation is most of this number -- without
# leaving room for a regression to hide in. A doubling fails; noise does not.
BUDGET_MIB=${LUCID_MEM_BUDGET:-15}

WORK=$(mktemp -d)
SWAY_PID=""
cleanup() {
    [ -n "$SWAY_PID" ] && kill "$SWAY_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

command -v sway >/dev/null 2>&1 || { echo "SKIP: sway not installed" >&2; exit 0; }

# The floor: an empty GTK4 window, built here so it is always the same GTK the
# dock was just built against.
cat > "$WORK/floor.c" <<'EOF'
#include <gtk/gtk.h>
static void act(GtkApplication *a, gpointer d) {
    (void)d;
    GtkWidget *w = gtk_application_window_new(a);
    gtk_window_set_default_size(GTK_WINDOW(w), 400, 200);
    gtk_window_set_child(GTK_WINDOW(w), gtk_label_new("floor"));
    gtk_window_present(GTK_WINDOW(w));
}
int main(int c, char **v) {
    GtkApplication *a = gtk_application_new("dev.lucidos.MemFloor", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(a, "activate", G_CALLBACK(act), NULL);
    int s = g_application_run(G_APPLICATION(a), c, v);
    g_object_unref(a);
    return s;
}
EOF
gcc -O2 -o "$WORK/floor" "$WORK/floor.c" $(pkg-config --cflags --libs gtk4) || {
    echo "FAIL: could not build the floor probe" >&2; exit 1; }

pss_of() {
    awk '/^Pss:/{p+=$2} END{if (p) printf "%.1f", p/1024}' "/proc/$1/smaps_rollup" 2>/dev/null
}

# Runs one binary under its own headless compositor and reports steady-state
# PSS. Sampled late and repeatedly: memory climbs for the first few seconds as
# icons are rasterised and the first frames are drawn, and a number taken
# before it settles is not the number a user lives with.
measure() {
    local binary="$1" procname="$2" peak=0 sample
    # Its own XDG_CONFIG_HOME, for two reasons. Running this must not read or
    # write the developer's real ~/.config/lucid/dock.conf -- the dock writes
    # that file on first run. And the pinned list decides how many icons get
    # rasterised, which is most of what the dock's own memory is, so measuring
    # against whatever happens to be pinned on this machine would make the
    # number unreproducible anywhere else.
    #
    # XDG_CACHE_HOME is deliberately NOT isolated. Doing so was tried and cost
    # 11 MiB on the floor alone: a cold cache means Mesa recompiles its shaders
    # and fontconfig rebuilds, neither of which a running desktop pays. That
    # measures the first second of the first boot rather than what a user
    # lives with.
    # The output is exactly the dock's surface height on purpose. With no GPU
    # -- which is every CI runner -- GTK renders through llvmpipe and its
    # framebuffers are ordinary process memory, so buffer size lands directly
    # in PSS. With a GPU they do not. That difference is invisible locally and
    # dominates on a runner: at a 1080p output the floor window tiles to
    # 1920x1080 while the dock's layer surface is 1920x323, and the floor
    # measured 226 MiB against the dock's 171 -- a *negative* delta that
    # sailed through the budget. Matching the output to SURFACE_HEIGHT makes
    # both probes occupy identical geometry, so what is left is the difference
    # that was being asked about.
    cat > "$WORK/sway.cfg" <<EOF
output HEADLESS-1 resolution 1920x323
exec_always env XDG_CONFIG_HOME=$WORK/config $binary > $WORK/$procname.log 2>&1
EOF
    mkdir -p "$WORK/config"
    WLR_BACKENDS=headless sway -c "$WORK/sway.cfg" > "$WORK/sway.log" 2>&1 &
    SWAY_PID=$!

    local pid="" waited=0
    while [ -z "$pid" ] && [ "$waited" -lt 30 ]; do
        sleep 1; waited=$((waited + 1))
        pid=$(pgrep -x "$procname" | head -1)
    done
    if [ -z "$pid" ]; then
        echo "FAIL: $procname never started under headless sway" >&2
        cat "$WORK/sway.log" >&2; cat "$WORK/$procname.log" 2>/dev/null >&2
        kill "$SWAY_PID" 2>/dev/null; SWAY_PID=""
        return 1
    fi

    for _ in 1 2 3 4 5 6 7 8; do
        sleep 1
        sample=$(pss_of "$pid")
        [ -n "$sample" ] && peak=$(echo "$peak $sample" | awk '{print ($2>$1)?$2:$1}')
    done

    kill "$pid" 2>/dev/null
    kill "$SWAY_PID" 2>/dev/null; wait "$SWAY_PID" 2>/dev/null; SWAY_PID=""
    sleep 1
    echo "$peak"
}

echo "Measuring under headless sway (anchored layer-shell path)..."
floor=$(measure "$WORK/floor" floor) || exit 1
dock=$(measure "$PWD/lucid_dock_cpp" lucid_dock_cpp) || exit 1

delta=$(echo "$dock $floor" | awk '{printf "%.1f", $1-$2}')
over=$(echo "$delta $BUDGET_MIB" | awk '{print ($1 > $2) ? "yes" : "no"}')

# A measurement that says the dock costs nothing, or costs less than an empty
# window, is a broken measurement and not good news. This guard exists because
# the first CI run reported -55.3 MiB and passed: the check was green while
# protecting nothing, which is worse than having no check at all.
implausible=$(echo "$delta" | awk '{print ($1 < 1.0) ? "yes" : "no"}')
if [ "$implausible" = "yes" ]; then
    echo
    printf "  %-34s %8s MiB PSS\n" "empty GTK4 window (the floor)" "$floor"
    printf "  %-34s %8s MiB PSS\n" "lucid_dock_cpp" "$dock"
    printf "  %-34s %8s MiB\n"     "the dock's own cost" "$delta"
    echo
    echo "FAIL: the dock measured $delta MiB above an empty GTK4 window." >&2
    echo "That is not plausible -- the dock cannot cost less than the window it" >&2
    echo "draws into -- so the measurement is broken rather than the dock being" >&2
    echo "free. Usually this means the two probes are not the same size: with no" >&2
    echo "GPU, framebuffers are counted in PSS and buffer geometry dominates." >&2
    exit 1
fi

echo
printf "  %-34s %8s MiB PSS\n" "empty GTK4 window (the floor)" "$floor"
printf "  %-34s %8s MiB PSS\n" "lucid_dock_cpp" "$dock"
printf "  %-34s %8s MiB\n"     "the dock's own cost" "$delta"
printf "  %-34s %8s MiB\n"     "budget" "$BUDGET_MIB"
echo

if [ "$over" = "yes" ]; then
    echo "FAIL: the dock costs $delta MiB above an empty GTK4 window, over the $BUDGET_MIB MiB budget." >&2
    echo "Either find what grew, or raise LUCID_MEM_BUDGET with a reason in the commit message." >&2
    exit 1
fi
echo "PASS: $delta MiB of $BUDGET_MIB MiB budget."
