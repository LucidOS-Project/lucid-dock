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

proto=../protocols/ext-foreign-toplevel-list-v1.xml
wayland-scanner server-header "$proto" "$build/ext-foreign-toplevel-list-v1-server-protocol.h"
wayland-scanner client-header "$proto" "$build/ext-foreign-toplevel-list-v1-client-protocol.h"
wayland-scanner private-code  "$proto" "$build/ext-protocol.c"

# The client side also links the wlr marshalling, because toplevel_source.cpp
# contains both backends.
wlr=../protocols/wlr-foreign-toplevel-management-unstable-v1.xml
wayland-scanner client-header "$wlr" "$build/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$wlr" "$build/wlr-protocol.c"

gcc -std=c11 -O1 -g -c "$build/ext-protocol.c" -o "$build/ext-protocol.o" $(pkg-config --cflags wayland-client)
gcc -std=c11 -O1 -g -c "$build/wlr-protocol.c" -o "$build/wlr-protocol.o" $(pkg-config --cflags wayland-client)

gcc -std=gnu11 -Wall -Wextra -O1 -g -I"$build" fake_ext_compositor.c "$build/ext-protocol.o" \
    -o "$build/fake_ext_compositor" $(pkg-config --cflags --libs wayland-server)

g++ -std=c++20 -O1 -g -I"$build" toplevel_source_probe.cpp ../toplevel_source.cpp \
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

# Expected sequence. Each line is the complete key set after one change, so a
# missing event and a spurious one both show up as a diff rather than as a
# grep that happens to still pass.
cat > "$build/expected" <<'EXPECTED'
source: ext
initial: [org.gnome.texteditor]
changed: [firefox org.gnome.texteditor]
changed: [org.gnome.texteditor]
changed: [org.gnome.console]
changed: []
EXPECTED

echo "--- diff (expected vs actual) ---"
if diff -u "$build/expected" "$build/probe.log"; then
    echo "PASS"
else
    echo "FAIL" >&2
    cat "$build/probe.err" >&2
    exit 1
fi
