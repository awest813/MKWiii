#!/usr/bin/env bash
# Web counterpart of Launcher/Test-Recompilation.ps1: translates an entirely synthetic DOL (no game
# dump, symbol map or mod involved) and builds the real runtime product around it with Emscripten.
#
#   runtime/web/synthetic-recomp.sh <fresh-stage-dir> [--parallel N] [-- extra CMake arguments]
#
# Needs: the .NET SDK (the translator targets net8.0; an SDK whose C# compiler is new enough for
# its source generators, e.g. 10.0), Emscripten on PATH (emsdk_env.sh), CMake, Ninja, Python 3, and
# aurora-main with runtime/web/aurora/aurora-web.patch applied (runtime/web/apply-aurora-patch.sh).
# The product lands in <stage>/web-build; runtime/web/run-product.mjs runs it in a browser.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
stage="${1:?usage: synthetic-recomp.sh <fresh-stage-dir> [--parallel N] [-- cmake args]}"
shift
parallel=$(nproc)
cmake_args=()
while (($#)); do
    case "$1" in
        --parallel) parallel=$2; shift 2 ;;
        --) shift; cmake_args=("$@"); break ;;
        *) echo "synthetic-recomp: unknown argument $1" >&2; exit 2 ;;
    esac
done
# Refuse reuse so an earlier build or a real game translation cannot make this pass.
if [[ -e "$stage" ]]; then
    echo "synthetic-recomp: stage already exists; choose a fresh directory: $stage" >&2
    exit 1
fi
mkdir -p "$stage"
stage="$(cd "$stage" && pwd)"

# Current sources, including uncommitted edits (the aurora patch adds files), but no build output.
copy_tree() {
    local repo="$1" prefix="$2"
    git -C "$repo" -c core.quotepath=false ls-files -z --cached --others --exclude-standard -- "${@:3}" |
        while IFS= read -r -d '' file; do
            [[ -f "$repo/$file" ]] || continue
            mkdir -p "$stage/$prefix$(dirname "$file")"
            cp "$repo/$file" "$stage/$prefix$file"
        done
}
copy_tree "$root" "" runtime
copy_tree "$root/aurora-main" "aurora-main/"

# One synthetic text section: li r3,40; addi r3,r3,2; nop; blr. The runtime resolves the guest
# functions it calls directly through the registry at run time, so the product links without any
# game code.
python3 - "$stage" <<'EOF'
import pathlib, struct, sys
stage = pathlib.Path(sys.argv[1])
entry, text_size = 0x80001000, 0x10
data_offset = 0x100 + text_size
dol = bytearray(data_offset + 4)
def put(offset, value): struct.pack_into('>I', dol, offset, value)
put(0x00, 0x100)          # text[0] file offset
put(0x48, entry)          # text[0] guest address
put(0x90, text_size)      # text[0] length
put(0x1C, data_offset)    # data[0] file offset
put(0x64, 0x80600000)     # data[0] guest address
put(0xAC, 4)              # data[0] length
put(0xD8, 0x80601000)     # BSS address
put(0xDC, 32)             # BSS length
put(0xE0, entry)          # entry point
for i, word in enumerate((0x38600028, 0x38630002, 0x60000000, 0x4E800020)):
    put(0x100 + 4 * i, word)
put(data_offset, 0x12345678)
(stage / 'synthetic.dol').write_bytes(dol)
(stage / 'synthetic-functions.txt').write_text(f'{entry:08X} func_{entry:08X}')
(stage / 'recomp.yml').write_text(f'''schema_version: 1
workspace_root: .
project:
  id: ci-synthetic-dol
  display_name: CI Synthetic DOL
memory:
  base: 0x80000000
  size: 0x01800000
  sda_base: 0x80600000
  sda2_base: 0x80600000
inputs:
  dol:
    path: synthetic.dol
translation:
  entry_points: [0x{entry:08X}]
  function_map:
    path: synthetic-functions.txt
  allow_unsupported_instructions: false
runtime:
  native_abi_directories: []
  native_registration_root: runtime/src
output:
  root: generated
''')
EOF

dotnet build "$root/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release --disable-build-servers -v quiet
translator="$root/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
manifest="$stage/recomp.yml"
metadata="$stage/generated/base_translation_output.json"
dotnet "$translator" translate-recursive 0x80001000 --project "$manifest" \
    --output-metadata "$metadata" --threads "$parallel"
# Function-map seeds can be skipped by discovery; do not accept a partial fixture.
python3 - "$metadata" <<'EOF'
import json, sys
functions = json.load(open(sys.argv[1]))['functions']
if 0x80001000 not in {f['entryPoint'] for f in functions}:
    sys.exit('synthetic-recomp: synthetic function 0x80001000 was not translated')
EOF
dotnet "$translator" generate-data-init --project "$manifest"
dotnet "$translator" emit-build-shards --project "$manifest"

emcmake cmake -S "$stage/runtime" -B "$stage/web-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMKW_BUILD_PRODUCTS=ON "${cmake_args[@]}"
cmake --build "$stage/web-build" --target WiiCompiled --parallel "$parallel"
echo "synthetic-recomp: translated and built the synthetic product in $stage/web-build"
