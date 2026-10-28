#!/usr/bin/env bash
# Builds runtime/tests/isa_differential_tests.cpp natively and for wasm32, runs the wasm builds
# under Node and diffs their per-helper digests against the native ones. Exit status is nonzero
# on any difference.
#
#   runtime/web/isa-differential.sh [iterations [trace-op]]
#
# Wasm toolchains, each used when present (at least one is required):
#   - WASI: clang with the wasm32 target, wasm-ld and a wasm32-wasi sysroot with libc++
#     (Debian/Ubuntu: wasi-libc libc++-18-dev-wasm32 libc++abi-18-dev-wasm32
#     libclang-rt-18-dev-wasm32). Override the sysroot with WASI_SYSROOT.
#   - Emscripten: em++ on PATH (source emsdk_env.sh), or EMXX.
# Node 18+ runs both.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${MKW_ISA_DIFF_DIR:-$(mktemp -d)}"
cxx="${CXX:-clang++}"
sysroot="${WASI_SYSROOT:-/usr}"
emxx="${EMXX:-$(command -v em++ || true)}"
flags=(-O2 -std=c++20 -fno-fast-math -ffp-contract=off
       -I"$root/runtime/tests/psq_memory" -I"$root/runtime/include/isa" -I"$root/runtime/include")
# The flags every web build uses (see runtime/CMakeLists.txt, MKW_PLATFORM_WEB).
wasm_flags=(-msimd128 -mno-nontrapping-fptoint)
src="$root/runtime/tests/isa_differential_tests.cpp"

native_flags=()
native_is_x86=false
case "$(uname -m)" in x86_64|amd64) native_flags=(-march=x86-64-v3); native_is_x86=true ;; esac

# [host-daz] digests are expected to differ, and [host-cast] ones only match an x86 host; see
# the header of isa_differential_tests.cpp.
skip='\[host-daz\]'
$native_is_x86 || skip="$skip\\|\[host-cast\]"
filter() { grep -v "$skip" "$1" > "$2" || true; }

"$cxx" "${flags[@]}" "${native_flags[@]}" "$src" -o "$out/isa_native"
"$out/isa_native" "$@" > "$out/native.full.txt"
filter "$out/native.full.txt" "$out/native.txt"

compared=0
failed=0
compare() {
    local name="$1"
    filter "$out/$name.full.txt" "$out/$name.txt"
    if diff -u "$out/native.txt" "$out/$name.txt"; then
        echo "isa-differential: native and $name digests are identical"
    else
        echo "isa-differential: native and $name differ" >&2
        failed=1
    fi
    compared=$((compared + 1))
}

if [[ -d "$sysroot/include/wasm32-wasi/c++/v1" ]]; then
    "$cxx" --target=wasm32-wasi --sysroot="$sysroot" -isystem "$sysroot/include/wasm32-wasi/c++/v1" \
        "${wasm_flags[@]}" -fno-exceptions "${flags[@]}" "$src" -o "$out/isa.wasm"
    # Write to files, not pipes: WASI stdio in Node drops output when a pipe would block.
    node --no-warnings "$root/runtime/web/run-wasi.mjs" "$out/isa.wasm" "$@" > "$out/wasi.full.txt"
    compare wasi
fi

if [[ -n "$emxx" ]]; then
    "$emxx" "${wasm_flags[@]}" "${flags[@]}" "$src" -o "$out/isa_emscripten.js"
    node "$out/isa_emscripten.js" "$@" > "$out/emscripten.full.txt"
    compare emscripten
fi

if [[ $compared -eq 0 ]]; then
    echo "isa-differential: no wasm toolchain found (WASI sysroot or em++)" >&2
    exit 2
fi
echo "isa-differential: outputs in $out"
exit $failed
