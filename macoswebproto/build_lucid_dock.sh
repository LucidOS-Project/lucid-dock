#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! pkg-config --exists gtk4; then
    echo "Missing GTK4 development files. Install: sudo apt install libgtk-4-dev" >&2
    exit 1
fi

# Only gtk4-layer-shell-0 is usable here. gtk-layer-shell-0 is the GTK3 build:
# linking it into a GTK4 application does not work, so it is not a fallback.
if ! pkg-config --exists gtk4-layer-shell-0; then
    echo "Missing gtk4-layer-shell development files." >&2
    echo "Note: libgtk-layer-shell-dev is the GTK3 version and will NOT work." >&2
    exit 1
fi

echo "Building with gtk4-layer-shell-0 $(pkg-config --modversion gtk4-layer-shell-0)"
g++ -std=c++20 -DHAVE_GTK4_LAYER_SHELL=1 lucid_dock.cpp -o lucid_dock_cpp \
    $(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)
echo "Built ./lucid_dock_cpp"
