#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
include=(-I"${GEA_COMPILER_RUNTIME:-../compiler/src/targets/cpp/runtime}" -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui)
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined "${include[@]}" \
  packages/core/test/test_http_typed.cpp packages/host/host/http.cpp -o "$out/test_http_typed"
"$out/test_http_typed"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -Ipackages/core/include \
  packages/core/test/test_captive_dns.cpp -o "$out/test_captive_dns"
"$out/test_captive_dns"
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined -ffunction-sections "$link_gc" "${include[@]}" \
  packages/core/test/test_file_cache_provider.cpp packages/host/host/image.cpp -o "$out/test_file_cache_provider"
"$out/test_file_cache_provider" packages/core/test/fixtures/native-jpeg.jpg "$out/badge-test.jpg"
echo 'Typed HTTP headers, captive DNS, persistent JPEG file-cache provider: PASS'
