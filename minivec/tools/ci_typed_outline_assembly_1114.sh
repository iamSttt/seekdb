#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/protocol-results/typed-outline-assembly"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/typed-outline-assembly-XXXXXX")
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
for role in base typed; do
  commit=760f66220f2c7186b4ba295f9e3065650e549f9e
  if [[ $role == typed ]]; then commit=6412cbf862959655f351b4d908c430a98c5eabfe; fi
  git fetch --no-tags --depth=1 origin "$commit"
  mkdir "$scratch/$role"
  git archive "$commit" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role"
done
cp -a "$scratch/typed" "$scratch/noinline"
python3 - "$scratch/noinline/minivec/src/exec.cpp" "$out/variant.patch" <<'PY'
from pathlib import Path
import difflib,sys
p=Path(sys.argv[1]);old=p.read_text()
needle='auto encode_typed_json_row = [&](const Row& row, const Candidate* candidate = nullptr) -> const std::string& {'
assert old.count(needle)==1
new=old.replace(needle,needle.replace(' -> ', ' __attribute__((noinline)) -> '))
p.write_text(new)
Path(sys.argv[2]).write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/minivec/src/exec.cpp',tofile='b/minivec/src/exec.cpp')))
PY
for role in base typed noinline; do
  mkdir "$out/$role"
  docker run --rm -v "$scratch/$role/minivec:/src:ro" -v "$out/$role:/out" -w /src "$compiler" bash -c '
    set -euo pipefail
    g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc src/*.cpp -o /minivec
    cp /minivec /out/unstripped.elf
    strip /minivec
    cp /minivec /out/root-equivalent.elf
    nm -S --size-sort --defined-only /out/unstripped.elf > /out/symbols.nm
    objdump -d -C /out/unstripped.elf > /out/full-disassembly.asm
    size -A /out/unstripped.elf > /out/sections.size
  ' > "$out/$role/build.txt" 2>&1
done
python3 -B minivec/tools/typed_outline1114/analyze_assembly.py \
  --output "$out" --contexts "$scratch" --plan validation/typed-outline-assembly-plan-20261114.json
git diff --exit-code -- Dockerfile .dockerignore minivec/src minivec/third_party
