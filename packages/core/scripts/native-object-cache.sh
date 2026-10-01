#!/usr/bin/env bash

# Shared native C/C++ object cache for geastack shell builds.
#
# Caller contract:
#   GEA_NATIVE_CACHE_BASE_DIR   stable root for deterministic relative object names
#   GEA_NATIVE_CACHE_OBJ_ROOT   cache root, default: $PWD/.build/obj
#   GEA_NATIVE_CACHE_OUTPUT     linked executable path
#   GEA_NATIVE_CC_BIN           C compiler, default: ${CC:-clang}
#   GEA_NATIVE_CXX_BIN          C++ linker/compiler, default: ${CXX:-clang++}
#   GEA_NATIVE_C_SOURCES        array of C sources
#   GEA_NATIVE_CXX_SOURCES      array of C++ sources
#   GEA_NATIVE_MM_SOURCES       array of Objective-C++ sources
#   GEA_NATIVE_CFLAGS           array of C compile flags
#   GEA_NATIVE_CXXFLAGS         array of C++ compile flags
#   GEA_NATIVE_MMFLAGS          array of Objective-C++ compile flags
#   GEA_NATIVE_LINK_FLAGS       array of link flags

gea_native_cpu_count() {
  local count=""
  count="$(sysctl -n hw.ncpu 2>/dev/null || true)"
  if [[ -z "$count" ]]; then count="$(nproc 2>/dev/null || true)"; fi
  if ! [[ "$count" =~ ^[0-9]+$ ]] || (( count < 1 )); then count=1; fi
  printf '%s\n' "$count"
}

gea_native_sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  else
    openssl dgst -sha256 | awk '{print $NF}'
  fi
}

gea_native_init_cache() {
  GEA_NATIVE_CACHE_BASE_DIR="${GEA_NATIVE_CACHE_BASE_DIR:-$(pwd)}"
  GEA_NATIVE_CACHE_OBJ_ROOT="${GEA_NATIVE_CACHE_OBJ_ROOT:-$(pwd)/.build/obj}"
  GEA_NATIVE_CC_BIN="${GEA_NATIVE_CC_BIN:-${CC:-clang}}"
  GEA_NATIVE_CXX_BIN="${GEA_NATIVE_CXX_BIN:-${CXX:-clang++}}"
  GEA_NATIVE_CACHE_JOBS="${GEA_NATIVE_CACHE_JOBS:-${GEA_NATIVE_JOBS:-$(gea_native_cpu_count)}}"
  if ! [[ "$GEA_NATIVE_CACHE_JOBS" =~ ^[0-9]+$ ]] || (( GEA_NATIVE_CACHE_JOBS < 1 )); then
    GEA_NATIVE_CACHE_JOBS=1
  fi

  GEA_NATIVE_CCACHE_PREFIX=()
  if [[ "${GEA_NATIVE_CCACHE:-1}" != "0" ]]; then
    if command -v sccache >/dev/null 2>&1; then
      GEA_NATIVE_CCACHE_PREFIX=(sccache)
    elif command -v ccache >/dev/null 2>&1; then
      GEA_NATIVE_CCACHE_PREFIX=(ccache)
    fi
  fi

  GEA_NATIVE_OBJECTS=()
  GEA_NATIVE_PIDS=()
  GEA_NATIVE_COMPILE_FAILED=0
  GEA_NATIVE_COMPILED=0
  GEA_NATIVE_REUSED=0
  GEA_NATIVE_LINKED=0
}

gea_native_compiler_version() {
  local compiler="$1"
  "$compiler" --version 2>/dev/null | head -n 1 || true
}

gea_native_join_flags() {
  local flag
  for flag in "$@"; do
    printf '%s\n' "$flag"
  done
}

gea_native_compile_signature() {
  local kind="$1"
  local compiler="$2"
  shift 2
  {
    printf 'geastack-native-object-cache-v1\n'
    printf 'kind=%s\n' "$kind"
    printf 'platform=%s\n' "$(uname -s 2>/dev/null || true)"
    printf 'arch=%s\n' "$(uname -m 2>/dev/null || true)"
    printf 'compiler=%s\n' "$compiler"
    printf 'compiler-version=%s\n' "$(gea_native_compiler_version "$compiler")"
    printf 'wrapper:\n'
    gea_native_join_flags ${GEA_NATIVE_CCACHE_PREFIX[@]+"${GEA_NATIVE_CCACHE_PREFIX[@]}"}
    printf 'flags:\n'
    gea_native_join_flags "$@"
  }
}

