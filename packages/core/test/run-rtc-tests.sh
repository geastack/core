#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
cxx="${CXX:-clang++}"
flags=(-std=c++20 -fsanitize=address,undefined -Ipackages/host/include -Ipackages/core/include)
"$cxx" "${flags[@]}" packages/core/test/test_rtc_sdp.cpp -o "$out/test_rtc_sdp"
"$out/test_rtc_sdp"
"$cxx" "${flags[@]}" packages/core/test/test_rtc_objects.cpp packages/host/host/rtc.cpp -o "$out/test_rtc_objects"
"$out/test_rtc_objects"
"$cxx" "${flags[@]}" -DGEA_RTC_STANDALONE_MEDIA_TEST packages/core/test/test_media_stream_lifecycle.cpp packages/host/host/media.cpp packages/host/host/worker.cpp -o "$out/test_rtc_media"
"$out/test_rtc_media"
"$cxx" "${flags[@]}" packages/core/test/test_media_video.cpp packages/host/host/media.cpp packages/host/host/worker.cpp -o "$out/test_media_video"
"$out/test_media_video"
for format in 0 1; do
  "$cxx" "${flags[@]}" -DGEA_EMBEDDED_PIXEL_FORMAT="$format" -Ipackages/engine \
    packages/core/test/test_video_presentation.cpp packages/host/host/video.cpp packages/host/host/mjpeg.cpp packages/host/host/media.cpp packages/host/host/worker.cpp \
    -o "$out/test_video_presentation"
  "$out/test_video_presentation"
done
# The desktop codec test links libopus; firmware gets the same API from esp-opus.
read -r -a opus_flags <<< "$("${PKG_CONFIG:-pkg-config}" --cflags --libs opus)"
"$cxx" "${flags[@]}" packages/core/test/test_rtc_opus.cpp "${opus_flags[@]}" -o "$out/test_rtc_opus"
"$out/test_rtc_opus"
# Keep the streaming element and PCM output boundary in the same native check.
source packages/core/test/native-host.sh
"$cxx" "${flags[@]}" -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_stream.cpp packages/host/host/audio.cpp packages/host/host/media.cpp packages/host/host/worker.cpp -o "$out/test_audio_stream"
"$out/test_audio_stream"

# Raw-PCM transports share the same device stream boundary, without Opus.
"$cxx" "${flags[@]}" packages/core/test/test_pcm_codec.cpp -o "$out/test_pcm_codec"
"$out/test_pcm_codec"
