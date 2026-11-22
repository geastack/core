#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
gea_require_compiler_runtime
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
include=(-I"$GEA_COMPILER_RUNTIME" -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui)
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined "${include[@]}" \
  packages/core/test/test_http_typed.cpp packages/host/host/http.cpp -o "$out/test_http_typed"
"$out/test_http_typed"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/core/include \
  packages/core/test/test_captive_dns.cpp -o "$out/test_captive_dns"
"$out/test_captive_dns"
source packages/core/test/native-host.sh
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined -ffunction-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} "${include[@]}" \
  packages/core/test/test_file_cache_provider.cpp packages/host/host/image.cpp -o "$out/test_file_cache_provider"
"$out/test_file_cache_provider" packages/core/test/fixtures/native-jpeg.jpg "$out/badge-test.jpg"
echo 'Typed HTTP headers, captive DNS, persistent JPEG file-cache provider: PASS'
