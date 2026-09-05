#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Allow a user-prefix install of gtk4-layer-shell (see README: distros older than
# Ubuntu 25.10 / Debian trixie do not package it, so it gets built into ~/.local).
LOCAL_LIBDIR="$HOME/.local/lib/$(gcc -dumpmachine)"
export PKG_CONFIG_PATH="$LOCAL_LIBDIR/pkgconfig:$HOME/.local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

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
# process instead of by comparing timestamps by hand.
BUILD_STAMP="$(date '+%Y-%m-%d %H:%M:%S')"
GIT_REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=""
git diff --quiet 2>/dev/null || GIT_DIRTY="+dirty"

echo "Building with gtk4-layer-shell-0 $(pkg-config --modversion gtk4-layer-shell-0)"
g++ -std=c++20 -O2 -DHAVE_GTK4_LAYER_SHELL=1 lucid_dock.cpp -o lucid_dock_cpp \
    $(pkg-config --cflags --libs gtk4-layer-shell-0 gtk4 gio-unix-2.0) \
    -DLUCID_BUILD_STAMP="\"$BUILD_STAMP\"" -DLUCID_GIT_REV="\"$GIT_REV$GIT_DIRTY\"" \
    $rpath_flag ${LUCID_RPATH:+-Wl,-rpath,"$LUCID_RPATH"}
echo "Built ./lucid_dock_cpp"

# The settings UI is a separate binary sharing dock_config.h. It has no
# layer-shell dependency -- it is an ordinary window.
g++ -std=c++20 -O2 dock_settings_page.cpp lucid_dock_settings.cpp -o lucid_dock_settings \
    -DLUCID_BUILD_STAMP="\"$BUILD_STAMP\"" -DLUCID_GIT_REV="\"$GIT_REV$GIT_DIRTY\"" \
    $(pkg-config --cflags --libs gtk4 gio-unix-2.0)
echo "Built ./lucid_dock_settings"