gea_native_link_signature() {
  {
    printf 'geastack-native-link-cache-v1\n'
    printf 'linker=%s\n' "$GEA_NATIVE_CXX_BIN"
    printf 'linker-version=%s\n' "$(gea_native_compiler_version "$GEA_NATIVE_CXX_BIN")"
    printf 'wrapper:\n'
    gea_native_join_flags ${GEA_NATIVE_CCACHE_PREFIX[@]+"${GEA_NATIVE_CCACHE_PREFIX[@]}"}
    printf 'flags:\n'
    gea_native_join_flags ${GEA_NATIVE_LINK_FLAGS[@]+"${GEA_NATIVE_LINK_FLAGS[@]}"}
    printf 'objects:\n'
    gea_native_join_flags ${GEA_NATIVE_OBJECTS[@]+"${GEA_NATIVE_OBJECTS[@]}"}
  }
}

gea_native_object_rel() {
  local src="$1"
  local kind="$2"
  local rel="$src"
  local base="${GEA_NATIVE_CACHE_BASE_DIR%/}"
  if [[ "$rel" == "$base/"* ]]; then
    rel="${rel#$base/}"
  else
    rel="${rel#/}"
  fi
  rel="${rel%.*}"
  rel="${rel//[^A-Za-z0-9._-]/__}"
  printf '%s.%s.o\n' "$rel" "$kind"
}

gea_native_input_is_newer() {
  # macOS Bash 3.2's builtin -nt compares whole seconds. Generated sources
  # (especially consecutive mutations of one path) can change after an object
  # was built within that same second. The system test preserves subsecond
  # timestamps for sources, headers and objects that require relinking.
  /bin/test "$1" -nt "$2"
}

gea_native_depfile_has_newer_input() {
  local depfile="$1"
  local obj="$2"
  [[ -f "$depfile" ]] || return 0
  local dep
  while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    dep="${dep//\\ / }"
    if [[ -e "$dep" ]] && gea_native_input_is_newer "$dep" "$obj"; then
      return 0
    fi
  done < <(sed -e ':again' -e '/\\$/N' -e 's/\\\n/ /' -e 'tagain' -e 's/^[^:]*: //' "$depfile" | tr ' ' '\n' | sed -e 's/\\$//' -e '/^$/d')
  return 1
}

gea_native_needs_compile() {
  local src="$1"
  local obj="$2"
  [[ -f "$obj" ]] || return 0
  [[ -f "$obj.d" ]] || return 0
  gea_native_input_is_newer "$src" "$obj" && return 0
  gea_native_depfile_has_newer_input "$obj.d" "$obj" && return 0
  return 1
}

