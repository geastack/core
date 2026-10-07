#!/usr/bin/env bash

# The example apps this repo's pipelines compile are not part of this repo --
# they live in `geastack/examples`. So these pipelines are RUN FROM that app
# project, the same rule as every other tool in the stack: the directory you
# are standing in is the thing being compiled. There is no environment
# variable and nothing to guess. Run from anywhere else and the pipeline skips
# -- these tests validate the runtime against real apps, so no apps means
# nothing to assert, not a failure.
#
#   cd /path/to/examples && bash /path/to/core/packages/core/test/run-tests.sh

gea_require_app_project() {
  GEA_APP_PROJECT="$(pwd -P)"
  [ -d "$GEA_APP_PROJECT/apps" ] && return 0
  echo "SKIP: run this from an app project (a directory with an apps/ folder, e.g. a checkout of geastack/examples); cwd is $GEA_APP_PROJECT" >&2
  exit 0
}

# The Apple host sources a few of these tests compile against are in
# @geastack/apple, and core cannot depend on it -- apple sits above core in the
# stack and depends on this package. So the location is supplied, never guessed:
# point GEA_APPLE_ROOT at the @geastack/apple PACKAGE root (the directory with
# its package.json). The old spelling walked to an adjacent apple checkout and then
# looked under `targets/macos`, which has not been the layout since the split:
# the package lives at apple/packages/geastack-apple.
gea_require_apple_root() {
  if [ -n "${GEA_APPLE_ROOT:-}" ] && [ -d "${GEA_APPLE_ROOT}/targets/macos" ]; then return 0; fi
  echo "SKIP: set GEA_APPLE_ROOT to the @geastack/apple package root (the directory holding its package.json)" >&2
  exit 0
}

gea_native_sources() {
  local engine_root="${GEA_NATIVE_TEST_ENGINE_ROOT:-$ROOT/packages/engine}"
  local host_root="$ROOT/packages/host"
  local elements_root="${GEA_NATIVE_TEST_ELEMENTS_ROOT:-$ROOT/packages/elements}"
  GEA_NATIVE_SRCS=(
    "$engine_root/canvas.cpp"
    "$engine_root/bitmap_font.cpp"
    "$ROOT/packages/core/events.cpp"
    "$engine_root/image_store.cpp"
    "$host_root/host/image.cpp"
    "$host_root/host/media.cpp"
    "$host_root/host/fetch.cpp"
    "$host_root/host/http.cpp"
    "$host_root/host/timers.cpp"
    "$host_root/host/worker.cpp"
    "$engine_root/rasterized_font.cpp"
    "$engine_root/touch_runtime.cpp"
    "$engine_root/ui/absolute_leaf_refresh.cpp"
    "$engine_root/ui/canvas_element.cpp"
    "$elements_root/ui/camera_element.cpp"
    "$engine_root/ui/canvas_store.cpp"
    "$engine_root/ui/core.cpp"
    "$engine_root/ui/dirty_regions.cpp"
    "$engine_root/ui/display_invalidation.cpp"
    "$engine_root/ui/document.cpp"
    "$engine_root/ui/image_element.cpp"
    "$engine_root/ui/image_node.cpp"
    "$engine_root/ui/input.cpp"
    "$engine_root/ui/input_render.cpp"
    "$engine_root/ui/layout.cpp"
    "$engine_root/ui/layout_snapshot.cpp"
    "$engine_root/ui/node.cpp"
    "$engine_root/ui/node_lifecycle.cpp"
    "$engine_root/ui/render.cpp"
    "$engine_root/ui/root_scroll_refresh.cpp"
    "$engine_root/ui/style.cpp"
    "$engine_root/ui/css_atom.cpp"
    "$engine_root/ui/text.cpp"
    "$engine_root/ui/text_element.cpp"
    "$engine_root/ui/tree_events.cpp"
    "$engine_root/ui/tree_nodes.cpp"
    "$engine_root/ui/tree_render.cpp"
    "$engine_root/ui/tree_scroll_mirror.cpp"
    "$engine_root/ui/tree_state.cpp"
    "$engine_root/ui/tree_style.cpp"
    "$engine_root/ui/view.cpp"
    "$engine_root/ui/view_element.cpp"
    "$engine_root/ui/virtual_keyboard.cpp"
    "$elements_root/ui/virtual_list.cpp"
    "$engine_root/ui/viewport_region.cpp"
  )
}

