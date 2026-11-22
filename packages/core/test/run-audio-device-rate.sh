#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
gea_require_compiler_runtime
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
source packages/core/test/native-host.sh
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/core/include \
  packages/core/test/test_audio_oscillator.cpp -o "$out/test_audio_oscillator"
"$out/test_audio_oscillator"
for rate in 16000 44100; do
"${CXX:-clang++}" -std=c++20 -pthread -DGEA_AUDIO_DEVICE_SAMPLE_RATE="$rate" \
  -fsanitize=address,undefined -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} \
  -I"$GEA_COMPILER_RUNTIME" \
  -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_device_rate.cpp packages/host/host/audio_worklet.cpp \
  packages/host/host/audio.cpp packages/host/host/audio_buffer_runtime.cpp packages/host/host/media.cpp packages/host/host/worker.cpp \
  -o "$out/test_audio_device_rate"
"$out/test_audio_device_rate"
echo "$rate Hz capture, worklet, PCM timing and input-only processing: PASS"
done
