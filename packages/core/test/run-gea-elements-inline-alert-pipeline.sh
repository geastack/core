#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec "$ROOT/packages/core/test/run-gea-native-app-pipeline.sh" \
  "gea-elements-inline-alert-pipeline" \
  "$ROOT/packages/core/test/fixtures/gea-elements-inline-alert" \
  "index.tsx" \
  "packages/core/test/test_gea_elements_inline_alert_main.cpp"
