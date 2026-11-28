#!/usr/bin/env bash
# GeaOS storage (packages/geaos): install/uninstall/recovery on volumes that
# fill, lock or are pulled out; volume monitoring and hot-plug; with ASan and
# UBSan, then under ThreadSanitizer.
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
sources=(
  packages/core/test/test_geaos_storage.cpp
  packages/geaos/geaos_json.cpp packages/geaos/geaos_manifest.cpp packages/geaos/geaos_package.cpp
  packages/geaos/geaos_fs_posix.cpp packages/geaos/geaos_registry.cpp packages/geaos/geaos_install.cpp
  packages/geaos/geaos_monitor.cpp packages/geaos/geaos_launcher.cpp packages/geaos/geaos_settings.cpp packages/geaos/geaos_trust.cpp packages/geaos/geaos_crypto.cpp
)
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ipackages/geaos "${sources[@]}" -o "$out/test_geaos_storage"
"$out/test_geaos_storage" "$out"
"${CXX:-clang++}" -std=c++17 -pthread -g -fsanitize=thread -Ipackages/geaos "${sources[@]}" -o "$out/test_geaos_storage_tsan"
"$out/test_geaos_storage_tsan" "$out"
