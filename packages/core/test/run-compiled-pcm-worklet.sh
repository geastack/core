#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 /absolute/path/to/pcm.worklet.cpp generated_entry_symbol" >&2
  exit 2
fi
module=$1
entry=$2
if [[ "$module" != /* || ! -f "$module" || ! "$entry" =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ ]]; then
  echo 'Expected an existing absolute C++ path and a C++ entry identifier.' >&2
  exit 2
fi
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
flags=(-O2)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags=(-O1 -fsanitize=address,undefined); fi
"${CXX:-clang++}" -std=c++20 "${flags[@]}" -pthread -fsized-deallocation -ffunction-sections "$link_gc" \
  -DGEA_RUNTIME_REALMS=1 -DGEA_RUNTIME_SINGLE_THREADED=1 -DGEA_RUNTIME_COMPACT_ALLOCATION=1 \
  "-DGEA_COMPILED_WORKLET=\"$module\"" "-DGEA_COMPILED_WORKLET_ENTRY=$entry" \
  -I"$(dirname "$module")" -Ipackages/host/include -Ipackages/core/include \
  -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_compiled_pcm_worklet.cpp packages/host/host/worker.cpp \
  -o "$out/test_compiled_pcm_worklet"
"$out/test_compiled_pcm_worklet"
