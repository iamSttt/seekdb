#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo= output=
while (($#)); do
  case "$1" in
    --repo) repo=$2;shift 2;;
    --output) output=$2;shift 2;;
    *) echo "unknown argument: $1" >&2;exit 2;;
  esac
done
test -n "$repo" && test -n "$output"
repo=$(cd "$repo" && pwd)
test ! -e "$output"
mkdir -p "$output"
output=$(cd "$output" && pwd)
export PYTHONDONTWRITEBYTECODE=1
python3 "$here/prepare.py" --repo "$repo" --output "$output/programs" 2>&1 | tee "$output/prepare.log"
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
printf '%s\n' "$compiler" > "$output/compiler-image.txt"
docker pull "$compiler" > "$output/compiler-pull.log" 2>&1
docker image inspect "$compiler" > "$output/compiler-inspect.json"
# Build-only container. Its limits are not represented as the DB runtime limits.
docker run --rm --network=none -v "$output/programs:/diagnostic" -w /diagnostic "$compiler" bash -c '
  set -euo pipefail
  g++ --version
  flags=(-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc
    -DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON -DMVDB_TYPED_WIDE_VECTOR_ONLY -DMVDB_MULTI_POSTING_REUSE_ONLY
    -DMVDB_COST_DIAGNOSTIC)
  printf "compiler_flags=";printf "%q " "${flags[@]}";printf "\n"
  g++ "${flags[@]}" src/*.cpp -o diagnostic-minivec
  g++ -O3 -std=c++17 -static -Isrc -DMVDB_COST_DIAGNOSTIC timer_overhead.cpp -o timer-overhead
  sha256sum diagnostic-minivec retained-minivec timer-overhead
' 2>&1 | tee "$output/compile.log"
cc --version > "$output/launcher-compiler.txt"
cc -O2 -static "$repo/minivec/tools/cgroup_exec.c" -o "$output/cgroup_exec" 2>&1 | tee "$output/launcher-build.log"
sha256sum "$repo/minivec/tools/cgroup_exec.c" "$output/cgroup_exec" > "$output/launcher-sha256.txt"
lscpu > "$output/lscpu.txt"
# Empty timer probe runs on the selected CPU; not a database measurement.
cpu=$(python3 -c 'import os;print(min(os.sched_getaffinity(0)))')
taskset --cpu-list "$cpu" "$output/programs/timer-overhead" > "$output/timer-overhead.json"
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$here/run.py" --repo "$repo" --output "$output" 2>&1 | tee "$output/measurements.log"
