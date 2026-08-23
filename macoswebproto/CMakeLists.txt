cmake_minimum_required(VERSION 3.16)

project(lucid_dock_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)

# gtk4-layer-shell is REQUIRED by default. Without it the dock cannot anchor to
# a screen edge and degrades to an unanchored, fixed-size window. That fallback
# used to be selected silently by a QUIET check, which made a broken build look
# like a working one. Opt out only for deliberate local experiments.
option(LUCID_ALLOW_NO_LAYER_SHELL "Build without layer-shell (unanchored window; not supported)" OFF)

if(LUCID_ALLOW_NO_LAYER_SHELL)
	pkg_check_modules(GTK4_LAYER_SHELL QUIET IMPORTED_TARGET gtk4-layer-shell-0)
else()
	pkg_check_modules(GTK4_LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
endif()

add_executable(lucid_dock_cpp lucid_dock.cpp)

# gtk4-layer-shell has to precede libwayland on the link line -- it works by
# shimming libwayland, and being linked after it fails at run time ("GTK4 Layer
# Shell may have been linked after libwayland") rather than at link time. Hence
# this link_libraries call comes before the GTK4 one.
if(GTK4_LAYER_SHELL_FOUND)
	target_link_libraries(lucid_dock_cpp PRIVATE PkgConfig::GTK4_LAYER_SHELL)
	target_compile_definitions(lucid_dock_cpp PRIVATE HAVE_GTK4_LAYER_SHELL=1)
	# Picks up a user-prefix build of the library (see README) without needing
	# LD_LIBRARY_PATH at run time.
	set_target_properties(lucid_dock_cpp PROPERTIES
		BUILD_RPATH "${GTK4_LAYER_SHELL_LIBRARY_DIRS}"
		INSTALL_RPATH "${GTK4_LAYER_SHELL_LIBRARY_DIRS}")
else()
	message(WARNING
		"Building WITHOUT layer-shell. The dock will not anchor to the screen edge, "
		"will not reserve space, and will be positioned by guesswork. This is not a "
		"supported configuration. Install gtk4-layer-shell (NOT libgtk-layer-shell-dev, "
		"which is the GTK3 build) and reconfigure.")
endif()

target_link_libraries(lucid_dock_cpp PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX)
