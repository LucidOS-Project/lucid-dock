# Lucid Dock

The LucidOS dock: a magnifying, animated dock implemented as a **GTK4 +
gtk4-layer-shell** client. It is a standalone Wayland client, not part of a
compositor — which is deliberate, and is the architecture the rest of the
LucidOS desktop is built around (see "Why out-of-process" below).

## Layout

| Path | What it is |
|---|---|
| `macoswebproto/lucid_dock.cpp` | Primary implementation (C++20, GTK4, layer-shell) |
| `macoswebproto/dock_config.h` | Config + `.desktop` catalogue, shared by both binaries |
| `macoswebproto/toplevel_source.{h,cpp}` | Which applications are running, and where that answer comes from |
| `macoswebproto/protocols/` | Vendored Wayland protocol XML (the two foreign-toplevel protocols) |
| `macoswebproto/tests/` | A fake compositor for the protocol no compositor here implements |
| `macoswebproto/tools/render_panel.c` | Renders the panel through GSK to a PNG, for measuring what GTK actually draws |
| `.github/workflows/ci.yml` | Builds both ways, runs the protocol tests and the memory budget |
| `macoswebproto/dock_settings_page.cpp` | The settings UI, as a widget |
| `macoswebproto/lucid_dock_settings.cpp` | ~20 line wrapper making that widget a window |
| `macoswebproto/lucid_dock.py` | Python implementation of the same design |
| `macoswebproto/CMakeLists.txt` | CMake build |
| `macoswebproto/build_lucid_dock.sh` | Direct g++ build |
| `macoswebproto/write_build_stamp.sh` | Generates the build stamp; shared by both builds |
| `macoswebproto/build_stamp.h` | Declares the stamp the two binaries print |
| `macoswebproto/*.svelte`, `apps-config.ts` | Original web prototype the magnification curve came from |
| `lucidogdock.py` | Earlier GTK3 prototype |

## Building

    cd macoswebproto
    cmake -S . -B build && cmake --build build
    ./build/lucid_dock_cpp

Requires `libgtk-4-dev`, `libglib2.0-dev` (for `gio-unix-2.0`),
`libwayland-dev` and `libwayland-bin` (for `wayland-scanner`), and
**`gtk4-layer-shell`** (the GTK4 version — *not* `libgtk-layer-shell-dev`, which
is the GTK3 one and will not work here).

The two foreign-toplevel protocols are generated from XML vendored in
`protocols/`, so `wayland-protocols` is not a build dependency. That is not
tidiness: `wlr-protocols` is not packaged on Debian or Ubuntu at all, and
pinning both files means the marshalling code cannot change underneath a build.
See "Running applications".

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

### The build stamp

Both binaries print their git rev and build time at startup and accept
`--version`, so "is this current?" is answerable from the running process
rather than by comparing file timestamps by hand:

    ** Message: lucid-dock b0039dd+dirty built 2026-09-04 22:15:06

`write_build_stamp.sh` writes that into a generated translation unit, and both
build systems call it, so there is one format and one definition of "dirty"
rather than two that drift.

Three details that are all deliberate:

- **It is a `.cpp`, not a header the sources include.** The stamp has to be
  recomputed every build to be true, and anything `lucid_dock.cpp` includes
  would then recompile it every build -- 5.7 s, against 0.01 s for the
  generated file alone. A no-op `cmake --build` costs 0.57 s and recompiles
  only the stamp.
- **CMake regenerates it from a custom target, not `execute_process`.** The
  latter runs at configure time, so the rev would freeze at whatever was
  checked out when `cmake` was last run and go quietly stale.
- **The values are `extern` constants, not macros with an `unknown` fallback.**
  That fallback is how the CMake build came to emit binaries announcing
  themselves as `lucid-dock unknown built unknown` for as long as nobody
  looked: it never defined the macros and the fallback made that silent. A
  missing stamp is now a link error that produces no binary, which is the
  failure mode this project wants.

Known gap: "dirty" is `git diff --quiet`, which does not see staged-but-
uncommitted changes, so a build from a fully staged tree reports a clean rev.

## Compositor support

The dock anchors itself with `wlr-layer-shell`, so it works on compositors that
implement that protocol and does **not** work on GNOME:

| Compositor | Works | Note |
|---|---|---|
| KWin (Plasma) | Yes | Primary development target |
| Hyprland, Sway, COSMIC, wlroots-based | Yes | |
| **GNOME / Mutter** | **No** | Mutter does not implement `wlr-layer-shell` and has consistently declined to |
| X11 | No | Wayland only |

A second protocol matters for a second reason -- see "Running applications"
below. Neither is required to start; both change how well the dock works.

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

## Running applications

The dot under an icon, and the unpinned icons that appear behind a separator,
both come from one question: which applications are running? There are three
ways to answer it and the dock picks the best one available at startup.

