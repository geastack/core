#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
node --input-type=module <<'JS'
import assert from 'node:assert/strict'
import path from 'node:path'
import { createRequire } from 'node:module'
import { validateWorkletRegistrations } from './packages/core/scripts/gea-worker-modules.mjs'
const ts = createRequire(path.resolve('../compiler/package.json'))('typescript')
const implementation = path.resolve('packages/core/runtime/audio-worklet.ts')
validateWorkletRegistrations(ts, path.resolve('packages/core/test/test_audio_worklet_compiled.ts'), implementation)
assert.throws(() => validateWorkletRegistrations(ts, path.resolve('packages/core/test/test_audio_worklet_parameters_rejected.ts'), implementation), /AudioParam parameterDescriptors/)
JS
GEATSC2_GEA_PLUGIN="$out/host-shims" node ../compiler/dist/cli.js compile \
  packages/core/test/test_audio_worklet_compiled.ts --out-dir "$out" \
  --plugin "$out/index.js" --realm-storage --entry-symbol gea_worklet_probe_entry
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
"${CXX:-clang++}" -std=c++20 -O1 -fsanitize=address,undefined -fsized-deallocation -ffunction-sections "$link_gc" \
  -DGEA_RUNTIME_REALMS=1 -DGEA_RUNTIME_COMPACT_ALLOCATION=1 \
  -I"$out" -Ipackages/host/include -Ipackages/core/include -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_audio_worklet_registration.cpp -o "$out/test_audio_worklet_registration"
"$out/test_audio_worklet_registration"
