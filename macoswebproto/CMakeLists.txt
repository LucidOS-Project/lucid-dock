cmake_minimum_required(VERSION 3.16)

project(lucid_dock_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

# ---------------------------------------------------------------------------
# lucid-wayland
#
# The foreign-toplevel protocols and the client that speaks them used to be
# generated and compiled here. They now live in third_party/lucid-wayland, so
# that lucid-panel uses one implementation rather than a second copy that would
# drift -- and protocol bugs are the hardest kind to find, because they look
# like the compositor's fault.
#
# Built through its own Makefile rather than re-described in CMake: it already
# knows how to run wayland-scanner and which sources are C, and restating that
# here would be a second description of one build to keep in step.
# ---------------------------------------------------------------------------

set(LUCID_WAYLAND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucid-wayland")
if(NOT EXISTS "${LUCID_WAYLAND_DIR}/src/toplevel_source.cpp")
	message(FATAL_ERROR
		"third_party/lucid-wayland is empty. Run: git submodule update --init --recursive")
endif()

set(LUCID_WAYLAND_LIB "${LUCID_WAYLAND_DIR}/liblucidwayland.a")
add_custom_command(
	OUTPUT "${LUCID_WAYLAND_LIB}"
	COMMAND "${CMAKE_COMMAND}" -E env make -C "${LUCID_WAYLAND_DIR}"
	VERBATIM)
add_custom_target(lucid_wayland_lib DEPENDS "${LUCID_WAYLAND_LIB}")


# ---------------------------------------------------------------------------
# Build stamp
#
# Both binaries print their git rev and build time at startup, so "is this
# current?" is answerable from the running process. This build used to skip
# that and produce binaries announcing themselves as "lucid-dock unknown built
# unknown", which is the exact failure the stamping exists to prevent.
#
# A custom target, not execute_process: execute_process runs at configure time,
# so the rev would freeze at whatever was checked out when cmake was last run
# and go quietly stale. A custom target runs on every build.
#
# The generated file is a translation unit rather than a header the sources
# include, because its contents change every build: as a header it would
# recompile lucid_dock.cpp (5.7 s) every time, whereas this file costs 0.01 s.
# build_stamp.h is what the sources include, and never changes.
# ---------------------------------------------------------------------------

set(LUCID_STAMP_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/build_stamp.cpp")

add_custom_target(lucid_build_stamp
	BYPRODUCTS "${LUCID_STAMP_CPP}"
	COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/write_build_stamp.sh"
	        "${LUCID_STAMP_CPP}" "${CMAKE_CURRENT_SOURCE_DIR}"
	COMMENT "Stamping git rev and build time"
	VERBATIM)

# It does not exist until the target above has run, so CMake has to be told not
# to look for it at configure time.
set_source_files_properties("${LUCID_STAMP_CPP}" PROPERTIES GENERATED TRUE)

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

# lucid-tokens is a submodule pinned to a commit, so the schema and the dock
# cannot drift apart the way they already did once.
set(LUCID_TOKENS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucid-tokens")
if(NOT EXISTS "${LUCID_TOKENS_DIR}/src/tokens.cpp")
	message(FATAL_ERROR
		"Missing ${LUCID_TOKENS_DIR}. Run: git submodule update --init --recursive")
endif()

add_executable(lucid_dock_cpp lucid_dock.cpp "${LUCID_STAMP_CPP}"
	"${LUCID_TOKENS_DIR}/src/tokens.cpp")
target_include_directories(lucid_dock_cpp PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}" "${LUCID_TOKENS_DIR}/include"
	"${LUCID_WAYLAND_DIR}/include")
add_dependencies(lucid_dock_cpp lucid_build_stamp lucid_wayland_lib)
target_link_libraries(lucid_dock_cpp PRIVATE "${LUCID_WAYLAND_LIB}")

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

target_link_libraries(lucid_dock_cpp PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX
	PkgConfig::WAYLAND_CLIENT)

# The settings UI. Separate binary, shares dock_config.h, and deliberately does
# not link layer-shell: it is an ordinary application window.
# The settings page reads and writes tokens too, so it needs the same library
# the dock does. Missing this compiled fine through build_lucid_dock.sh and
# failed only here, which is the reason both build paths are in CI.
add_executable(lucid_dock_settings dock_settings_page.cpp lucid_dock_settings.cpp
	"${LUCID_STAMP_CPP}" "${LUCID_TOKENS_DIR}/src/tokens.cpp")
target_include_directories(lucid_dock_settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
	"${LUCID_TOKENS_DIR}/include")
add_dependencies(lucid_dock_settings lucid_build_stamp)
target_link_libraries(lucid_dock_settings PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX)

# ---------------------------------------------------------------------------
# Installation
#
# The binaries are named lucid_dock_cpp and lucid_dock_settings in the build
# tree, for historical reasons and because the C++ one lived beside a Python
# one for a while. Installed, they are lucid-dock and lucid-dock-settings:
# a user types the installed name, and it should look like every other command
# on the system rather than like a build artifact that escaped.
#
# The dock's right-click menu already looks for the settings binary beside
# itself first and then on PATH, so both the build tree and an install work
# without either knowing which it is.
include(GNUInstallDirs)

install(TARGETS lucid_dock_cpp
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS lucid_dock_settings
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# CMake cannot rename an installed target, so the rename happens here.
#
# $ENV{DESTDIR} is not decoration. Packaging always installs into a staging
# root, and CMAKE_INSTALL_PREFIX alone points at the real / -- so without it
# this reaches outside the staging tree, fails, and on a machine where it
# happened to succeed it would have been renaming the system's own files.
install(CODE "
	set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
	file(RENAME \"\${_bindir}/lucid_dock_cpp\"      \"\${_bindir}/lucid-dock\")
	file(RENAME \"\${_bindir}/lucid_dock_settings\" \"\${_bindir}/lucid-dock-settings\")
")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/lucid-dock.desktop"
	DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/lucid-dock-settings.desktop"
	DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
