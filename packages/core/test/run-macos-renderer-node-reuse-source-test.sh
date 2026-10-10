#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# The macOS host sources are in @geastack/apple, which core cannot depend on
# (apple sits above core and depends on this package), so the location is
# supplied rather than guessed. GEA_APPLE_ROOT is the package root -- the
# directory holding its package.json. The old spelling walked to a sibling
# apple checkout and looked under `targets/macos`, which stopped being the layout
# at the split: the package lives at apple/packages/geastack-apple.
APPLE_ROOT="${GEA_APPLE_ROOT:-}"
if [ -z "$APPLE_ROOT" ] || [ ! -d "$APPLE_ROOT/targets/macos" ]; then
  echo "SKIP: set GEA_APPLE_ROOT to the @geastack/apple package root (the directory holding its package.json)" >&2
  exit 0
fi
RENDERER="$APPLE_ROOT/targets/macos/main/macos_renderer.mm"

require_source() {
  local pattern="$1"
  local message="$2"
  if ! grep -Fq "$pattern" "$RENDERER"; then
    echo "[test_macos_renderer_node_reuse] $message" >&2
    return 1
  fi
}

require_source "bool viewMatchesNode(NSView *view" \
  "macOS renderer must verify cached NSViews against reused node slots"
require_source "if (view && !viewMatchesNode(view, node, tagName, inputType, checkboxButton))" \
  "macOS renderer must evict stale NSViews when a reused node id changes native kind"
require_source "[nodeIdToView() removeObjectForKey:key]" \
  "macOS renderer must remove evicted stale NSViews from the node cache"
