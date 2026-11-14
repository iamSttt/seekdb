#!/usr/bin/env bash
# Separately frozen parent9ed-versus-spill ablation. No main EE or R matrix.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?missing report path}/spill-ablation"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-spill-ablation-XXXXXX")
baseline=9ed5baa1b1363ebd388850e74271b353954902da
candidate=2bbe6d00f2dc97182d3021f21449473581d623d2
bundle="$repo/minivec/tools/spill_ablation1114"
git rev-parse HEAD > "$out/harness-commit.txt"
cp "$bundle/plan.json" "$out/frozen-plan.json"
sha256sum "$bundle/plan.json" > "$out/frozen-plan-sha256.txt"
lscpu > "$out/lscpu.txt"
git fetch --no-tags --depth=1 origin "$baseline" "$candidate"
for role in baseline candidate; do
  source=$baseline
  if [ "$role" = candidate ]; then source=$candidate; fi
  mkdir "$scratch/$role"
  git archive "$source" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role"
  image="minivec:spill-ablation-$role"
  docker build --no-cache --platform linux/amd64 --progress=plain -t "$image" "$scratch/$role" 2>&1 | tee "$out/$role-build.txt"
  docker image inspect "$image" > "$out/$role-image.json"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  python3 -B "$bundle/verify_root.py" --role "$role" --context "$scratch/$role" \
    --binary "$scratch/$role-binary" --image-inspect "$out/$role-image.json" --output "$out/$role-build-proof.json"
done
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 -B "$bundle/run_ablation.py" \
  --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" \
  --baseline-evidence "$out/baseline-build-proof.json" --candidate-evidence "$out/candidate-build-proof.json" \
  --launcher "$scratch/cgroup-exec" --output "$out/ablation.json"
git diff --exit-code -- Dockerfile .dockerignore minivec/src minivec/third_party .github/workflows
