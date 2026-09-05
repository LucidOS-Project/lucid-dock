#pragma once

// ---------------------------------------------------------------------------
// Where "is this application running?" comes from.
//
// The dock has three ways to answer that, and they are not equally good:
//
//   ext-foreign-toplevel-list-v1   the standard protocol. Event-driven, and it
//                                  reports *windows*, which is the question the
//                                  dock is actually asking.
//   zwlr-foreign-toplevel-management-v1
//                                  the wlroots predecessor. Same information
//                                  for this purpose, much wider install base
//                                  today: every wlroots compositor older than
//                                  0.18, plus KWin and Hyprland.
//   /proc                          scan process names and match them against
//                                  .desktop Exec lines. Needs nothing from the
//                                  compositor, and is wrong in both directions.
//
// The /proc path is wrong in both directions because a process is not a window.
// It reports D-Bus-activated background services as running applications --
// GNOME Calendar and Seahorse show up with no window on screen -- and it misses
// applications whose binary name does not resemble their desktop id, which is
// most Flatpaks. It also costs a ~10 ms directory walk on the UI thread every
// four seconds to answer a question that changes a few times an hour.
//
// It is kept because it is the only one of the three that works everywhere,
// including on Mutter, which implements neither protocol.
//
// Which one is in use is printed at startup and can be forced with
// LUCID_DOCK_TOPLEVEL_SOURCE=ext|wlr|proc, because "the dot is wrong" is a very
// different bug depending on the answer.
// ---------------------------------------------------------------------------

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace lucid {

// One open window.
//
// The dock itself only needs to know that *something* with a given app_id is
// open, which is what running_keys() answers. This is the same information
// before it has been reduced to that: the compositor sends a title and a stable
// identifier with every toplevel, and collapsing them into a set of app_ids
// threw both away.
//
// Kept because a taskbar mode is one button per window rather than one icon per
// application, so it needs exactly what was being discarded -- and because
// declining to destroy information we are already handed is much cheaper now
// than reconstructing the path for it later.
struct ToplevelInfo {
    // Normalised the same way running_keys() is, so it can be matched against a
    // desktop entry's candidates without further work.
    std::string app_id;
    // As reported, not normalised: this is display text.
    std::string title;
    // Stable across the life of the toplevel. ext- only; the wlroots protocol
    // has no equivalent, so it is empty there.
    std::string identifier;
};

enum class ToplevelSourceKind {
    Proc,
    WlrForeignToplevel,
    ExtForeignToplevel,
};

// Short token: what LUCID_DOCK_TOPLEVEL_SOURCE accepts, and what gets logged.
const char* toplevel_source_token(ToplevelSourceKind kind);

// One line naming the protocol and how it updates, for the startup message.
const char* toplevel_source_description(ToplevelSourceKind kind);

class ToplevelSource {
  public:
    // Called when the *key set* has changed, once per batch of Wayland events
    // rather than once per event -- opening a window produces a toplevel, an
    // app_id, a title and a done, and rebuilding the dock four times for one
    // window would be visible.
    //
    // Note what this does not fire on, because it is deliberate rather than an
    // oversight: a second window of an application already running does not
    // change the key set, and neither does a title change. The dock must not
    // rebuild for either -- a browser would rebuild it on every page it
    // navigated to. A per-window consumer needs a signal this one does not
    // give, and should add one rather than widening this, which exists to keep
    // the dock still.
    using ChangedCallback = std::function<void()>;

    virtual ~ToplevelSource() = default;

    virtual ToplevelSourceKind kind() const = 0;

    // What is running, as lower-cased keys to be intersected with a desktop
    // entry's candidate keys. Under the Wayland sources these are app_ids;
    // under Proc they are process names. The two are not interchangeable, so
    // the caller has to build its candidates for the source it actually got --
    // see window_match_candidates() and exec_candidates() in lucid_dock.cpp.
    virtual const std::unordered_set<std::string>& running_keys() const = 0;

    // Every open window, in the order the compositor announced them -- which is
    // oldest first, and is the order a taskbar would want to lay buttons out
    // in. Two windows of one application are two entries here and one entry in
    // running_keys().
    //
    // Empty under Proc, which has no window information at all. That is not the
    // same as "nothing is open", so ask reports_windows() rather than testing
    // this for emptiness.
    virtual const std::vector<ToplevelInfo>& toplevels() const = 0;

    // False only for Proc. A process is not a window: /proc can say that
    // something called firefox is running and cannot say how many windows it
    // has, or what any of them are called.
    virtual bool reports_windows() const = 0;

    // False only for Proc. An event-driven source needs no refresh timer, and
    // installing one anyway would reintroduce exactly the cost this replaces.
    //
    // Deliberately separate from reports_windows(), which is true for exactly
    // the same sources today. They are different claims -- one is about how the
    // answer arrives, the other about what the answer contains -- and a source
    // that polled a window list would break the coincidence.
    virtual bool is_event_driven() const = 0;

    // Bring running_keys() up to date. A no-op on the event-driven sources,
    // where it is already current by construction.
    virtual void refresh() = 0;
};

// Picks the best source that can actually be established, honouring
// LUCID_DOCK_TOPLEVEL_SOURCE when it names one that works. Never returns null:
// the /proc source cannot fail.
std::unique_ptr<ToplevelSource> make_toplevel_source(ToplevelSource::ChangedCallback on_changed);

// Exposed for the /proc source and for tests. Lower-cases, and strips a
// trailing ".desktop" so a compositor that reports app_ids that way still
// matches.
std::string normalise_match_key(std::string key);

}  // namespace lucid
