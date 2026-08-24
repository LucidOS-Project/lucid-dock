// The dock's settings, as a widget rather than a window.
//
// This is the whole point of the file existing separately. Today a ~40 line
// main() wraps this in a GtkWindow and it is the lucid-dock-settings binary.
// When LucidOS Settings exists it compiles this same file and drops the same
// widget into a stack page -- no rework, no extraction, no "we'll refactor it
// later". Building it as a window instead is the version that has to be gutted.
//
// It talks to the dock through ~/.config/lucid/dock.conf and nothing else. The
// dock has a GFileMonitor on that file, so changes here apply live with no IPC,
// no D-Bus and no protocol between the two processes.

#pragma once

#include <gtk/gtk.h>

GtkWidget* lucid_dock_settings_page_new(void);
