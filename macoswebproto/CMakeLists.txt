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
# Foreign-toplevel protocols
#
# The XML is vendored in protocols/ rather than taken from wayland-protocols:
# wlr-protocols is not packaged on Debian or Ubuntu at all, and pinning both
# files means the marshalling code cannot change underneath a build.
#
# The generated sources are C. They are compiled as C -- the generated headers
# carry extern "C" guards, so the C++ side links against them unchanged.
# ---------------------------------------------------------------------------

find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
	message(FATAL_ERROR "wayland-scanner not found. Install: sudo apt install libwayland-bin")
endif()

enable_language(C)

set(LUCID_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LUCID_PROTOCOL_DIR}")

set(LUCID_PROTOCOL_SOURCES "")
foreach(proto ext-foreign-toplevel-list-v1 wlr-foreign-toplevel-management-unstable-v1)
	set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${proto}.xml")
	set(header "${LUCID_PROTOCOL_DIR}/${proto}-client-protocol.h")
	set(code "${LUCID_PROTOCOL_DIR}/${proto}-protocol.c")
	add_custom_command(
		OUTPUT "${header}"
		COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
		DEPENDS "${xml}"
		VERBATIM)
	add_custom_command(
		OUTPUT "${code}"
		COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
		DEPENDS "${xml}"
		VERBATIM)
	list(APPEND LUCID_PROTOCOL_SOURCES "${header}" "${code}")
endforeach()

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

add_executable(lucid_dock_cpp lucid_dock.cpp toplevel_source.cpp "${LUCID_STAMP_CPP}"
	"${LUCID_TOKENS_DIR}/src/tokens.cpp"
	${LUCID_PROTOCOL_SOURCES})
target_include_directories(lucid_dock_cpp PRIVATE "${LUCID_PROTOCOL_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}" "${LUCID_TOKENS_DIR}/include")
add_dependencies(lucid_dock_cpp lucid_build_stamp)

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
add_executable(lucid_dock_settings dock_settings_page.cpp lucid_dock_settings.cpp
	"${LUCID_STAMP_CPP}")
target_include_directories(lucid_dock_settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(lucid_dock_settings lucid_build_stamp)
target_link_libraries(lucid_dock_settings PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX)
