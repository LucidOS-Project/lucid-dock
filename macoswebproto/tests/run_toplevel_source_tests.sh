#!/usr/bin/env bash
# Exercises the dock's foreign-toplevel client against a compositor that
# implements ext-foreign-toplevel-list-v1 and nothing else.
#
# This is the only way to run that path on a host whose newest available
# compositor is sway 1.9 / wlroots 0.17, which does not implement it. The wlr
# path is testable for real -- see the README -- so this covers the one that
# is not.
set -euo pipefail
cd "$(dirname "$0")"

build=$(mktemp -d)
trap 'rm -rf "$build"; [ -n "${fake_pid:-}" ] && kill "$fake_pid" 2>/dev/null || true' EXIT

# The protocol XML moved to lucid-wayland with the client that speaks it.
WL=../third_party/lucid-wayland
proto=$WL/protocols/ext-foreign-toplevel-list-v1.xml
wayland-scanner server-header "$proto" "$build/ext-foreign-toplevel-list-v1-server-protocol.h"
wayland-scanner client-header "$proto" "$build/ext-foreign-toplevel-list-v1-client-protocol.h"
wayland-scanner private-code  "$proto" "$build/ext-protocol.c"

# The client side also links the wlr marshalling, because the toplevel source
# contains both backends.
wlr=$WL/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
wayland-scanner client-header "$wlr" "$build/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$wlr" "$build/wlr-protocol.c"

gcc -std=c11 -O1 -g -c "$build/ext-protocol.c" -o "$build/ext-protocol.o" $(pkg-config --cflags wayland-client)
gcc -std=c11 -O1 -g -c "$build/wlr-protocol.c" -o "$build/wlr-protocol.o" $(pkg-config --cflags wayland-client)

gcc -std=gnu11 -Wall -Wextra -O1 -g -I"$build" fake_ext_compositor.c "$build/ext-protocol.o" \
    -o "$build/fake_ext_compositor" $(pkg-config --cflags --libs wayland-server)

# Built against lucid-wayland's sources rather than its library, so the probe
# gets the same protocol headers the fake compositor was generated from -- the
# point of this test is that the client and the server agree, and linking a
# prebuilt .a would let them be generated from different XML without anyone
# noticing.
g++ -std=c++20 -O1 -g -I"$build" -I"$WL/include" \
    toplevel_source_probe.cpp "$WL/src/toplevel_source.cpp" \
    "$build/ext-protocol.o" "$build/wlr-protocol.o" -o "$build/probe" \
    $(pkg-config --cflags --libs glib-2.0 gio-2.0 wayland-client)

# Its own runtime dir, so the fake socket cannot collide with the real session
# and the probe cannot accidentally reach the real compositor.
export XDG_RUNTIME_DIR="$build/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

"$build/fake_ext_compositor" lucid-fake-ext 2>"$build/fake.log" &
fake_pid=$!

# Wait for the socket rather than sleeping a guessed amount.
for _ in $(seq 50); do
    [ -S "$XDG_RUNTIME_DIR/lucid-fake-ext" ] && break
    sleep 0.1
done
if [ ! -S "$XDG_RUNTIME_DIR/lucid-fake-ext" ]; then
    echo "FAIL: fake compositor never created its socket" >&2
    cat "$build/fake.log" >&2
    exit 1
fi

WAYLAND_DISPLAY=lucid-fake-ext "$build/probe" 6000 > "$build/probe.log" 2>"$build/probe.err"

echo "--- fake compositor ---"
cat "$build/fake.log"
echo "--- probe ---"
head -1 "$build/probe.log"

# Falling back to /proc means the probe never reached the fake compositor, and
# the key-set diff that follows would be several hundred process names deep for
# no reason. Say what actually went wrong instead.
if ! head -1 "$build/probe.log" | grep -qx "source: ext"; then
    echo "FAIL: probe did not select the ext source -- it got '$(head -1 "$build/probe.log")'." >&2
    echo "The fake compositor's log:" >&2
    cat "$build/fake.log" >&2
    echo "The probe's stderr:" >&2
    cat "$build/probe.err" >&2
    exit 1
fi
sed -n '2,$p' "$build/probe.log"

# Every window-list sample, for eyeballing when something fails.
echo "--- window list over time ---"
grep -E '^(initial|t\+[0-9.]+s) windows:' "$build/probe.log"

# Expected sequence. Each line is the complete key set after one change, so a
# missing event and a spurious one both show up as a diff rather than as a
# grep that happens to still pass.
# Only the key-set lines and the final window list: the periodic samples in
# between are timing-dependent and would make this a flaky test rather than a
# stricter one. The samples are still printed, and shown above on failure.
cat > "$build/expected" <<'EXPECTED'
source: ext
reports windows: yes
initial: [org.gnome.texteditor]
changed: [firefox org.gnome.texteditor]
changed: [org.gnome.texteditor]
changed: [org.gnome.console]
changed: []
EXPECTED

echo "--- diff (expected vs actual) ---"
if ! diff -u "$build/expected" <(grep -vE '^(initial|t\+[0-9.]+s) windows:' "$build/probe.log"); then
    echo "FAIL: key-set sequence" >&2
    cat "$build/probe.err" >&2
    exit 1
fi

# The window list is what the key set cannot tell you. Two windows of one
# application are one key and two entries, a retitle moves the list and not the
# keys, and the order is the order the compositor announced them in.
# Asserted on content, never on which sample it landed in: the fake's script
# steps every 700 ms and the probe samples every 500 ms, so the two drift and
# pinning a state to a timestamp is a flaky test rather than a stricter one.
# Whole-line matching after the prefix is stripped, so a two-window list cannot
# pass by being a prefix of a three-window one.
sed -nE 's/^(initial|t\+[0-9.]+s) windows: ?//p' "$build/probe.log" > "$build/samples"

check() {
    if ! grep -qxF "$2" "$build/samples"; then
        echo "FAIL: no window-list sample ever equalled:" >&2
        echo "  $2" >&2
        echo "($1)" >&2
        echo "what was actually seen:" >&2
        sort -u "$build/samples" | sed 's/^/  /' >&2
        exit 1
    fi
    echo "  ok: $1"
}
echo "--- window list assertions ---"
check "initial enumeration carries title and identifier" \
      "{org.gnome.texteditor|Untitled Document|a}"
check "two windows of one app are two entries, oldest first" \
      "{org.gnome.texteditor|Untitled Document|a} {firefox|Wikipedia|b} {firefox|Hacker News|c}"
check "a retitle moves the window list, and only that window" \
      "{org.gnome.texteditor|Untitled Document|a} {firefox|Wikipedia|b} {firefox|Lobsters|c}"
check "closing one of two leaves the other, in place" \
      "{org.gnome.texteditor|Untitled Document|a} {firefox|Lobsters|c}"
check "an app_id change keeps the window and its title" \
      "{org.gnome.console|Untitled Document|a}"
check "everything closed empties the list" \
      ""
echo "PASS"
