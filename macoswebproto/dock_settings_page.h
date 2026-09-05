// The dock's settings, as a widget rather than a window.
//
// This is the whole point of the file existing separately. Today a ~40 line
// main() wraps this in a GtkWindow and it is the lucid-dock-settings binary.
// When LucidOS Settings exists it compiles this same file and drops the same
// widget into a stack page -- no rework, no extraction, no "we'll refactor it
// later". Building it as a window instead is the version that has to be gutted.
//
// It talks to the dock through files and nothing else -- ~/.config/lucid/dock.conf
// for the pinned arrangement, and the lucid-tokens user layer for everything
// with a default and a range. The dock watches both, so changes here apply live
// with no IPC, no D-Bus and no protocol between the two processes.
//
// That split is deliberate and it is the reason this widget can show provenance
// at all. A token knows which layer set it, so "where did this come from" and
// "reset only this one" are lookups rather than features. An arrangement has
// neither a default nor a meaningful revert, so it stays in dock.conf and
// Reset to Defaults does not touch it.

#pragma once

#include <gtk/gtk.h>

GtkWidget* lucid_dock_settings_page_new(void);