| Source | Protocol | How it updates | Reports |
|---|---|---|---|
| `ext` | `ext-foreign-toplevel-list-v1` | Event-driven | Windows |
| `wlr` | `zwlr-foreign-toplevel-management-v1` | Event-driven | Windows |
| `proc` | none | Polled, every 4 s | Processes |

Which one is in use is printed at startup, because "the dot is wrong" is three
different bugs depending on the answer:

    running-app detection: wlr -- zwlr-foreign-toplevel-management-v1 (event-driven, windows)

`LUCID_DOCK_TOPLEVEL_SOURCE=ext|wlr|proc` forces one. It names exactly one
source and does not fall through to a better one, because the point of the
variable is to reproduce a specific source's behaviour -- it does still fall
back to `proc` if the named protocol is absent, since the alternative is no
dock.

### Why a process is the wrong thing to count

The `proc` source walks `/proc`, collects process names, and matches them
against names derived from each `.desktop` file's `Exec` line. It needs nothing
from the compositor, which is its only virtue. It is wrong in both directions:
it misses applications whose binary name does not resemble their desktop id,
which is most Flatpaks, and it reports background services as running
applications.

The second one is not hypothetical. Three of the three dots showing on a stock
Zorin session were wrong:

| Desktop entry | `proc` says | Actual process | Windows |
|---|---|---|---|
| `org.gnome.Calendar.desktop` | RUNNING | `gnome-calendar --gapplication-service` | none |
| `org.gnome.Software.desktop` | RUNNING | `gnome-software --gapplication-service` | none |
| `org.gnome.Nautilus.desktop` | RUNNING | `nautilus --gapplication-service` | none |

All three are D-Bus activated and sitting idle with nothing on screen. A dock
that claims they are open is not reporting a subtle edge case, it is reporting
the opposite of the truth.

The foreign-toplevel protocols answer the question that was actually being
asked. They enumerate toplevels -- windows -- so a service with no window is
not running, and an application is matched by the `app_id` its window reports
rather than by guessing at its binary name.

### What it cost to ask the wrong question

Measured on the machine in "Performance notes", with `LUCID_DOCK_TIMERS=1`:

| | Cost on the UI thread |
|---|---|
| `proc`, per 4 s tick | **16.4 - 19.8 ms**, whether or not anything changed |
| `ext`/`wlr`, nothing changed | 0 ms -- no timer exists |
| `ext`/`wlr`, a window opened or closed | 1.9 - 2.8 ms |
| `ext`/`wlr`, 2nd window of an app already running | 0 ms -- no callback at all |

The first change after startup also pays a one-off 14-28 ms to load the
`.desktop` catalogue, which is lazy and would otherwise have been paid by the
first `/proc` tick. It is not a per-change cost and is not counted above.

A frame at 60 Hz is 16.7 ms, so the polled path dropped a frame every four
seconds, permanently, to recompute an answer that changes a few times an hour.

Two separate things went away, and it is worth keeping them apart. The `/proc`
walk itself was 16-20 ms. On top of that, `refresh_running_indicators()` walked
the whole catalog to decide whether anything relevant had changed -- 3.7-8.7 ms
-- because the raw `/proc` set churns constantly and comparing it wholesale
never matches. **An event-driven source has already answered that question**:
its callback fires only when the set of `app_ids` changed. That check is now
skipped entirely on the event path, and kept on the polled one where it is still
load-bearing.

The last row is the reference counting. Two windows of one application hold one
key between them, so opening a second Firefox window, or closing one of two,
produces no dock work at all -- and, more importantly, does not put the dot out
while a window is still open.

### One key per application, one entry per window

`running_keys()` answers "is anything with this app_id open", which is the
question a dot under an icon asks. `toplevels()` is the same information before
it has been reduced to that: one `ToplevelInfo` per open window, carrying the
app_id, the title, and -- under `ext` only -- the stable identifier. Two windows
of one application are one key and two entries.

The list is in the order the compositor announced windows, oldest first, which
is the order a taskbar would lay buttons out in and the one property a hash
container cannot promise. It is empty under `proc`, which has no window
information at all -- ask `reports_windows()` rather than testing it for
emptiness, because "no windows" and "cannot see windows" are different answers.

This exists because the compositor already sends a title and an identifier with
every toplevel and the dock was receiving both and dropping them on the floor.
Keeping them is not building a taskbar; it is declining to destroy information
we are handed, which is far cheaper now than adding the path for it later.

**The change callback still fires only on key-set changes.** A second window of
a running application does not move the key set, and neither does a retitle --
and the dock must not rebuild for either, or a browser would rebuild it on every
page it navigated to. Verified: opening a second window of an already-running
application produces no callback at all. A per-window consumer needs a signal
this one does not give and should add one rather than widening this, which
exists to keep the dock still.

### Matching a window to a desktop file

`app_id` and desktop file id are related by convention, not by rule. The
candidates for an entry are, in order of how much they are trusted:

1. the desktop file id, less `.desktop` (`org.gnome.Nautilus`)
2. `StartupWMClass`, which is the Desktop Entry spec's own answer to exactly
   this question, and is treated as ground truth when present
