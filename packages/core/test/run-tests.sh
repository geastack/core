#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Keep the established order, then discover additional standalone tests so new
# files cannot silently fall out of the full runner.
NODE_TESTS=(
  test_native_integer_types.mjs
  test_display_internal_reserve.mjs
  test_embedded_weak_symbols.mjs
  test_gea_embedded_compat_transform.mjs
  test_gea_native_style_plugin.mjs
  test_component_startup.mjs
  test_gea_runtime_settings_gesture.mjs
  test_gea_vite_module_graph_assets.mjs
  test_generate_embedded_fonts.mjs
  test_radio_preinit_order.mjs
  test_render_dynamic_state.mjs
  test_static_css_codegen.mjs
  test_viewgeometry_active_cache.mjs
  test_web_build_sources.mjs
)

NATIVE_TESTS=(
  run-debugger-picker.sh
  run-packed-copy-overlap.sh
  run-bouncing-balls-regressions.sh
  run-text-sprite-cache.sh
  run-canvas-rounded-rect-alpha.sh
  run-transformed-rounded-rect.sh
  run-renderer-features.sh
  run-rotated-projected-text.sh
  run-position-storage.sh
  run-canvas-overlay.sh
  run-native-jpeg.sh
  run-image-lifetime.sh
  run-gea-retained-absolute-subtree.sh
)

PIPELINES=(
  run-gea-analog-clock-pipeline.sh
  run-gea-app-launcher-pipeline.sh
  run-gea-bouncing-balls-pipeline.sh
  run-gea-bouncing-balls-jsx-pipeline.sh
  run-gea-button-tetris-pipeline.sh
  run-gea-canvas-3d-pipeline.sh
  run-gea-counter-pipeline.sh
  run-gea-hid-clicker-pipeline.sh
  run-gea-sky-hop-canvas-pipeline.sh
  run-gea-sky-hop-jsx-pipeline.sh
  run-gea-settings-pipeline.sh
  run-gea-static-card-pipeline.sh
  run-gea-stopwatch-pipeline.sh
  run-gea-tic-tac-toe-pipeline.sh
  run-gea-tilt-breakout-pipeline.sh
  run-gea-todo-pipeline.sh
  run-gea-typography-pipeline.sh
  run-gea-watch-face-pipeline.sh
  run-gea-watch-analog-pipeline.sh
)

for node_test in "${NODE_TESTS[@]}"; do
  echo "==> $node_test"
  GEA_THREE_AUDIO_MUTED=1 node "$TEST_DIR/$node_test"
done

for path in "$TEST_DIR"/test_*.mjs; do
  node_test="${path##*/}"
  case " ${NODE_TESTS[*]} " in *" $node_test "*) continue ;; esac
  echo "==> $node_test"
  GEA_THREE_AUDIO_MUTED=1 node "$path"
done

for test_script in "${NATIVE_TESTS[@]}"; do
  echo "==> $test_script"
  bash "$TEST_DIR/$test_script"
done

for pipeline in "${PIPELINES[@]}"; do
  echo "==> $pipeline"
  bash "$TEST_DIR/$pipeline"
done

for path in "$TEST_DIR"/run-*.sh; do
  script="${path##*/}"
  case "$script" in
    run-tests.sh|run-gea-native-app-pipeline.sh|run-compiled-pcm-worklet.sh) continue ;;
  esac
  case " ${NATIVE_TESTS[*]} ${PIPELINES[*]} " in *" $script "*) continue ;; esac
  echo "==> $script"
  bash "$path"
done

echo "All standalone GEA tests and native example pipelines passed."
