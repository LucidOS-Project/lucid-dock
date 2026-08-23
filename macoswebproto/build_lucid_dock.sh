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
echo "Building with gtk4-layer-shell-0 $(pkg-config --modversion gtk4-layer-shell-0)"
g++ -std=c++20 -O2 -DHAVE_GTK4_LAYER_SHELL=1 lucid_dock.cpp -o lucid_dock_cpp \
    $(pkg-config --cflags --libs gtk4-layer-shell-0 gtk4 gio-unix-2.0) \
    ${LUCID_RPATH:+-Wl,-rpath,"$LUCID_RPATH"}
echo "Built ./lucid_dock_cpp"
