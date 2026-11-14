#!/usr/bin/env bash
# Two independent factors, three fresh root images; no EE/R/release inference.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?missing report path}/batch-factors"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-batch-factors-XXXXXX")
baseline=760f66220f2c7186b4ba295f9e3065650e549f9e
factor_a=d0e3f9a7ed2dc76dfd77ed25963d7ca6cdd74530
factor_b=8d364e83b225dba54988a727b52cfa4a74fd241a
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
git rev-parse HEAD > "$out/harness-commit.txt"
git rev-parse HEAD:.github/workflows/minivec-score.yml > "$out/workflow-blob.txt"
lscpu > "$out/lscpu.txt"
g++ --version > "$out/native-compiler.txt"
docker version > "$out/docker-version.txt"
cat /proc/sys/kernel/random/boot_id > "$out/boot-id.txt"
git fetch --no-tags --depth=1 origin "$baseline" "$factor_a" "$factor_b"
python3 -B minivec/tools/verify_batch_sources_1114.py > "$out/source-verification.txt"
for factor in a b; do
  bundle="$repo/minivec/tools/batch_${factor}1114"
  cp "$bundle/plan.json" "$out/factor-$factor-frozen-plan.json"
  sha256sum "$bundle/plan.json" > "$out/factor-$factor-frozen-plan-sha256.txt"
done
# These source snapshots, not the mutable CI checkout, own all runtime bytes.
for role in baseline a b; do
  source=$baseline
  if [[ $role == a ]]; then source=$factor_a; fi
  if [[ $role == b ]]; then source=$factor_b; fi
  mkdir "$scratch/$role" "$out/$role"
  git archive "$source" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role"
  image="minivec:batch1114-$role"
  docker build --no-cache --platform linux/amd64 --progress=plain -t "$image" "$scratch/$role" 2>&1 | tee "$out/$role/build.txt"
  docker image inspect "$image" > "$out/$role/image.json"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  sha256sum "$scratch/$role-binary" > "$out/$role/binary-sha256.txt"
  if [[ $role == baseline ]]; then continue; fi
  python3 protocol_check/run.py --image "$image" --handshake-only --output "$out/$role/handshake" 2>&1 | tee "$out/$role/handshake.txt"
  python3 protocol_check/run.py --image "$image" --output "$out/$role/public" 2>&1 | tee "$out/$role/public.txt"
  python3 minivec/tests/image_interactive.py --image "$image" --output "$out/$role/interactive" 2>&1 | tee "$out/$role/interactive.txt"
  python3 minivec/tests/json_protocol.py --binary "$scratch/$role-binary" --timeout 15 2>&1 | tee "$out/$role/json.txt"
  python3 minivec/tests/topk_bounds.py --binary "$scratch/$role-binary" 2>&1 | tee "$out/$role/topk.txt"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/$role-binary" 2>&1 | tee "$out/$role/topk-parallel.txt"
  mkdir "$scratch/native-$role"
  git archive "$source" minivec/src minivec/third_party minivec/tests \
    "minivec/tools/run_batch_${role}_1114.sh" minivec/tools/ci_spill_guard_1114.sh \
    | tar -x -C "$scratch/native-$role"
  # Prove that native guards compile the same source bytes as the Docker image.
  diff -r "$scratch/$role/minivec/src" "$scratch/native-$role/minivec/src"
  bash "$scratch/native-$role/minivec/tools/run_batch_${role}_1114.sh" "$out/$role/native"
done
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
for factor in a b; do
  bundle="$repo/minivec/tools/batch_${factor}1114"
  mkdir "$out/$factor/paired"
  python3 -B "$bundle/verify_root.py" --role baseline --context "$scratch/baseline" \
    --binary "$scratch/baseline-binary" --image-inspect "$out/baseline/image.json" --output "$out/$factor/paired/baseline-build-proof.json"
  python3 -B "$bundle/verify_root.py" --role candidate --context "$scratch/$factor" \
    --binary "$scratch/$factor-binary" --image-inspect "$out/$factor/image.json" --output "$out/$factor/paired/candidate-build-proof.json"
  sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 -B "$bundle/run_ablation.py" \
    --baseline "$scratch/baseline-binary" --candidate "$scratch/$factor-binary" \
    --baseline-evidence "$out/$factor/paired/baseline-build-proof.json" \
    --candidate-evidence "$out/$factor/paired/candidate-build-proof.json" \
    --launcher "$scratch/cgroup-exec" --output "$out/$factor/paired/ablation.json"
done
git diff --exit-code -- Dockerfile .dockerignore minivec/src minivec/third_party .github/workflows
python3 - "$out" <<'PYFINAL'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1]);result={'status':'completed_all_native_and_paired_runs','formal_submission':False,'release_gate_passed':False,'factors':{}}
for factor in ('a','b'):
    path=out/factor/'paired/ablation.json';data=json.loads(path.read_bytes())
    result['factors'][factor]={'source':data['candidate_source'],'completed_pairs':data['completed_pairs'],
      'original31guards_passed':data['numerical_ablation_guards_passed'],'factor_thresholds_passed':data['factor_thresholds_passed'],
      'isolated_factor_acceptance_passed':data['isolated_factor_acceptance_passed'],'report_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
result['scope']='Two independently paired factors versus fresh760f. No A+B runtime, no multiplied ratios, no EE/R/release acceptance.'
(out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
PYFINAL
