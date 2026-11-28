#!/usr/bin/env bash
# GeaOS app model (packages/geaos): manifest, .geapp package, registry and
# launcher, with ASan and UBSan.
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ipackages/geaos \
  packages/core/test/test_geaos_app_model.cpp \
  packages/geaos/geaos_json.cpp packages/geaos/geaos_manifest.cpp packages/geaos/geaos_package.cpp \
  packages/geaos/geaos_fs_posix.cpp packages/geaos/geaos_registry.cpp packages/geaos/geaos_install.cpp packages/geaos/geaos_monitor.cpp packages/geaos/geaos_launcher.cpp packages/geaos/geaos_settings.cpp packages/geaos/geaos_trust.cpp packages/geaos/geaos_crypto.cpp \
  -o "$out/test_geaos_app_model"
"$out/test_geaos_app_model" "$out"
# The launcher is called from the runtime task, a home-button task and a
# volume-watch task at once: the same suite under ThreadSanitizer.
"${CXX:-clang++}" -std=c++17 -pthread -g -fsanitize=thread \
  -Ipackages/geaos \
  packages/core/test/test_geaos_app_model.cpp \
  packages/geaos/geaos_json.cpp packages/geaos/geaos_manifest.cpp packages/geaos/geaos_package.cpp \
  packages/geaos/geaos_fs_posix.cpp packages/geaos/geaos_registry.cpp packages/geaos/geaos_install.cpp packages/geaos/geaos_monitor.cpp packages/geaos/geaos_launcher.cpp packages/geaos/geaos_settings.cpp packages/geaos/geaos_trust.cpp packages/geaos/geaos_crypto.cpp \
  -o "$out/test_geaos_app_model_tsan"
"$out/test_geaos_app_model_tsan" "$out"
