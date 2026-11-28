#!/usr/bin/env bash
# GeaOS process host (packages/geaos/geaos_process.*): apps as child
# processes that crash, hang, flood the channel or lose their Home; with ASan
# and UBSan. POSIX; run-geaos-process.cmd runs the same test on Windows.
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
"${CXX:-clang++}" -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ipackages/geaos \
  packages/core/test/test_geaos_process.cpp \
  packages/geaos/geaos_json.cpp packages/geaos/geaos_manifest.cpp packages/geaos/geaos_package.cpp \
  packages/geaos/geaos_fs_posix.cpp packages/geaos/geaos_registry.cpp packages/geaos/geaos_install.cpp \
  packages/geaos/geaos_monitor.cpp packages/geaos/geaos_launcher.cpp packages/geaos/geaos_settings.cpp packages/geaos/geaos_trust.cpp packages/geaos/geaos_crypto.cpp packages/geaos/geaos_process.cpp \
  -o "$out/test_geaos_process"
# The deliberate crashes are the apps', not the test's: ASan must not turn a
# child's SIGSEGV into its own report and exit code.
ASAN_OPTIONS="${ASAN_OPTIONS:-}:handle_segv=0:handle_abort=0" "$out/test_geaos_process"
