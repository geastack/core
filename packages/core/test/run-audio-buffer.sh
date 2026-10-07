#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
export TMPDIR="$out"
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -ffunction-sections "$link_gc" \
  -I"${GEA_COMPILER_RUNTIME:-../compiler/src/targets/cpp/runtime}" \
  -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_buffer_main.cpp packages/host/host/audio.cpp packages/host/host/audio_buffer_runtime.cpp \
  -o "$out/test_audio_buffer"
"$out/test_audio_buffer"
