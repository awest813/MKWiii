#!/usr/bin/env bash
# Builds runtime/tests/isa_differential_tests.cpp natively and for wasm32 (WASI sysroot, run under
# Node) and diffs the per-helper digests. Exit status is nonzero on any difference.
#
#   runtime/web/isa-differential.sh [iterations [trace-op]]
#
# Needs clang with the wasm32 target, wasm-ld and a wasm32-wasi sysroot with libc++ (Debian/Ubuntu:
# wasi-libc libc++-18-dev-wasm32 libc++abi-18-dev-wasm32 libclang-rt-18-dev-wasm32), and Node 18+.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${MKW_ISA_DIFF_DIR:-$(mktemp -d)}"
cxx="${CXX:-clang++}"
sysroot="${WASI_SYSROOT:-/usr}"
flags=(-O2 -std=c++20 -fno-fast-math -ffp-contract=off
       -I"$root/runtime/tests/psq_memory" -I"$root/runtime/include/isa" -I"$root/runtime/include")
src="$root/runtime/tests/isa_differential_tests.cpp"

native_flags=()
case "$(uname -m)" in x86_64|amd64) native_flags=(-march=x86-64-v3) ;; esac
"$cxx" "${flags[@]}" "${native_flags[@]}" "$src" -o "$out/isa_native"
"$cxx" --target=wasm32-wasi --sysroot="$sysroot" -isystem "$sysroot/include/wasm32-wasi/c++/v1" \
    -msimd128 -fno-exceptions "${flags[@]}" "$src" -o "$out/isa.wasm"

# Write to files, not pipes: WASI stdio in Node drops output when a pipe would block.
"$out/isa_native" "$@" > "$out/native.full.txt"
node --no-warnings "$root/runtime/web/run-wasi.mjs" "$out/isa.wasm" "$@" > "$out/wasm.full.txt"
# [host-daz] digests are expected to differ; see the header of isa_differential_tests.cpp.
grep -v '\[host-daz\]' "$out/native.full.txt" > "$out/native.txt" || true
grep -v '\[host-daz\]' "$out/wasm.full.txt" > "$out/wasm.txt" || true
if diff -u "$out/native.txt" "$out/wasm.txt"; then
    echo "isa-differential: native and wasm digests are identical ($out)"
else
    echo "isa-differential: native and wasm differ ($out)" >&2
    exit 1
fi
