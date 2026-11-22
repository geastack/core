#!/usr/bin/env bash

# Default locations of what the native tests build against, for every one the
# caller has not set. Sourced by run-tests.sh and by the scripts that need it,
# so a single script runs on its own too.
#
# The compiler is the sibling `compiler/` checkout when there is one (so a
# workspace tests the live compiler sources), otherwise the @geastack/compiler
# this package installs. Everything is overridable by setting the variable:
#
#   GEA_COMPILER_CLI / GEA_GEATSC_BIN   compiler CLI (dist/cli.js)
#   GEA_COMPILER_RUNTIME                compiler runtime header directory
#   GEA_TEST_RUNTIME_HEADER             that directory's gea_runtime.h
#   GEA_JSDOM_PACKAGE                   jsdom package directory (the DOM oracles)
#   GEA_CORE_DIR, GEA_HOST_DIR,         the core, host and plugin packages
#   GEA_PLUGIN_DIR
#   GEA_CENSUS_REFERENCE_ENGINE         engine pair compared by the UI memory census
#   GEA_CENSUS_CANDIDATE_ENGINE

GEA_TEST_CORE_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
GEA_TEST_CORE_PACKAGE="$GEA_TEST_CORE_REPO/packages/core"
GEA_TEST_PLUGIN_DIST="$GEA_TEST_CORE_REPO/packages/geatsc-plugin-gea/dist"

# Directory of an installed package, resolved the way Node resolves it from
# packages/core. Prints nothing when it is not installed.
gea_test_package_dir() {
  node -e '
    const { createRequire } = require("node:module")
    const path = require("node:path")
    try {
      process.stdout.write(path.dirname(createRequire(process.argv[1]).resolve(process.argv[2] + "/package.json")))
    } catch {}
  ' "$GEA_TEST_CORE_PACKAGE/package.json" "$1"
}

if [[ -z "${GEA_COMPILER_ROOT:-}" ]]; then
  if [[ -n "${GEA_COMPILER_CLI:-}" ]]; then
    GEA_COMPILER_ROOT="$(cd "$(dirname "$GEA_COMPILER_CLI")/.." && pwd)"
  elif [[ -n "${GEA_GEATSC_BIN:-}" ]]; then
    GEA_COMPILER_ROOT="$(cd "$(dirname "$GEA_GEATSC_BIN")/.." && pwd)"
  elif [[ -f "$GEA_TEST_CORE_REPO/../compiler/src/targets/cpp/runtime/gea_runtime.h" ]]; then
    GEA_COMPILER_ROOT="$(cd "$GEA_TEST_CORE_REPO/../compiler" && pwd)"
  else
    GEA_COMPILER_ROOT="$(gea_test_package_dir @geastack/compiler)"
  fi
fi
if [[ -n "$GEA_COMPILER_ROOT" ]]; then
  export GEA_COMPILER_ROOT
  export GEA_COMPILER_CLI="${GEA_COMPILER_CLI:-$GEA_COMPILER_ROOT/dist/cli.js}"
  export GEA_GEATSC_BIN="${GEA_GEATSC_BIN:-$GEA_COMPILER_CLI}"
  export GEA_COMPILER_RUNTIME="${GEA_COMPILER_RUNTIME:-$GEA_COMPILER_ROOT/src/targets/cpp/runtime}"
  export GEA_TEST_RUNTIME_HEADER="${GEA_TEST_RUNTIME_HEADER:-$GEA_COMPILER_RUNTIME/gea_runtime.h}"
fi
if [[ -z "${GEA_JSDOM_PACKAGE:-}" ]]; then
  GEA_JSDOM_PACKAGE="$(gea_test_package_dir jsdom)"
  [[ -n "$GEA_JSDOM_PACKAGE" ]] && export GEA_JSDOM_PACKAGE
fi
# The packages the compiler's Gea host and the app pipelines read declarations
# and sources from: this repo's, not copies installed next to the compiler.
export GEA_CORE_DIR="${GEA_CORE_DIR:-$GEA_TEST_CORE_PACKAGE}"
export GEA_HOST_DIR="${GEA_HOST_DIR:-$GEA_TEST_CORE_REPO/packages/host}"
export GEA_PLUGIN_DIR="${GEA_PLUGIN_DIR:-$GEA_TEST_CORE_REPO/packages/geatsc-plugin-gea}"
export GEA_CENSUS_REFERENCE_ENGINE="${GEA_CENSUS_REFERENCE_ENGINE:-$GEA_TEST_CORE_REPO/packages/engine}"
export GEA_CENSUS_CANDIDATE_ENGINE="${GEA_CENSUS_CANDIDATE_ENGINE:-$GEA_TEST_CORE_REPO/packages/engine}"

# The compiler runtime headers. Missing only when packages/core's dependencies
# are not installed and there is no sibling compiler checkout.
gea_require_compiler_runtime() {
  [[ -f "${GEA_COMPILER_RUNTIME:-}/gea_runtime.h" ]] && return 0
  echo "error: no geatsc runtime headers found; run 'npm install' in packages/core, or set GEA_COMPILER_RUNTIME" >&2
  exit 1
}

# Tests that compile with the Gea plugin, or write their binaries into the
# plugin's build output, need that package built first.
gea_require_plugin_build() {
  [[ -f "$GEA_TEST_PLUGIN_DIST/index.js" ]] && return 0
  echo "error: packages/geatsc-plugin-gea is not built; run 'npm install && npm run build' in packages/geatsc-plugin-gea first" >&2
  exit 1
}
