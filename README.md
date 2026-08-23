# Lucid Dock

The LucidOS dock: a magnifying, animated dock implemented as a **GTK4 +
gtk4-layer-shell** client. It is a standalone Wayland client, not part of a
compositor — which is deliberate, and is the architecture the rest of the
LucidOS desktop is built around (see "Why out-of-process" below).

## Layout

| Path | What it is |
|---|---|
| `macoswebproto/lucid_dock.cpp` | Primary implementation (C++20, GTK4, layer-shell) |
| `macoswebproto/lucid_dock.py` | Python implementation of the same design |
| `macoswebproto/CMakeLists.txt` | CMake build |
| `macoswebproto/build_lucid_dock.sh` | Direct g++ build |
| `macoswebproto/*.svelte`, `apps-config.ts` | Original web prototype the magnification curve came from |
| `lucidogdock.py` | Earlier GTK3 prototype |

## Building

    cd macoswebproto
    cmake -S . -B build && cmake --build build
    ./build/lucid_dock_cpp

Requires `libgtk-4-dev` and **`gtk4-layer-shell`** (the GTK4 version — *not*
`libgtk-layer-shell-dev`, which is the GTK3 one and will not work here).

## Compositor support

The dock anchors itself with `wlr-layer-shell`, so it works on compositors that
implement that protocol and does **not** work on GNOME:

| Compositor | Works | Note |
|---|---|---|
| KWin (Plasma) | Yes | Primary development target |
| Hyprland, Sway, COSMIC, wlroots-based | Yes | |
| **GNOME / Mutter** | **No** | Mutter does not implement `wlr-layer-shell` and has consistently declined to |
| X11 | No | Wayland only |

Without layer-shell the dock degrades to a plain undecorated window that cannot
anchor to the screen edge, cannot reserve space, and is positioned by guesswork.
That fallback is a development convenience, not a supported mode.

## Why out-of-process

The dock is a separate process from the compositor on purpose. A dock that
crashes should cost you a dock, not your session. The same rule applies to every
LucidOS shell component and to third-party extensions later, and it is what makes
"deep customization that cannot break your session" implementable rather than
aspirational. It also keeps the dock portable: it runs on any layer-shell
compositor today, so the choice of compositor for LucidOS stays open.
