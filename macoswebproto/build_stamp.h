#pragma once

// The build time and git rev of the running binary, printed at startup by both
// binaries and by --version.
//
// Deliberately extern constants rather than macros with an "unknown" fallback.
// The fallback was how the CMake build came to produce binaries that announced
// themselves as "lucid-dock unknown built unknown" for as long as nobody
// looked: it never defined the macros, and the fallback made that silent. A
// missing stamp is now a link error, which is the failure mode this project
// wants -- see "Make it impossible to run a stale binary without noticing".
//
// The definitions come from a generated translation unit that both build
// systems rewrite on every build. This header's contents never change, so
// including it costs no rebuilds.

namespace lucid {

extern const char* const kBuildStamp;   // "2026-09-04 22:09:33"
extern const char* const kGitRev;       // "b0039dd" or "b0039dd+dirty"

}  // namespace lucid
