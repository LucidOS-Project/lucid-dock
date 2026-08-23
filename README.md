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

Requires `libgtk-4-dev`, `libglib2.0-dev` (for `gio-unix-2.0`) and
**`gtk4-layer-shell`** (the GTK4 version — *not* `libgtk-layer-shell-dev`, which
is the GTK3 one and will not work here).

### gtk4-layer-shell availability

It is packaged, but only on recent releases. Verified against the Debian and
Ubuntu archives:

| Distro | Package | Status |
|---|---|---|
| Ubuntu 26.04 LTS (resolute) | `libgtk4-layer-shell-dev` 1.3.0-1 (universe) | **Available** — the LucidOS base |
| Ubuntu 25.10 (questing) | `libgtk4-layer-shell-dev` 1.0.4-2 (universe) | Available |
| Ubuntu 24.04 LTS / 22.04 | — | **Not packaged**, build from source |
| Debian 13 trixie | `libgtk4-layer-shell-dev` 1.0.4-2 | Available |
| Debian forky / sid | `libgtk4-layer-shell-dev` 1.3.0-1 | Available |
| Arch, Fedora | `gtk4-layer-shell` | Available |

So on the shipping target this is a one-line `Depends:`. Only development on a
24.04-era host needs the source build below.

### Building gtk4-layer-shell from source

For hosts without the package. Installs to `~/.local`, needs no root, and is
undone by deleting the files it lists:

    git clone --depth 1 https://github.com/wmww/gtk4-layer-shell.git
    cd gtk4-layer-shell
    meson setup build --prefix="$HOME/.local" \
        -Dexamples=false -Ddocs=false -Dtests=false \
        -Dintrospection=false -Dvapi=false
    meson compile -C build && meson install -C build

`-Dintrospection=false -Dvapi=false` drops the GObject-introspection and Vala
bindings; the dock is C++ and does not use them, and they are what pulls in
`gobject-introspection` and `valac` as build dependencies. If `meson`/`ninja`
are not installed either, `pip install --target=<dir> meson ninja` gets them
without touching the system.

`build_lucid_dock.sh` picks a `~/.local` install up automatically. For the CMake
build, export `PKG_CONFIG_PATH=$HOME/.local/lib/$(gcc -dumpmachine)/pkgconfig`
first; the rpath is then set from pkg-config, so no `LD_LIBRARY_PATH` at run
time.

Link order matters: `gtk4-layer-shell` works by shimming libwayland, so it has
to precede it on the link line. Getting this wrong fails at run time, not link
time. Both build files handle it.

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
That fallback is a development convenience, not a supported mode. The choice is
made at run time via `gtk_layer_is_supported()`, so one binary built with
layer-shell covers both cases and says which one it took.

### Testing layer-shell from a GNOME session

Mutter does not advertise `zwlr_layer_shell_v1` at all, so the anchored path
cannot be exercised from a stock GNOME session. Run a nested wlroots compositor
inside it instead:

    sudo apt install labwc      # or sway, cage
    labwc -s ./macoswebproto/lucid_dock_cpp

That opens a window containing a real layer-shell compositor, with the dock
anchored inside it. It is the only way to test the supported path short of
logging into KWin or sway.

## Animation

The magnification curve and its easing both come from
[macos-web](https://github.com/PuruVJ/macos-web) (`macoswebproto/DockItem.svelte`
is a copy of the reference). Easing is a **damped spring, the same one in both
directions** — matching the reference, which uses a single
`spring({damping: 0.47, stiffness: 0.12})` for growing and shrinking alike and
has no separate release path.

The C++ constants are derived from that spring rather than tuned by eye. Svelte's
spring is a frame-normalised discrete integrator; its characteristic polynomial
at 60 Hz gives poles that correspond to a continuous spring of
`omega_n = 24.32 rad/s, zeta = 0.783`, which is what `lucid_dock.cpp` uses.

Release, 2x back to 1x:

| Easing | 50% | 90% | settled |
|---|---|---|---|
| Spring (current, = macos-web) | 67 ms | 117 ms | **150 ms** |
| `RELEASE_TAU = 135 ms` (removed) | 94 ms | 311 ms | 622 ms |
| `MAGNIFY_TAU = 55 ms` (removed) | 38 ms | 127 ms | 253 ms |

The old easing was exponential decay with a *slower* time constant on release,
on the theory that letting go should feel like settling. That was wrong twice
over. Exponential decay is front-loaded and then crawls — it approaches the
target asymptotically and never arrives — so the last 10% of the shrink took
300 ms on its own and read as sluggish. A slightly underdamped spring instead
undershoots by 2% and comes back, which is the cue that reads as the icon
*arriving*. Note the spring is actually slower than the old easing over the
first half of the distance; being quicker to finish is what makes it feel fast.

The spring is integrated in closed form, not stepped. This dock runs anywhere
between 20 and 60 fps depending on the GSK renderer (see below), and a stepped
integrator would change the feel with the frame rate. Verified identical to
within frame quantisation at 20, 30, 60 and 144 fps.