3. the trailing segment of a reverse-DNS id (`org.gnome.Nautilus` -> `nautilus`)
4. the `Exec` line's binary name

Everything is lower-cased on both sides. Step 3 is a guess and is **only made
when `StartupWMClass` is absent**: two vendors' Calculator both reduce to
`calculator`, and lighting up the wrong icon is worse than missing a dot. Where
the desktop file has told us the answer, the dock does not guess as well.

`StartupWMClass` was not previously parsed at all; `dock_config.h` now reads it.

The `proc` source keeps its own, separate candidate list built from the `Exec`
line, because a process name and an `app_id` are not interchangeable and mixing
the two lists would invent matches that neither source justifies.

### Seeing what it thinks

    LUCID_DOCK_RUNNING=1 ./lucid_dock_cpp

prints the running key set and a per-icon verdict at startup and on every
change:

    running state (after change) from 'wlr': 2 keys
      keys: org.gnome.calculator org.gnome.texteditor
      org.gnome.Calendar.desktop                   -        via -
      org.gnome.Calculator.desktop                 RUNNING  via org.gnome.calculator

Geometry got a printer for the same reason. This is state that decides what is
drawn, does not show up in a screenshot, and has been wrong before. Printing
the raw key set matters as much as the per-icon verdict: a missing dot is
either "the application is not in the set" or "it is in the set under a name
none of the candidates guessed", and those have completely different fixes.

The key dump is suppressed for `proc`, where the set is every process on the
machine rather than one entry per window.

### Protocol availability, as measured rather than as documented

Checked by binding the registry and listing globals, not by reading changelogs:

| Session | `zwlr_layer_shell_v1` | `ext_foreign_toplevel_list_v1` | `zwlr_foreign_toplevel_manager_v1` |
|---|---|---|---|
| GNOME / Mutter 46 | no | no | no |
| sway 1.9 (wlroots 0.17), Ubuntu 24.04 | v4 | **no** | v3 |
| sway 1.10+ (wlroots 0.18+), KWin 6, Hyprland, COSMIC, niri | yes | yes | yes |

`ext-foreign-toplevel-list-v1` is the standard and is what the dock prefers.
It landed in wlroots 0.18, so it is **not** available on the newest sway Ubuntu
24.04 packages -- which is the whole reason the `wlr` source exists rather than
`ext` alone. `wlr` is also what KWin and Hyprland have had longest, so it is the
wider net today and the one that will age out later.

Note that Mutter implements neither of these and neither `wlr-layer-shell`, so
on GNOME the dock cannot anchor *and* falls back to `proc`. Everywhere the dock
can actually anchor, one of the two protocols is available.

### Testing this

The `wlr` path is testable for real, from a GNOME session:

    WLR_BACKENDS=wayland sway -c <config that execs the dock>

The `ext` path is not, on a 24.04-era host: nothing available implements it.
`macoswebproto/tests/` therefore contains a compositor that implements
`ext-foreign-toplevel-list-v1` and nothing else -- no surfaces, no seat, no
rendering -- and plays a fixed script of toplevels at the dock's real
`ToplevelSource`:

    ./macoswebproto/tests/run_toplevel_source_tests.sh

It covers initial enumeration on bind, a toplevel appearing later, two
toplevels sharing an `app_id`, a retitle, an `app_id` changed and committed with
`done`, and `closed`. It asserts the whole sequence of key sets rather than
grepping for individual events, so a missing event and a spurious one both fail,
and it asserts the window list separately -- on content rather than on which
sample it landed in, since the fake's script and the probe's sampling drift
against each other and pinning a state to a timestamp would be a flaky test
rather than a stricter one.

This is a stand-in for running against a real compositor, not a replacement for
it. It proves the client speaks the protocol correctly; it cannot prove a real
compositor sends what this one sends. Re-run the `wlr` scenario against a real
`ext` compositor once one is available on the host.

## Configuration

`~/.config/lucid/dock.conf`, GKeyFile (`.desktop`) syntax. Not GSettings: a
schema has to be compiled into `/usr/share/glib-2.0/schemas/` to exist at all,
which is install-time friction for a component whose pitch is "clone, build,
run". Not TOML or JSON either — both need a dependency GLib already replaces.
`~/.config/lucid/` is shared with the other LucidOS components so a settings
application has one directory to look in.

| Key | Meaning |
|---|---|
| `Pinned` | Desktop file IDs, in dock order |
| `DividersBefore` | Draw a separator before each of these |
| `Magnification` | On/off |
| `MaxScale` | Peak magnification, 1.0–3.0 (reference: 2.0) |
| `Spread` | How far magnification reaches in icon widths, 1.5–8.0 |
| `IconSize` | Idle icon size in px, 24–80 (0 = the default 58) |
| `LayoutMode`, `Position` | Written and round-tripped, **not yet honoured** |

