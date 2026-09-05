#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Allow a user-prefix install of gtk4-layer-shell (see README: distros older than
# Ubuntu 25.10 / Debian trixie do not package it, so it gets built into ~/.local).
LOCAL_LIBDIR="$HOME/.local/lib/$(gcc -dumpmachine)"
export PKG_CONFIG_PATH="$LOCAL_LIBDIR/pkgconfig:$HOME/.local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# lucid-tokens is a submodule, pinned to a commit, so the schema and the dock
# cannot drift apart the way they already did once -- two of fifteen keys were
# configuring an easing the dock had deleted.
for sub in lucid-tokens lucid-wayland; do
    if [ ! -d "third_party/$sub/src" ]; then
        echo "Missing third_party/$sub. Run:" >&2
        echo "  git submodule update --init --recursive" >&2
        exit 1
    fi
done

# The shared Wayland client. The protocol XML and the toplevel source used to
# live here; they moved so lucid-panel could use one implementation rather than
# a second copy that would drift.
make -C third_party/lucid-wayland

if ! pkg-config --exists gtk4; then
    echo "Missing GTK4 development files. Install: sudo apt install libgtk-4-dev" >&2
    exit 1
fi

# Only gtk4-layer-shell-0 is usable here. gtk-layer-shell-0 is the GTK3 build:
# linking it into a GTK4 application does not work, so it is not a fallback.
if ! pkg-config --exists gtk4-layer-shell-0; then
    echo "Missing gtk4-layer-shell development files." >&2
    echo "  Ubuntu 25.10+ / Debian trixie+: sudo apt install libgtk4-layer-shell-dev" >&2
    echo "  Older releases do not package it: see README, 'Building gtk4-layer-shell'." >&2
    echo "Note: libgtk-layer-shell-dev is the GTK3 version and will NOT work." >&2
    exit 1
fi

# The foreign-toplevel protocols, and the client that speaks them, now live in
# third_party/lucid-wayland -- which generates and compiles the marshalling
# itself, so this build no longer does. wayland-scanner is still required; the
# submodule's Makefile calls it.
if ! command -v wayland-scanner >/dev/null 2>&1; then
    echo "Missing wayland-scanner. Install: sudo apt install libwayland-bin" >&2
    exit 1
fi

mkdir -p generated
proto_objs=""

# gtk4-layer-shell must appear before libwayland on the link line: it works by
# shimming libwayland, and linking it after produces a run-time failure rather
# than a link error.
# A gtk4-layer-shell built into a ~/.local prefix is not on the runtime linker's
# search path, so without an rpath the binary compiles and then fails to start
# with "libgtk4-layer-shell.so.0 => not found". Bake the library's own directory
# in rather than making every caller export LD_LIBRARY_PATH.
layer_shell_libdir="$(pkg-config --variable=libdir gtk4-layer-shell-0)"
rpath_flag=""
case "$layer_shell_libdir" in
    /usr/lib*|/lib*|"") ;;                       # already on the default path
    *) rpath_flag="-Wl,-rpath,$layer_shell_libdir" ;;
esac

# Delete the binaries first. A failed build otherwise leaves the previous ones
# in place, and running ./lucid_dock_cpp then silently gets you the last build
# that worked -- which has already cost this project an afternoon of measuring
# a binary that did not contain the change being measured. Better to have no
# dock than a lying one.
rm -f lucid_dock_cpp lucid_dock_settings

# Stamped into the binary so "is this current?" is answerable from the running
# process instead of by comparing timestamps by hand. Generated rather than
# passed as -D defines so the CMake build can use the identical format and the
# identical definition of "dirty" -- see write_build_stamp.sh.
./write_build_stamp.sh generated/build_stamp.cpp .

echo "Building with gtk4-layer-shell-0 $(pkg-config --modversion gtk4-layer-shell-0)"
g++ -std=c++20 -O2 -DHAVE_GTK4_LAYER_SHELL=1 -Igenerated -I. \
    -Ithird_party/lucid-tokens/include \
    -Ithird_party/lucid-wayland/include \
    lucid_dock.cpp generated/build_stamp.cpp \
    third_party/lucid-wayland/liblucidwayland.a \
    third_party/lucid-tokens/src/tokens.cpp $proto_objs -o lucid_dock_cpp \
    $(pkg-config --cflags --libs gtk4-layer-shell-0 gtk4 gio-unix-2.0 wayland-client) \
    $rpath_flag ${LUCID_RPATH:+-Wl,-rpath,"$LUCID_RPATH"}
echo "Built ./lucid_dock_cpp"

# The settings UI is a separate binary sharing dock_config.h. It has no
# layer-shell dependency -- it is an ordinary window.
g++ -std=c++20 -O2 -I. -Ithird_party/lucid-tokens/include \
    dock_settings_page.cpp lucid_dock_settings.cpp \
    generated/build_stamp.cpp third_party/lucid-tokens/src/tokens.cpp -o lucid_dock_settings \
    $(pkg-config --cflags --libs gtk4 gio-unix-2.0)
echo "Built ./lucid_dock_settings"