### Where magnification is allowed to happen

Only while the pointer is on the dock panel. This is a hit test rather than a
widget boundary, because the window spans the whole monitor width under
layer-shell and `root_fixed_` spans the window — "in the widget" is not the same
question as "on the dock".

It used to be the widget boundary, plus a `DISTANCE_LIMIT` overhang, and the
result was that the dock magnified for a cursor up to 300 px away from it:

| Cursor, relative to the dock's left edge | Leftmost icon | Magnified |
|---|---|---|
| 300 px away | 58.1 px | 1% |
| 150 px away | 76.8 px | 33% |
| 50 px away | 93.9 px | 63% |
| at the edge | 102.7 px | **78%** |
| over the icon | 115.2 px | 100% |

So 78% of the effect was spent before the pointer reached the dock, and moving
across the dock delivered only the remaining 22% at the ends — the magnification
read as barely responding to the pointer, which is the symptom that led here.
The curve amplitude was never the problem.

Entering the dock is now a step change (57.6 -> 102.7 px at the left edge) that
the spring absorbs over ~150 ms. That is not a bug and it is not a workaround:
macos-web has the identical discontinuity, because `mouseleave` on `.dock-el`
drops the pointer to "infinitely far" in one event. The pop on entry *is* the
effect.

Vertically the hit region runs to the bottom of the window rather than the
bottom of the panel, so the `BOTTOM_MARGIN` strip still counts. A dock you
cannot hit by slamming the pointer into the screen edge is a dock you have to
aim at.

Also from the reference: app-launch bounce is 40 px over 400 ms with sine-in-out
easing, matching `tweened(0, {duration: 400, easing: sineInOut})`. Not yet
implemented from it: the auto-hide slide (`transform: translate3d(0, 200%, 0)`
over 300 ms), group dividers, and the hover tooltip.

## Performance notes

Measured on a MacBook Pro (Retina, 15-inch, Mid 2015): Intel Iris Pro 5200
(Haswell GT3), 2880x1800 panel at scale factor 2, GTK 4.14.5.

Run the built-in benchmark with `LUCID_DOCK_BENCH=180 ./lucid_dock_cpp`. It
sweeps a synthetic pointer across the dock and reports where the frame time
goes. It is the source of every number below. `LUCID_BENCH_IDLE=1` ticks without doing layout, to separate per-frame
work from window and compositor cost.

| Configuration | fps | `layout_panel()` p50 |
|---|---|---|
| `GSK_RENDERER=gl` (removed in GTK 4.18) | **60** | 0.071 ms |
| `GSK_RENDERER=ngl` (GTK 4.14 default) | 30 | 0.137 ms |
| `GSK_RENDERER=vulkan` (GTK 4.16+ default on Wayland) | **20** | 0.058 ms |
| Any renderer, idle | 60 | — |

`layout_panel()` costs ~0.1 ms in every column, so layout is not the bottleneck
and the ranking is entirely the renderer's. Note that the fastest column is the
one doing the *most* layout work per frame — the renderer differences dwarf
anything the dock does.

Vulkan here is `hasvk`, Mesa's legacy Haswell driver, which announces itself
with *"Haswell Vulkan support is incomplete"*. On GTK 4.14, launch with
`GSK_RENDERER=gl` on pre-Gen8 Intel graphics.

**This workaround does not survive to the shipping target, and the measurement
it rests on does not describe it either.** Both caveats are now checked:

- Ubuntu 26.04 ships GTK **4.22**. The old `gl` renderer was removed in 4.18 —
  `GSK_RENDERER=gl` there prints *"The old GL renderer has been removed"* and
  falls back. So the workaround becomes an inert environment variable rather
  than a broken one, which is the good version of this outcome.
- GTK 4.16 and later default to the **Vulkan** renderer on Wayland, not `ngl`.
  The 30 fps figure above is a measurement of a renderer that is not the default
  on the target and cannot be selected as a workaround there either.

Put together, the 60 fps column is the one that ceases to exist on the target,
and the renderer that replaces it as the default measures **slowest of the
three** on this GPU. The question worth answering is therefore not "why is `ngl`
slow on GTK 4.14" — it is whether this class of GPU can drive the dock at 60 fps
at all under GTK 4.22, where the only two candidates are `ngl` and a Vulkan
driver that ships with a "support is incomplete" warning. That is a measurement
to take on 26.04, not an argument to have here. Until then `GSK_RENDERER=gl`
costs nothing on 4.14 and buys 3x over the 26.04 default.

Renderer choice still belongs in a per-GPU capability tier for the desktop as a
whole rather than in one application — the same problem as the visual tiers.

## Why out-of-process

The dock is a separate process from the compositor on purpose. A dock that
crashes should cost you a dock, not your session. The same rule applies to every
LucidOS shell component and to third-party extensions later, and it is what makes
"deep customization that cannot break your session" implementable rather than
aspirational. It also keeps the dock portable: it runs on any layer-shell
compositor today, so the choice of compositor for LucidOS stays open.