**The file is the only source of truth.** The right-click menu does not hold
settings in memory and hand them out — it writes the file, and the file is what
gets read back. A `GFileMonitor` applies changes live, so a settings
application later edits the same file, the dock notices, and neither side needs
to know the other exists: no IPC, no D-Bus, no protocol to design. Reload never
writes, so a write cannot trigger a write and there is no loop to suppress.

Changing `Pinned` or `DividersBefore` rebuilds the items; everything else is
applied in place. `MaxScale` resizes the surface rather than clipping the icons.

### The dock used to crash on every compositor it was built for

`load_system_dock_apps()` called `g_settings_new("org.gnome.shell")` unguarded
to get its app list. A missing GSettings schema is a `g_error()`, which aborts:

    GLib-GIO-ERROR **: Settings schema '...' is not installed
    Trace/breakpoint trap        exit=133

So on KDE, sway, Hyprland and COSMIC — every compositor where layer-shell
actually works — the dock died before it drew a frame. It ran only on GNOME,
the one place it cannot anchor.

GNOME favourites are now an *import*, not a dependency: on first run the schema
is probed with `g_settings_schema_source_lookup()` and used if present, and
`builtin_default_pinned()` fills in from installed applications if not. Either
way the result is written to disk, so from the second run GNOME is never
consulted again. Verified by running with `XDG_DATA_DIRS` pointed at an empty
directory: previously exit 133, now it starts and picks six sensible apps.

### Two layout bugs the config work exposed

**The dock jumped to the left edge on any config change.** Reload called
`gtk_widget_set_size_request(window_, -1, height)` to update only the height,
but `-1` does not mean "leave the width alone", it means "no width request" --
so it cleared the monitor-width pin, the window auto-sized down to the panel,
and centring against *that* width put the dock at x = 0. Both dimensions are now
set together in one `apply_window_size()` that build and reload both call.

**Icons twitched under a slowly moving pointer.** An icon's width has to be a
whole number of pixels -- it is a raster size -- but positions were being
accumulated from those *rounded* widths, so one icon's rounding flipping by 1 px
shifted every icon after it, and integer division for the centre shifted the
whole panel again. Creeping the pointer 0.5 px per frame:

| | largest single-frame jump | frames stepping >= 1 px |
|---|---|---|
| positions from rounded widths | 2.00 px | **51%** |
| positions in floating point | 0.50 px | 0% |

Sizes are quantised, positions are not: they accumulate from the unrounded
widths and go through `gtk_fixed_move()`, which takes doubles. Moving every icon
every frame instead of only on a rounding change costs nothing measurable --
`layout_panel()` p50 0.088 ms, still 60 fps.

## Settings

    ./lucid_dock_settings

Also reachable from the dock's right-click menu, which looks for the binary
beside itself first so a build tree works uninstalled, then on `PATH`, and falls
back to opening the config file if neither is there.

**It is a widget, not a window,** and that is the entire reason it is a separate
file. `dock_settings_page.cpp` exposes exactly one symbol:

    GtkWidget* lucid_dock_settings_page_new(void);

Today a ~20 line `main()` wraps that in a `GtkWindow`. When LucidOS Settings
exists it compiles the same file and drops the same widget into a stack page --
no extraction, no rework. Built as a window instead, that step is a gutting job.

It talks to the dock through `~/.config/lucid/dock.conf` and nothing else: it
writes, the dock's `GFileMonitor` notices, the change applies live. No IPC, no
D-Bus, no protocol between the two processes, and no reason for either binary to
know the other is running.

Controls: size, magnification on/off, maximum size, spread, and the pinned list
with reorder, remove, add, and a per-row "Separator before" toggle -- a
separator belongs to the app it precedes, so that is where the control for it
belongs, rather than in a second list to be kept in step by hand.

**Reset to Defaults** restores size, magnification and spread only. The pinned
list is the user's own arrangement, not a setting with a sensible default, and
discarding it because someone wanted the sliders back would be a bad trade. The
values come from `DockConfig`'s own member initialisers rather than being
written out a second time, so there is one place for them to be wrong.

`dock_config.h` is header-only. The shared surface is a config struct, a
`.desktop` catalogue, and the functions that read and write them; a static
library would be more build system than that is worth.

## The name label

Hovering an item shows its name above it, immediately.

Immediately is the whole specification. The reference has **no transition on it
at all** -- `display: none` to `display: block` -- so the label is up on the
frame the pointer arrives. GTK's stock tooltip, which is what the dock used
before, waits about 500 ms by design; that delay is the entire difference
between "the dock is telling me what this is" and "a tooltip appeared".

The label is parented to `root_fixed_` rather than the panel, so it can sit
above the panel's top edge, and it is repositioned every frame from the icon's
animated width so it rides the magnification instead of trailing it. GTK has no
`backdrop-filter`, so the reference's 50%-white-plus-5px-blur is approximated
with a slightly more opaque fill.

### It goes away on its own

