#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! pkg-config --exists gtk4; then
    echo "Missing GTK4 development files. Install: sudo apt install libgtk-4-dev" >&2
    exit 1
fi

layer_shell_module=""
if pkg-config --exists gtk4-layer-shell-0; then
    layer_shell_module="gtk4-layer-shell-0"
elif pkg-config --exists gtk-layer-shell-0; then
    layer_shell_module="gtk-layer-shell-0"
else
    echo "Missing layer-shell development files. Install: sudo apt install libgtk-layer-shell-dev" >&2
    exit 1
fi

echo "Building with $layer_shell_module"
g++ -std=c++20 lucid_dock.cpp -o lucid_dock_cpp $(pkg-config --cflags --libs gtk4 "$layer_shell_module")
echo "Built ./lucid_dock_cpp"
