#!/usr/bin/env bash
# Host-specific flags for the native tests' clang builds. Sourced, never run.
#
#   GEA_HOST_CXXFLAGS   added to every C and C++ compile
#   GEA_HOST_LINK_GC    the link's dead-code removal, in this host's spelling
#
# The tests link only the sources they exercise, and those sources reference
# code the test never runs (a canvas that can draw text references the font
# registry). ELF linkers with --gc-sections and ld64 with -dead_strip discard
# such references with the dead sections.
#
# Windows (clang targeting MSVC, run from Git Bash):
#   -fno-ms-compatibility  MSVC's quoted-include rule searches the directory of
#     every file on the include stack before the -I paths, so engine/ui's
#     `#include "display.h"` resolved to host/include/host/display.h instead of
#     core/include/display.h. The rest of MS compatibility is not needed by
#     the MSVC STL or these sources.
#   _CRT_DECLARE_NONSTDC_NAMES  without MS compatibility clang defines
#     __STDC__, and the Windows CRT then hides its POSIX spellings (`struct
#     stat`, `stat`, `fileno`) that platform/atomic_file.h and the hosts use.
#   windows-include/  the POSIX headers the test hosts include (unistd.h, with
#     an alarm() watchdog).
#   python3  Windows names its Python 3 `python` (see the case below).
#   -fverify-intermediate-code  LLVM's IR verifier, which clang skips by
#     default: clang 22 and 23 targeting MSVC can produce invalid IR at -O1 and
#     above for a coroutine whose catch handler assigns a local declared outside
#     the try (CoroSplit), and unverified that build runs miscompiled. Verified
#     it fails with "Broken function found". An interim safety net.
#   /force:unresolved  lld-link reports undefined symbols before /opt:ref
#     discards the dead code that references them, which the other hosts'
#     linkers never do. Forcing the link reproduces their result for dead code;
#     a reference the test does execute still fails, at run time, and Linux and
#     macOS keep catching it at link time. Limited to this test harness.
GEA_NATIVE_HOST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in
  Darwin)
    GEA_HOST_CXXFLAGS=()
    GEA_HOST_LINK_GC=(-Wl,-dead_strip)
    ;;
  MINGW* | MSYS* | CYGWIN*)
    GEA_HOST_CXXFLAGS=(-fno-ms-compatibility -D_CRT_DECLARE_NONSTDC_NAMES=1 -fverify-intermediate-code -I "$GEA_NATIVE_HOST_DIR/windows-include")
    # lld-link options are spelled with '-', not '/': Git Bash rewrites an
    # argument such as /map into a Windows path before clang sees it.
    # -opt:ref alone would also turn on identical-code folding, which the other
    # hosts' dead-code removal does not do, and which merges AddressSanitizer's
    # instrumented string literals into a poisoned redzone (a
    # global-buffer-overflow reading "" in test_audio_stream, still so with
    # clang 23); -opt:noicf. That is the only reason: these tests are
    # sanitized, and the Gea runtime itself is indifferent to folding.
    GEA_HOST_LINK_GC=(-fuse-ld=lld -Wl,-opt:ref -Wl,-opt:noicf -Wl,-force:unresolved)
    # Python 3 is `python` on Windows; `python3` there is usually the
    # Microsoft Store's install prompt, which exits non-zero. The tests that
    # mutate sources or read layouts call `python3`.
    if ! python3 -c '' > /dev/null 2>&1 && python -c 'import sys; sys.exit(sys.version_info[0] != 3)' > /dev/null 2>&1; then
      python3() { python "$@"; }
    fi
    ;;
  *)
    GEA_HOST_CXXFLAGS=()
    GEA_HOST_LINK_GC=(-Wl,--gc-sections)
    ;;
esac

# The symbols a linked test program kept, one per line, for the tests that
# check what a link retained or dropped. nm -C prints them demangled; on Windows
# they come from the link's map, Microsoft-mangled, and GEA_HOST_MANGLING says
# so to a caller whose patterns depend on the spelling.
GEA_HOST_MANGLING=itanium
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) GEA_HOST_MANGLING=microsoft ;; esac
gea_native_symbols() {
  if [[ "$GEA_HOST_MANGLING" == microsoft ]]; then cat "${1%.exe}.map"; else nm -C "$1"; fi
}

# The link flag that writes that list for the program at $1: a Windows
# executable carries no symbol table, so lld-link writes its map beside it,
# named explicitly (a bare -map named it after the wrong path). Nothing
# elsewhere, where nm reads the binary.
gea_native_symbol_map_flags() {
  if [[ "$GEA_HOST_MANGLING" == microsoft ]]; then printf '%s\n' "-Wl,-map:$(cygpath -m "${1%.exe}.map")"; fi
}

# The suffix a linked executable needs to be started by a native program such
# as node: Windows' CreateProcess search (libuv's included) adds .com/.exe to a
# name without an extension and never tries the bare name, which only Git
# Bash runs. Empty elsewhere.
GEA_HOST_EXE_SUFFIX=
if [[ "$GEA_HOST_MANGLING" == microsoft ]]; then GEA_HOST_EXE_SUFFIX=.exe; fi

# $1, a Git Bash path (/c/Users/...), spelled for a native program to open:
# outside Git Bash such a path is drive-relative. Unchanged elsewhere.
gea_native_path() {
  if [[ "$GEA_HOST_MANGLING" == microsoft ]]; then cygpath -w "$1"; else printf '%s\n' "$1"; fi
}