Instant in, slow out. The label holds at full opacity for `TOOLTIP_HOLD` (3 s)
and then fades over `TOOLTIP_FADE` (0.6 s) -- gone at 3.6 s -- easing with the
same `sine_in_out` the launch bounce uses so the dock has one vocabulary of
motion rather than one per animation.

The hold was 4 s first, then 2.75, and is 3, settled by feel because that is
the only way it can be settled. The label has been read inside the first
second; everything after that is it sitting on the desktop having already done
its job. There is no correct value here and no measurement that would produce
one -- these are the two constants to change if it ever feels wrong again.

The asymmetry is the point. Appearing instantly is what makes the label read as
an answer to the pointer -- that is the whole reason it is not GTK's stock
tooltip, which waits 500 ms. Fading it *in* would put that delay straight back.
But a label that has been up for four seconds has already been read, and
holding it for as long as the pointer happens to rest on an icon leaves a
bright rectangle sitting over the desktop. Nothing is gained by keeping it.

The rules, all four verified end to end by driving a real pointer in a nested
sway with `swaymsg seat - cursor set`:

| | |
|---|---|
| Pointer reaches an icon | up at full opacity, +20 ms |
| Held there | fade begins at +3.002 s, gone 0.617 s later |
| Still on the same icon after it has gone | stays gone -- it is not re-shown every frame |
| Pointer reaches a *different* icon | up again instantly, with a fresh hold -- not the remainder of the old one |
| Mid-fade, pointer reaches another icon | back to full opacity, hold restarts |
| Pointer leaves the dock | hidden immediately |

The hold is a one-shot `g_timeout`, not a check on the frame clock. A pointer
resting on an icon is exactly the case where the dock has stopped ticking, and
spinning the frame clock for the whole hold to discover that the hold has
elapsed would undo the work that stops it. The timer arms on show, is cancelled
on hide, and re-arms when the label moves to another icon; the fade itself runs
on the tick, which the timer starts.

`LUCID_DOCK_TOOLTIP_TRACE=1` logs shown / fade start / faded out / hidden with
timestamps. The timings are the whole feature and none of them are visible in a
screenshot. Instrumenting the per-frame opacity during a fade gives 56 samples
across two fades and the expected ease-in-out shape -- 1.000, 0.939, 0.792,
0.592, 0.376, 0.183, 0.050, 0.000 -- which is what distinguishes a fade from a
label that sits still and then vanishes.

It needs headroom, so the surface is now 198 px rather than 160 at `MaxScale`
2.0. **Known consequence:** the dock's surface is monitor-wide and now taller,
and a layer surface takes pointer input across its whole area unless an input
region is set, which GTK does not expose. Worth fixing before this ships.

### The surface is a constant size

`SURFACE_HEIGHT` is a compile-time constant sized for the largest configuration
the dock will accept, not for the current one.

Sizing it to the current configuration made the dock walk down the screen every
time magnification was raised. Growing a surface is only free under layer-shell,
where it is anchored to the bottom edge and grows upward; on the fallback path
the compositor owns placement, keeps the window's top where it is, and the extra
height comes out of the bottom. Each increase pushed the dock further down, and
only restarting put it back.

A constant surface cannot do that on either path. Verified across `MaxScale`
2 -> 3 and `IconSize` 58 -> 80: window stays 1440x323, the panel's bottom edge
stays at the same y, and the panel grows *upward* (79 px to 101 px) as it should.

Configuration is clamped to what that constant can contain: icon size 24-80,
magnification 1.0-3.0.

### Input region

A layer surface takes pointer input across its whole area by default, and this
one is monitor-wide. Without an input region the dock silently swallows every
click in a band across the bottom of the screen, including on the windows behind
it. That was true at 160 px too; a constant 323 px surface just makes it
impossible to ignore.

`gdk_surface_set_input_region()` restricts it to the rectangle the dock actually
occupies -- the same rectangle `pointer_is_on_panel()` accepts, so what the
compositor delivers and what the dock counts as "on the dock" cannot disagree.
It follows the icons as they magnify, and is only pushed to the compositor when
it actually changes.

### The shadow was a slab, not a shadow

Reported as "really blocky when a white application is behind it", and that is
exactly the condition that exposes it.

    2px 5px 19px 7px rgba(0, 0, 0, 0.3)
                     ^^^ spread

Spread is not blur. It inflates the shadow rectangle by 7 px on every side at
the full 30% black *before* anything is blurred, so the panel sat inside a
near-solid dark slab and the 19 px blur only ever softened that slab's outer
edge. Over a dark wallpaper it is invisible. Over a white application it is a
grey brick with the panel sitting in it.

Removing the spread was right and **was not the fix**, which is worth recording
because the first attempt shipped believing it was.

The shadow looks blocky because it is **clipped**. Nothing renders outside the
surface; the panel's bottom edge sits `BOTTOM_MARGIN` (8 px) above the
surface's; and a shadow needs roughly 30 px to fade out. So the bottom of it is
cut off flat, straight across the full width of the dock. A shadow that stops
abruptly is a rectangle however soft its blur is, and no amount of blur tuning
fixes a cut.

