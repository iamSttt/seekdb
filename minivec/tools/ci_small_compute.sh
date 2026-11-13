#!/usr/bin/env bash
# Twelve pairs from an already verified executable, without rebuilding PGO.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(git rev-parse --show-toplevel)
cd "$repo"
out="$repo/small-compute-results"
test ! -e "$out"
mkdir "$out"
printf '%s\n' prepare > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  python3 - "$out" "$code" <<'PY'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1]);code=int(sys.argv[2])
path=out/'measurements.json'
data=json.loads(path.read_bytes()) if path.exists() else {}
passed=code==0 and data.get('status')=='passed' and data.get('completed_pairs')==12
summary={'status':'passed' if passed else 'failed_or_incomplete','exit_code':code,
    'completed_pairs':data.get('completed_pairs',0),'expected_pairs':12,
    'comparison_kind':'same_binary_environment_control','new_pgo_builds':0,
    'official_score':None,'automatic_adoption':False,'formal_submission':False,
    'stage':(out/'stage.txt').read_text().strip()}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
manifest={'artifacts':[{'path':p.relative_to(out).as_posix(),'bytes':p.stat().st_size,
    'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
    for p in sorted(out.rglob('*')) if p.is_file() and p.name!='hashes.json']}
(out/'hashes.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(summary))
sys.exit(0 if passed else 1)
PY
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  exit "$code"
}
trap finish EXIT
git rev-parse HEAD > "$out/harness-commit.txt"
lscpu > "$out/lscpu.txt"
python3 minivec/tools/prepare_small_compute.py --repo "$repo" \
  --archive "$repo/validation/iteration4-bulk-loop-raw.zip" --output "$out/retained" \
  --candidate-policy "${MVDB_SMALL_COMPUTE_POLICY:-serial}" \
  2>&1 | tee "$out/prepare.log"
# Only this tiny launcher is compiled. The measured database is imported by
# immutable hash and source proof; there is no new database or PGO build.
cc --version > "$out/launcher-compiler.txt"
cc -O2 -static minivec/tools/cgroup_exec.c -o "$out/cgroup_exec" \
  2>&1 | tee "$out/launcher-build.log"
sha256sum minivec/tools/cgroup_exec.c "$out/cgroup_exec" > "$out/launcher-sha256.txt"
printf '%s\n' twelve-pairs > "$out/stage.txt"
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$repo/minivec/tools/small_compute.py" \
  --tools "$repo/minivec/tools" --identities "$out/retained/identities.json" \
  --launcher "$out/cgroup_exec" --output "$out/measurements.json" \
  --cpu-quota-us 100000 --cpu-period-us 100000 --case-timeout 600 \
  2>&1 | tee "$out/measurements.log"
printf '%s\n' complete > "$out/stage.txt"
