#!/usr/bin/env bash
# Rebuild and profile original EE only. Existing paired results are not rerun.
# ci_score owns the report-log transport and chooses this diagnostic entry.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?ci_score must declare MINIVEC_REPORTS}/from-best"
mkdir -p "$out"
test ! -e "$out/ee-profile"
test ! -e "$out/ee-profile-setup"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-profile-only-XXXXXX")
baseline=ee299adb296a2922d01982aa6c5707d37e767b41
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'

# Preserve the historical root image recipe and compiler, including /src paths.
git fetch --no-tags --depth=1 origin "$baseline"
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline"
docker build --platform linux/amd64 --progress=plain -t minivec:ee-profile-1114 "$scratch/baseline" 2>&1 | tee "$out/baseline-build.txt"
container=$(docker create minivec:ee-profile-1114)
docker cp "$container:/minivec" "$scratch/baseline-binary"
docker rm "$container"
docker image inspect minivec:ee-profile-1114 > "$out/baseline-image.json"
sha256sum "$scratch/baseline-binary" > "$out/binary-sha256.txt"
echo '9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd  '"$scratch/baseline-binary" | sha256sum -c -

# Symbols are usable only if the profiler verifies strip output byte-for-byte.
docker run --rm -v "$scratch/baseline/minivec:/src:ro" -v "$scratch:/out" -w /src "$compiler" \
  sh -c 'g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc src/*.cpp -o /out/ee-symbols'
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sha256sum "$scratch/ee-symbols" "$scratch/cgroup-exec" > "$out/profile-helper-sha256.txt"
printf 'source_commit=%s\ncompiler=%s\nmode=EE-only external sampling; no paired comparison or formal submission\n' "$baseline" "$compiler" > "$out/profile-only-scope.txt"
bash minivec/tools/ci_profile_ee_1114.sh "$scratch/baseline-binary" "$scratch/ee-symbols" "$scratch/cgroup-exec" "$out/ee-profile"
