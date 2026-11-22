#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
"${CXX:-clang++}" -std=c++20 -O2 -fsanitize=address,undefined -Ipackages/host/include \
  packages/core/test/test_video_color.cpp -o "$out/test_video_color"
"$out/test_video_color"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/host/include \
  packages/core/test/test_rtc_rtp.cpp -o "$out/test_rtc_rtp"
"$out/test_rtc_rtp"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/host/include \
  packages/core/test/test_rtc_receive_sdp.cpp -o "$out/test_rtc_receive_sdp"
"$out/test_rtc_receive_sdp"
read -r -a vpx_flags <<< "$("${PKG_CONFIG:-pkg-config}" --cflags --libs vpx)"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/host/include \
  packages/core/test/test_rtc_vp8.cpp "${vpx_flags[@]}" -o "$out/test_rtc_vp8"
"$out/test_rtc_vp8"
