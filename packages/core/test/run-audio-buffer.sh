#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
gea_require_compiler_runtime
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
source packages/core/test/native-host.sh
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} \
  -I"$GEA_COMPILER_RUNTIME" \
  -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_buffer_main.cpp packages/host/host/audio.cpp packages/host/host/audio_buffer_runtime.cpp \
  -o "$out/test_audio_buffer"
"$out/test_audio_buffer"