gea_native_wait_for_slot() {
  (( ${#GEA_NATIVE_PIDS[@]} < GEA_NATIVE_CACHE_JOBS )) && return 0
  local pid="${GEA_NATIVE_PIDS[0]}"
  local rest=("${GEA_NATIVE_PIDS[@]:1}")
  if ! wait "$pid"; then
    GEA_NATIVE_COMPILE_FAILED=1
  fi
  GEA_NATIVE_PIDS=(${rest[@]+"${rest[@]}"})
}

gea_native_wait_for_all() {
  local pid
  for pid in ${GEA_NATIVE_PIDS[@]+"${GEA_NATIVE_PIDS[@]}"}; do
    if ! wait "$pid"; then
      GEA_NATIVE_COMPILE_FAILED=1
    fi
  done
  GEA_NATIVE_PIDS=()
  if (( GEA_NATIVE_COMPILE_FAILED != 0 )); then
    return 1
  fi
}

gea_native_queue_compile() {
  local kind="$1"
  local src="$2"
  shift 2
  local compiler="$1"
  shift
  local signature hash obj rel
  signature="$(gea_native_compile_signature "$kind" "$compiler" "$@")"
  hash="$(printf '%s' "$signature" | gea_native_sha256)"
  rel="$(gea_native_object_rel "$src" "$kind")"
  obj="$GEA_NATIVE_CACHE_OBJ_ROOT/$hash/$rel"
  GEA_NATIVE_OBJECTS+=("$obj")

  if ! gea_native_needs_compile "$src" "$obj"; then
    GEA_NATIVE_REUSED=$((GEA_NATIVE_REUSED + 1))
    return 0
  fi

  mkdir -p "$(dirname "$obj")"
  GEA_NATIVE_COMPILED=$((GEA_NATIVE_COMPILED + 1))
  if [[ "${GEA_NATIVE_CACHE_TRACE:-0}" == "1" ]]; then
    printf 'native-cache: compile %s\n' "$src" >&2
  fi
  (
    ${GEA_NATIVE_CCACHE_PREFIX[@]+"${GEA_NATIVE_CCACHE_PREFIX[@]}"} "$compiler" "$@" -MMD -MP -MF "$obj.d" -c "$src" -o "$obj"
  ) &
  GEA_NATIVE_PIDS+=("$!")
  gea_native_wait_for_slot
}

gea_native_link_needs_rebuild() {
  local output="$1"
  local sigFile="$2"
  local signature="$3"
  [[ -f "$output" ]] || return 0
  [[ -f "$sigFile" ]] || return 0
  [[ "$(cat "$sigFile")" == "$signature" ]] || return 0
  local obj
  for obj in "${GEA_NATIVE_OBJECTS[@]}"; do
    gea_native_input_is_newer "$obj" "$output" && return 0
  done
  return 1
}

gea_native_compile_and_link() {
  gea_native_init_cache
  local src
  for src in ${GEA_NATIVE_C_SOURCES[@]+"${GEA_NATIVE_C_SOURCES[@]}"}; do
    [[ -n "$src" ]] || continue
    gea_native_queue_compile c "$src" "$GEA_NATIVE_CC_BIN" ${GEA_NATIVE_CFLAGS[@]+"${GEA_NATIVE_CFLAGS[@]}"}
  done
  for src in ${GEA_NATIVE_CXX_SOURCES[@]+"${GEA_NATIVE_CXX_SOURCES[@]}"}; do
    [[ -n "$src" ]] || continue
    gea_native_queue_compile cxx "$src" "$GEA_NATIVE_CXX_BIN" ${GEA_NATIVE_CXXFLAGS[@]+"${GEA_NATIVE_CXXFLAGS[@]}"}
  done
  for src in ${GEA_NATIVE_MM_SOURCES[@]+"${GEA_NATIVE_MM_SOURCES[@]}"}; do
    [[ -n "$src" ]] || continue
    gea_native_queue_compile mm "$src" "$GEA_NATIVE_CXX_BIN" ${GEA_NATIVE_MMFLAGS[@]+"${GEA_NATIVE_MMFLAGS[@]}"}
  done
  gea_native_wait_for_all

  local linkSig linkSigFile
  linkSig="$(gea_native_link_signature)"
  linkSigFile="$GEA_NATIVE_CACHE_OUTPUT.link.sig"
  mkdir -p "$(dirname "$GEA_NATIVE_CACHE_OUTPUT")"
  if gea_native_link_needs_rebuild "$GEA_NATIVE_CACHE_OUTPUT" "$linkSigFile" "$linkSig"; then
    GEA_NATIVE_LINKED=1
    ${GEA_NATIVE_CCACHE_PREFIX[@]+"${GEA_NATIVE_CCACHE_PREFIX[@]}"} "$GEA_NATIVE_CXX_BIN" ${GEA_NATIVE_OBJECTS[@]+"${GEA_NATIVE_OBJECTS[@]}"} ${GEA_NATIVE_LINK_FLAGS[@]+"${GEA_NATIVE_LINK_FLAGS[@]}"} -o "$GEA_NATIVE_CACHE_OUTPUT"
    printf '%s\n' "$linkSig" > "$linkSigFile"
  fi

  printf 'native-cache: compiled=%s reused=%s linked=%s jobs=%s obj-root=%s\n' \
    "$GEA_NATIVE_COMPILED" "$GEA_NATIVE_REUSED" "$GEA_NATIVE_LINKED" "$GEA_NATIVE_CACHE_JOBS" "$GEA_NATIVE_CACHE_OBJ_ROOT" >&2
}