So the vertical offset is the thing that had to go. Offsetting a shadow
downwards aims its darkest part at the one direction with no room:

        0 0 6px rgba(0, 0, 0, 0.22),
        0 0 14px rgba(0, 0, 0, 0.18);

Measured by rendering the stylesheet through GSK itself (`tools/render_panel`),
over white. "Clip step" is how many grey levels the last visible row differs
from the background -- that is the size of the hard line:

| | clip step at 8 px | shadow at the panel's side |
|---|---|---|
| Original, `2px 5px 19px 7px` | **49** | 233 |
| Spread removed, offsets kept | **23** | 233 |
| Offsets removed (current) | **5** | 234 |

The side shadow is unchanged, so the dock has not lost any depth; only the part
that was being sliced off is gone.

**Where this actually shows.** Anchored, the cut lands exactly on the physical
screen edge -- `gtk_layer_set_margin(..., edge, 0)` -- where there is nothing
beyond it to compare against, which is how the macOS dock's shadow behaves too.
It is glaring on the GNOME fallback, where the window floats mid-screen by
guesswork and the cut is a line across the middle of the display. That is the
path a developer on GNOME looks at all day, which is how it got reported.

**The 1px dark ring above it is load-bearing and should not be tidied away.**
The panel is 40% white, so over a white application it is very nearly invisible
and the ring is the only thing still saying where the dock is. Against white,
the border and the shadow are the dock.

## Right-click menu

Right-click anywhere on the dock: Magnification, Edit Configuration File…,
Reload Configuration, Quit Dock.

Anywhere, deliberately — including on icons. The obvious design is macOS's,
where the icon and the dock have separate menus, and that is still where this
ends up. But the dock's own targets are two 10 px end caps and eleven 10 px
inter-item gaps, 16% of an 826 px panel, and the icons *magnify as you approach*
— so you would be aiming at a 10 px target that moves out from under the cursor
as you reach for it. Until icons have their own menu there is nothing to
conflict with, so the gesture takes the whole dock in the `CAPTURE` phase. When
the icon menu lands it claims the bubble phase and the dock menu keeps the
background and the separators.

Separators help here too, which is the other reason they exist: a separator slot
is `DIVIDER_WIDTH + 2 * DIVIDER_MARGIN` = 17 px, it does not magnify, and it
looks like something you can click. macOS puts Dock Settings on exactly that
target.

"Edit Configuration File…" stands in for "Dock Settings…" until the settings
widget exists. It opens the same file the settings application will write, so
the entry point moves later without the plumbing under it changing.

### Separators

From the reference: `apps-config.ts` has `dock_breaks_before` and `Dock.svelte`
renders a `.divider`. A separator spans the drawn panel's interior rather than
the item slot — the slot is tall enough for a magnified icon, and a separator
that tall would stick out of the panel along with the icons.

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
| Spring, `omega_n = 30` (current) | 50 ms | 100 ms | **133 ms** |
| Spring, `omega_n = 24.32` (= macos-web exactly) | 67 ms | 117 ms | 150 ms |
| `RELEASE_TAU = 135 ms` (removed) | 94 ms | 311 ms | 622 ms |
| `MAGNIFY_TAU = 55 ms` (removed) | 38 ms | 127 ms | 253 ms |

### The shape is not animated; entering and leaving is

Springing each icon's width made the dock a low-pass filter on pointer position.
Sweep quickly and every icon holds a large target for only a few frames, so none
of them get large and they converge on looking the same size:

| Sweep speed | Peak reached, per-icon spring | Envelope model |
|---|---|---|
| slow drag | 112.3 px (98%) | 115.2 px (100%) |
| normal move | 105.9 px (92%) | 115.2 px (100%) |
| fast flick | 83.2 px (72%) | 115.2 px (100%) |
| very fast flick | 71.3 px (62%) | 115.2 px (100%) |

The first attempt at this exposed a `TrackingSpeed` knob to tighten the spring.
That was the wrong fix and it was wrong in an instructive way: **it was the same
spring that shapes the shrink**, so tightening the tracking necessarily sped up
the release. Two unrelated things behind one control.

macOS does not filter the shape at all. Icon size is a direct function of
pointer position, so a fast sweep magnifies exactly as much as a slow one; what
animates is *entering and leaving the dock*. So the shape is applied instantly
and a single 0..1 envelope is sprung -- 1 while the pointer is on the dock, 0
when it is not -- with each width being `base + envelope * (shape - base)`.

Release animation and magnification response are now independent, the
attenuation is gone rather than tuned, and there is no knob for it because there
is no longer anything to tune. `layout_panel()` also dropped from 0.088 ms to
0.033 ms, since eleven springs became one.

The pointer position is remembered after the pointer leaves, until the envelope
finishes decaying -- otherwise the shape would snap flat and the envelope would
be easing nothing.