gea_read_geatsc_sources() {
  local build_dir="$1"
  local source_list="$build_dir/geatsc-sources.txt"
  # Half of this suite does not run the compiler at all: it hand-writes a single
  # `program.cpp` stub (`void __gea_top_level() {}`) because what it exercises is
  # the ENGINE, not the emit. Before the split-TU migration `generated_sources`
  # was literally `("$build_dir/program.cpp")`, so those tests worked by
  # construction; the migration replaced that seed with the manifest and left
  # them with an empty source list and the misleading message "no generated
  # geatsc C++ sources found".
  #
  # The usual case for those tests is simply no manifest. The `-nt` arm covers
  # the other one: a manifest that DOES exist but is older than the stub the
  # caller just wrote is a leftover from some earlier pipeline build that
  # happened to use the same directory. (Both arms are spelled out rather than
  # leaning on bash's "true when file2 is absent" reading of `-nt`, which zsh
  # does not share.) The leftover is not hypothetical:
  # `.build/gea-style-viewport-metrics/` still holds a July manifest naming
  # `entry.cpp` + `modules/*.cpp` from a v1 multi-module emit, and those files
  # are all still on disk -- so without this the engine-only test silently
  # compiles two-month-old generated code instead of the `void __gea_top_level()
  # {}` stub it just wrote, and whatever it then reports is about neither.
  if [[ -f "$build_dir/program.cpp" ]] &&
     { [[ ! -f "$source_list" ]] || [[ "$build_dir/program.cpp" -nt "$source_list" ]]; }; then
    printf '%s\n' "$build_dir/program.cpp"
    return
  fi
  if [[ -f "$source_list" ]]; then
    local source
    while IFS= read -r source; do
      [[ -z "$source" ]] && continue
      if [[ ! -f "$source" ]]; then
        echo "geatsc source listed but missing: $source" >&2
        return 1
      fi
      printf '%s\n' "$source"
    done < "$source_list"
    return
  fi
}

source "$ROOT/packages/core/scripts/native-object-cache.sh"

