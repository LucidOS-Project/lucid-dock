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

## Performance notes

Measured on a MacBook Pro (Retina, 15-inch, Mid 2015): Intel Iris Pro 5200
(Haswell GT3), 2880x1800 panel at scale factor 2, GTK 4.14.5.

Run the built-in benchmark with `LUCID_DOCK_BENCH=180 ./lucid_dock_cpp`. It
sweeps a synthetic pointer across the dock and reports where the frame time
goes. `LUCID_BENCH_IDLE=1` ticks without doing layout, to separate per-frame
work from window and compositor cost.

| Configuration | fps |
|---|---|
| `GSK_RENDERER=gl` | 60 |
| `GSK_RENDERER=ngl` (GTK 4.14 default) | 30 |
| Either renderer, idle | 60 |

`layout_panel()` itself costs ~0.1 ms, so layout is not the bottleneck. The
split is entirely in the renderer: on this GPU the newer `ngl` renderer halves
the frame rate for the dock's per-frame redraw while the older `gl` renderer
sustains 60. **Until that is understood, launch with `GSK_RENDERER=gl` on
pre-Gen8 Intel graphics.**

Two caveats worth verifying before relying on this:

- GTK 4.16 reworked the renderers and the old `gl` renderer may not exist on
  Ubuntu 26.04. This workaround has an expiry date. *(unverified against 26.04)*
- Renderer choice belongs in a per-GPU capability tier for the desktop as a
  whole, not in one application. This is the same problem as visual tiers.

## Why out-of-process

The dock is a separate process from the compositor on purpose. A dock that
crashes should cost you a dock, not your session. The same rule applies to every
LucidOS shell component and to third-party extensions later, and it is what makes
"deep customization that cannot break your session" implementable rather than
aspirational. It also keeps the dock portable: it runs on any layer-shell
compositor today, so the choice of compositor for LucidOS stays open.