That remembered position is *only* the shape. The envelope's target comes from
where the pointer is now, and deriving it from the remembered position instead
makes the two circular -- the target stays 1 because the position is set, and
the position is only cleared once the target reaches 0. The dock then magnifies
on first hover and never shrinks again, which is exactly what it did.

**The benchmark now lets go of the pointer.** Its last quarter runs with the
pointer off the dock and it reports whether the icons actually returned to their
idle size:

    after release    : envelope 0.0000, widest icon 57.6 px (idle is 57.6) -- SHRANK

This dock has now twice shipped a bug where it magnified and then would not come
back -- once from a 622 ms exponential tail, once from that circular target.
Both were invisible to a benchmark that only ever swept and never released,
because "it magnifies" and "it un-magnifies" are separate claims and only the
first is obvious while you are working on it.

`omega_n` is set ~23% above the reference deliberately: same spring shape, run
a little quicker, because the reference tracks a touch lazily for a dock driven
with a mouse. `zeta` is what sets the *shape*, so `omega_n` is the knob to turn
for reaction time without changing how the motion reads.

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

### Panel height, and why icons overflow it

The panel is sized for an **unmagnified** icon: 79 px, of which 58 px is the
icon. Magnified icons are 115 px and simply grow out of the top of it, ending up
49 px above the panel edge.

It used to be sized for the magnified case — 144 px tall, permanently, so it
read as a slab at idle and reserved space it only needed while being pointed at.
That is not what macOS does, and not what the reference does either: `.dock-el`
has a fixed height and the `<img>` inside it just overflows.

Reproducing that means the drawn panel and the row of item slots are **two
separate widgets**. `panel_bg_` carries the `.dock-panel` styling and is the
short one; `panel_fixed_` is a tall, undrawn layout container stacked above it.
Everything vertical is placed relative to one line — the icon baseline, where
the bottom of every icon sits regardless of size — so icons grow upward out of
the panel rather than the panel growing to contain them.

    window            : 1440 x 160
    panel (drawn)     : y  73 .. 152   height 79
    icon baseline     : y 139
    idle icon top     : y  81   (inside the panel)
    magnified top     : y  24   (49 px above the panel)
    item row (no draw): y  16 .. 160
    screen-edge gap   : 8 px

Run `LUCID_DOCK_GEOM=1 ./lucid_dock_cpp` to print that for the current display.
Geometry is the part of this dock that has been wrong most often and it does not
show up in a screenshot, so it is printable.

Exposing the item slots also exposed that they were never styled: a `GtkButton`
brings Adwaita's background, border and shadow with it, which drew a visible
128 px vertical band behind every icon. Invisible while the panel was tall
enough to cover them; obvious once it wasn't. The dock item is a hit target, not
a button, and `.dock-item` now says so.

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

Vertically the region is whatever the dock currently occupies: the panel, plus
however far the icons are sticking out above it at that moment. The panel alone
would mean the magnified part of an icon is not part of the dock, so pointing at
a big icon would shrink it. The DOM gets this free — an overflowing `<img>` is
still a descendant of `.dock-el`, so `mouseleave` does not fire over it.

Downward it runs to the bottom of the window rather than the bottom of the
panel, so the `BOTTOM_MARGIN` strip still counts. A dock you cannot hit by
slamming the pointer into the screen edge is a dock you have to aim at.

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

### Re-measured, 4 September 2026

All three renderers now sustain 60 fps, under both the `performance` and
`power-saver` profiles, on battery:

| Renderer | fps | `layout_panel()` p50 |
|---|---|---|
| `gl` | **60** | 0.022 ms |
| `ngl` | **60** | 0.034 ms |
| `vulkan` | **60** | 0.036 ms |

**So the workaround below is no longer needed, and its explanation was wrong.**
The earlier table is kept underneath because the measurement was real, but the
conclusion drawn from it -- that the ranking was "entirely the renderer's" --
does not survive re-measurement. Neither the renderer nor the power profile
predicts the frame rate today.

What changed between the two measurements is code, not configuration: the
per-icon springs became one envelope, positions gained a `drawn_x` so redundant
`gtk_fixed_move()` calls are skipped, and the surface became constant.
`layout_panel()` fell from ~0.1 ms to ~0.03 ms across the board. That is a
plausible story and it is *not* offered as the cause -- layout was never more
than 1% of a frame, so it cannot by itself explain 30 fps becoming 60. The
honest position is that the old numbers describe a build that no longer exists
and the cause was never established.

If the dock feels slow, measure before changing anything:

    LUCID_DOCK_BENCH=180 ./lucid_dock_cpp

and check the GPU is not parked, which looks identical from the inside:

    cat /sys/class/drm/card*/gt_cur_freq_mhz   # against gt_max_freq_mhz
    powerprofilesctl get

### Historical: measured under GTK 4.14.5, earlier build

