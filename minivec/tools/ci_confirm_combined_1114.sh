#!/usr/bin/env bash
# Confirm an unchanged, already gated runtime on a new runner and seed.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?}/confirmation-root"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-confirm-XXXXXX")
plan=validation/confirmation-run-20261114.json
python3 - "$plan" "$out/bound-inputs.json" <<'PY'
import hashlib,json,re,sys
from pathlib import Path
p=json.loads(Path(sys.argv[1]).read_bytes())
for k in ('initial_screen','initial_runner','initial_retained_gate','initial_retained_report'):
    f=Path(p[k]['path'])
    assert hashlib.sha256(f.read_bytes()).hexdigest()==p[k]['sha256'], k
s=json.loads(Path(p['initial_screen']['path']).read_bytes())
r=json.loads(Path(p['initial_retained_gate']['path']).read_bytes())
g=json.loads(Path(p['initial_retained_report']['path']).read_bytes())
assert s['all_EE_primary_thresholds_passed'] is True
assert r['retained_guard_stage_passed'] is True
assert r['checks'] and all(v is True for v in r['checks'].values())
assert r['candidate_source_commit']==p['candidate_runtime_commit']
assert r['candidate_binary_sha256']==p['candidate_elf_sha256']
assert r['guards_report_sha256']==p['initial_retained_report']['sha256']
assert s['completed_pairs']==12
assert re.fullmatch('[0-9a-f]{40}',p['candidate_runtime_commit'])
assert s['identities']['candidate']['source_commit']==p['candidate_runtime_commit']
assert s['identities']['candidate']['binary_sha256']==p['candidate_elf_sha256']
assert s['identities']['baseline']['source_commit']=='ee299adb296a2922d01982aa6c5707d37e767b41'
assert s['identities']['baseline']['binary_sha256']=='9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd'
assert g['status']=='retained_guards_complete_no_trigger'
assert g['numerical_retained_guards_passed'] is True
assert g['expected_pairs']==g['completed_pairs']==12
assert g['candidate_source']==p['candidate_runtime_commit']
assert g['identities']['candidate']['source_commit']==p['candidate_runtime_commit']
assert g['identities']['candidate']['binary_sha256']==p['candidate_elf_sha256']
sys.path.insert(0,str(Path('minivec/tools/retained_guards1114').resolve()))
from retained_guards import numerical_guards
recomputed=numerical_guards(g['arms'])
assert recomputed['numerical_retained_guards_passed'] is True
assert recomputed['guard_metrics']==g['guard_metrics']
assert recomputed['guard_triggers']==g['guard_triggers']==[]
Path(sys.argv[2]).write_text(json.dumps(p,indent=2)+'\n')
PY
candidate_source=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["candidate_runtime_commit"])' "$plan")
initial_screen=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initial_screen"]["path"])' "$plan")
initial_runner=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["initial_runner"]["path"])' "$plan")
for role in baseline candidate; do
  source=$candidate_source
  if [[ "$role" == baseline ]]; then source=ee299adb296a2922d01982aa6c5707d37e767b41; fi
  git fetch --no-tags --depth=1 origin "$source"
  mkdir "$scratch/$role-context"
  git archive "$source" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role-context"
  image="minivec:confirmation-$role-${GITHUB_RUN_ID:?}"
  docker build --platform linux/amd64 --progress=plain -t "$image" "$scratch/$role-context" 2>&1 | tee "$out/$role-build.txt"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
  python3 - "$role" "$source" "$scratch/$role-binary" "$initial_screen" "$out" <<'PY'
import hashlib,json,sys
from pathlib import Path
role,source,binary,screen,out=sys.argv[1:]
out=Path(out); expected=json.loads(Path(screen).read_bytes())['identities'][role]
digest=hashlib.sha256(Path(binary).read_bytes()).hexdigest()
assert source==expected['source_commit'] and digest==expected['binary_sha256']
proof={'source_commit':source,'binary_sha256':digest,
       'image_id':json.loads((out/(role+'-image.json')).read_bytes())[0]['Id'],
       'build_log_sha256':hashlib.sha256((out/(role+'-build.txt')).read_bytes()).hexdigest(),
       'source_method':'git archive of exact runtime commit; unchanged root Docker rebuild',
       'same_elf_as_initial_gated_run':True,'official_score':None}
(out/(role+'-file-identity.json')).write_text(json.dumps(proof,indent=2)+'\n')
PY
done
# Tooling/evidence may advance, but the branch's runtime must still be identical.
git diff --exit-code "$candidate_source" HEAD -- Dockerfile .dockerignore minivec/src minivec/third_party
cc -O2 -Wall -Wextra -static minivec/tools/confirmation1114/vendor/cgroup_exec.c -o "$scratch/cgroup-exec"
bash minivec/tools/confirmation1114/ci_confirmation.sh \
  "$scratch/baseline-binary" "$scratch/candidate-binary" "$candidate_source" \
  "$out/baseline-file-identity.json" "$out/candidate-file-identity.json" \
  "$initial_screen" "$initial_runner" "$scratch/cgroup-exec" "$out/confirmation19087"