gea_build_native_test() {
  local build_dir="$1"
  local output_bin="$2"
  local test_main="$3"
  shift 3
  local pixel_panel_endian="${GEA_EMBEDDED_NATIVE_PIXEL_PANEL_ENDIAN:-1}"
  # Explicit reference-engine input for allocation/behavior comparisons.
  # Normal tests keep the repository engine; no installed package is modified.
  local engine_root="${GEA_NATIVE_TEST_ENGINE_ROOT:-$ROOT/packages/engine}"
  local engine_vendor="$engine_root/vendor"

  gea_native_sources
  local native_sources=()
  local src
  for src in "${GEA_NATIVE_SRCS[@]}"; do
    if [[ "$src" != "$ROOT/packages/host/host/image.cpp" ]]; then
      native_sources+=("$src")
    fi
  done

  local generated_sources=()
  while IFS= read -r src; do
    generated_sources+=("$src")
  done < <(gea_read_geatsc_sources "$build_dir")
  if [[ ${#generated_sources[@]} -eq 0 ]]; then
    echo "no generated geatsc C++ sources found in $build_dir" >&2
    return 1
  fi
  # Engine-only tests supply this exact top-level stub and own their font
  # fixtures. A previous compiler pipeline may have left generated assets in
  # this shared output directory; their existence does not make them inputs to
  # the new stub program (and links duplicate ensureLinked definitions).
  local engine_only_stub=0
  if [[ ${#generated_sources[@]} -eq 1 && "${generated_sources[0]}" == "$build_dir/program.cpp" ]] &&
     [[ "$(tr -d '[:space:]' < "$build_dir/program.cpp")" == 'void__gea_top_level(){}' ]]; then
    engine_only_stub=1
  fi
  if [[ "$engine_only_stub" == 0 && -f "$build_dir/gea_embedded_font_generated.cpp" ]]; then
    generated_sources+=("$build_dir/gea_embedded_font_generated.cpp")
  fi
  if [[ "$engine_only_stub" == 0 && -f "$build_dir/gea_embedded_assets_generated.cpp" ]]; then
    generated_sources+=("$build_dir/gea_embedded_assets_generated.cpp")
  fi
  # No `gea_runtime.cpp` wrapper. It used to be written here as a one-line
  # `#include "$ROOT/packages/geatsc/dist/targets/cpp/runtime/runtime.cpp"` --
  # v1's runtime, a compiled translation unit under the old in-tree geatsc
  # workspace. That path has not existed since the compiler moved out of this
  # repo, and the file it wrote was never added to `generated_sources`, to
  # `GEA_NATIVE_CXX_SOURCES`, or to any link line -- so it compiled nothing and
  # only left a dangling include in every build directory for a reader to
  # mistake for the live runtime. geatsc's runtime is header-only: the compiler
  # writes `gea_runtime.h` into the output directory and the emitted unit
  # includes it, which is why `-iquote "$build_dir"` below is all that is needed.
  local cc_bin="${CC:-clang}"

  # The host BACKEND contract, always. Not inside the conditional below: an app
  # that never mentions bluetooth still references `HidBackend` today, because
  # `@geastack/core`'s runtime module wraps the whole host surface and the
  # compiler emits those wrappers as part of the module's object graph. See the
  # note at the top of native_host_backends.cpp.
  local -a test_host_sources=("$ROOT/packages/core/test/native_host_backends.cpp")
  if [[ "${GEA_NATIVE_TEST_INCLUDE_HOST:-1}" != "0" ]]; then
    test_host_sources+=("$ROOT/packages/core/test/native_test_host.cpp")
    # Host camera bridge; backed by the null platform camera stub in
    # native_test_host.cpp (camera apps link CameraBackend from generated code).
    test_host_sources+=("$ROOT/packages/core/test/native_camera_bridge.cpp")
  fi

  local -a extra_c_sources=()
  local -a extra_cxx_sources=()
  local -a extra_mm_sources=()
  local -a extra_compile_flags=()
  local -a extra_link_flags=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      *.c)
        extra_c_sources+=("$1")
        ;;
      *.cc|*.cpp|*.cxx)
        extra_cxx_sources+=("$1")
        ;;
      *.m|*.mm)
        extra_mm_sources+=("$1")
        ;;
      -D*|-I*)
        extra_compile_flags+=("$1")
        ;;
      -framework)
        extra_link_flags+=("$1")
        shift
        if [[ $# -eq 0 ]]; then
          echo "missing framework name after -framework" >&2
          return 2
        fi
        extra_link_flags+=("$1")
        ;;
      *)
        extra_link_flags+=("$1")
        ;;
    esac
    shift
  done

  local -a main_c_sources=()
  local -a main_cxx_sources=()
  local -a main_mm_sources=()
  case "$test_main" in
    *.c) main_c_sources+=("$test_main") ;;
    *.cc|*.cpp|*.cxx) main_cxx_sources+=("$test_main") ;;
    *.m|*.mm) main_mm_sources+=("$test_main") ;;
    *) main_cxx_sources+=("$test_main") ;;
  esac

  local -a common_cxx_flags=(
    -std=c++20
    # No -DGEA_CPP_VALUE_AVAILABLE. That macro tells the host headers that the
    # translation unit the COMPILER emitted defines `gea_cpp_value`, v1's boxed
    # dynamic carrier -- it gates the `operator=(gea_cpp_value)` callback
    # setters in host/websocket.h and host/http.h, which are written in terms of
    # it. geatsc does not emit that type: its box is `gea::Value`, and typed
    # values never reach a box at all. Defining the macro anyway made every
    # pipeline test fail to compile the host umbrella with "use of undeclared
    # identifier 'gea_cpp_value'" -- the header correctly believing a promise
    # this harness had stopped keeping.
    -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 \
    -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN="$pixel_panel_endian" \
    -Wno-deprecated-this-capture \
    -Wno-deprecated \
    -Wno-unused-value \
    -Wno-return-type \
    -Wno-parentheses-equality \
    -ffunction-sections \
    -fdata-sections \
    -ferror-limit=60 \
    # Generated runtime headers include one named string.h. Keep the output
    # directory on the quoted-include path only so libc++'s <cstring> cannot
    # accidentally resolve its system <string.h> include to that runtime header.
    -iquote "$build_dir" \
    -I "$ROOT/packages/core" \
    -I "$ROOT/packages/core/include" \
    -I "$ROOT/packages/core/test" \
    -I "$ROOT/packages/host" \
    -I "$ROOT/packages/host/include" \
    -I "$engine_root" \
    -I "$engine_root/ui" \
    -I "$ROOT/packages/elements" \
    -I "$ROOT/packages/elements/ui" \
    -I "$engine_vendor/AnimatedGIF" \
    -I "$engine_vendor/stb"
  )

  GEA_NATIVE_CACHE_BASE_DIR="$ROOT"
  GEA_NATIVE_CACHE_OBJ_ROOT="$build_dir/.build/obj"
  GEA_NATIVE_CACHE_OUTPUT="$output_bin"
  GEA_NATIVE_CC_BIN="$cc_bin"
  GEA_NATIVE_CXX_BIN="$CXX_BIN"
  GEA_NATIVE_C_SOURCES=(
    "$engine_vendor/AnimatedGIF/AnimatedGIF.c"
    ${main_c_sources[@]+"${main_c_sources[@]}"}
    ${extra_c_sources[@]+"${extra_c_sources[@]}"}
  )
  GEA_NATIVE_CXX_SOURCES=(
    ${generated_sources[@]+"${generated_sources[@]}"}
    ${native_sources[@]+"${native_sources[@]}"}
    ${test_host_sources[@]+"${test_host_sources[@]}"}
    ${main_cxx_sources[@]+"${main_cxx_sources[@]}"}
    ${extra_cxx_sources[@]+"${extra_cxx_sources[@]}"}
  )
  GEA_NATIVE_MM_SOURCES=(
    ${main_mm_sources[@]+"${main_mm_sources[@]}"}
    ${extra_mm_sources[@]+"${extra_mm_sources[@]}"}
  )
  GEA_NATIVE_CFLAGS=(
    -DGEA_EMBEDDED_GIF_C_API
    -I "$engine_vendor/AnimatedGIF"
    ${extra_compile_flags[@]+"${extra_compile_flags[@]}"}
  )
  GEA_NATIVE_CXXFLAGS=(
    ${common_cxx_flags[@]+"${common_cxx_flags[@]}"}
    ${extra_compile_flags[@]+"${extra_compile_flags[@]}"}
  )
  GEA_NATIVE_MMFLAGS=(
    ${common_cxx_flags[@]+"${common_cxx_flags[@]}"}
    ${extra_compile_flags[@]+"${extra_compile_flags[@]}"}
  )
  # Section GC at link time: ld64 spells it -dead_strip, GNU/LLVM ld --gc-sections.
  local link_gc=-Wl,--gc-sections
  if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
  GEA_NATIVE_LINK_FLAGS=(
    ${extra_link_flags[@]+"${extra_link_flags[@]}"}
    "$link_gc"
  )
  gea_native_compile_and_link
}