| Configuration | fps | `layout_panel()` p50 |
|---|---|---|
| `GSK_RENDERER=gl` (removed in GTK 4.18) | **60** | 0.071 ms |
| `GSK_RENDERER=ngl` (GTK 4.14 default) | 30 | 0.137 ms |
| `GSK_RENDERER=vulkan` (GTK 4.16+ default on Wayland) | **20** | 0.058 ms |
| Any renderer, idle | 60 | — |

Vulkan here is `hasvk`, Mesa's legacy Haswell driver, which announces itself
with *"Haswell Vulkan support is incomplete"*.

Both caveats on that workaround were checked at the time, and remain true
regardless of which numbers you believe:

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

## What it costs in RAM

A dock runs for the whole session, so what it costs resident is a promise to
the user in the same way 60 fps is -- and it regresses the same way, quietly,
in a commit about something else. `tests/check_memory_budget.sh` is the check
that makes that loud, and CI runs it.

Measured under headless sway with `GSK_RENDERER=cairo`, against a fixture of
eight applications the check ships itself:

| | PSS |
|---|---|
| An empty GTK4 window | 11.9 MiB |
| `lucid_dock_cpp`, 8 pinned icons | **20.4 MiB** |

Repeatable to 0.1 MiB. Four decisions make that number mean something, and
three of them are there because the check was wrong first:

**The budget is on the absolute figure, not on the cost above an empty
window.** The delta is the more meaningful quantity and it does not survive
contact with a second machine: the floor measured 11.9 MiB on a development
host and 17.3 MiB on a CI runner, which have different GTK builds, fontconfig
caches and locale data. Subtracting a term that moves by 5 MiB from one that
moves by 2 gives a difference dominated by the wrong term -- the same dock
measured 8.5 MiB by that method locally and 0.6 MiB on CI. The absolute is
stable *within* an environment, which is all a regression check needs. Both
numbers are still printed, because "the dock grew" and "GTK grew" are different
problems and the pair says which.

**The graphics driver is excluded.** With the GL renderer the figure is at the
mercy of the driver: with a GPU the framebuffers live in GPU memory and never
appear in PSS, and without one -- every CI runner -- GTK falls back to llvmpipe
and those same buffers become process memory counted in full. The first CI run
measured the floor at 226 MiB against the dock's 171 for that reason and
reported the dock as costing *minus 55 MiB*, which it passed.

**The check brings its own applications and its own icons.** Without a fixture
it measures whatever is installed: a bare runner had almost no `.desktop` files,
the dock drew no icons, and it measured byte for byte the same as an empty
window. Naming *stock* icons was the next attempt and was wrong the same way --
CI resolved Adwaita's symbolic SVGs while a development machine resolved
ZorinBlue's full-colour PNGs, and rasterising those is most of what is being
measured. The fixture now ships eight identical 256x256 PNGs, generated with
the Python standard library so no image tooling is needed.

**It asserts it measured something.** The check counts the fixture icons the
dock actually built and fails if any are missing. This is checked rather than
assumed because the failure looks exactly like good news: a dock that finds no
applications draws no icons, costs almost nothing, and passes any budget. CI
reported -55.3, then 0.0, then 0.6 MiB, and only the first was obviously wrong
at a glance.

**PSS, not RSS.** RSS charges the dock for every shared library page in full
whether or not anything else maps them. PSS divides shared pages by the number
of processes mapping them and is the only figure that adds up across a session.
The dock's RSS is about 130 MiB; that number is not wrong, it just cannot be
added to anything.

This is a regression signal, not what a user pays. The shipped GL path costs
about 44 MiB PSS on a machine with a GPU.

### Where the RAM actually goes, for scale

Measured on the same machine, in a stock Zorin/GNOME session:

| | PSS |
|---|---|
| `gnome-shell` | 294.6 MiB |
| `gnome-software` -- **no window**, `--gapplication-service` | 321.7 MiB |
| `nautilus` -- **no window**, `--gapplication-service` | 163.8 MiB |
| evolution-\* (4 processes) | 53.9 MiB |
| gjs, gsd-\*, portals, ibus | 38.8 MiB |
| **those alone** | **872.8 MiB** |
| `lucid_dock_cpp` | 50.0 MiB |

Two windowless background services account for 485 MiB, more than the shell
itself. A toolkit is not what makes a desktop heavy; a service constellation
is. An empty GTK4 window is 4% of that total, which is worth knowing before
concluding that the toolkit is the problem -- for reference, `foot`, a lean
Wayland terminal in C, measures 10.6 MiB, so leaving GTK entirely would save
roughly 35 MiB per surface.

## Why out-of-process

The dock is a separate process from the compositor on purpose. A dock that
crashes should cost you a dock, not your session. The same rule applies to every
LucidOS shell component and to third-party extensions later, and it is what makes
"deep customization that cannot break your session" implementable rather than
aspirational. It also keeps the dock portable: it runs on any layer-shell
compositor today, so the choice of compositor for LucidOS stays open.
