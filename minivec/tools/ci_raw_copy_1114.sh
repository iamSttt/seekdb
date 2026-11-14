#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?}/raw-copy"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-raw-copy-XXXXXX")
baseline=760f66220f2c7186b4ba295f9e3065650e549f9e
git fetch --no-tags --depth=1 origin "$baseline"
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline"
git diff --exit-code "$baseline" HEAD -- Dockerfile .dockerignore minivec/third_party
python3 - <<'PY'
import hashlib,json
from pathlib import Path
p=json.loads(Path('validation/raw-copy-plan-20261114.json').read_bytes())
for name,digest in p['runtime_inputs'].items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest()==digest,name
PY
docker build --platform linux/amd64 --progress=plain -t minivec:raw-copy-parent "$scratch/baseline" 2>&1 | tee "$out/parent-build.txt"
for role in baseline candidate; do
  image="$IMAGE"
  source="$RUNTIME_COMMIT"
  if [[ "$role" == baseline ]]; then image=minivec:raw-copy-parent; source=$baseline; fi
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
  python3 - "$source" "$scratch/$role-binary" "$out/$role-proof.json" <<'PY'
import hashlib,json,sys
from pathlib import Path
p={'status':'verified_current_root_build','source_commit':sys.argv[1],
   'binary_sha256':hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest(),
   'scope':'Actual ELF extracted from current unchanged-root Docker build; not formal or release scoring.'}
Path(sys.argv[3]).write_text(json.dumps(p,indent=2)+'\n')
PY
done
echo 'fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456  '"$scratch/baseline-binary" | sha256sum -c -
python3 minivec/tests/validated_request_diff.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --expected-raw-nul-changes 0 --output "$out/request-differential.json"
python3 minivec/tests/short_decimal_protocol.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --output "$out/numeric-protocol.json"
python3 minivec/tests/shared_bound_topk.py --binary "$scratch/candidate-binary" --output "$out/shared-bound-topk.json"
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo --preserve-env=PATH python3 minivec/tools/best_screen_1114.py \
  --baseline "$scratch/baseline-binary" --baseline-source "$baseline" \
  --candidate "$scratch/candidate-binary" --candidate-source "$RUNTIME_COMMIT" \
  --launcher "$scratch/cgroup-exec" --output "$out/screen.json"
set +e
sudo --preserve-env=PATH python3 minivec/tools/raw_copy1114/source_factor.py \
  --baseline "$scratch/baseline-binary" --baseline-build-proof "$out/baseline-proof.json" \
  --candidate "$scratch/candidate-binary" --candidate-source "$RUNTIME_COMMIT" \
  --candidate-evidence "$out/candidate-proof.json" --launcher "$scratch/cgroup-exec" --output "$out/guards.json"
factor_exit=$?
set -e
python3 - "$out" "$factor_exit" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);s=json.loads((p/'screen.json').read_bytes())
g=json.loads((p/'guards.json').read_bytes()) if (p/'guards.json').exists() else {}
assert set(s['comparisons'])=={'1','2'} and s['status']=='screen_completed'
sys.path.insert(0,str(Path('minivec/tools/retained_guards1114').resolve()))
from retained_guards import rules
guard_times,guard_memory=rules.guard_metric_names('standard10k')
checks={};blocks=[]
for cpu,c in s['comparisons'].items():
    m=c['metrics']['ingest_cpu_usec'];orders=m['order_median_candidate_over_baseline']
    checks[cpu]=len(m['raw_pairs'])==6 and m['median_candidate_over_baseline']<=.98 and m['candidate_strictly_faster_pairs']>=5 and all(orders[x]<1 for x in ('AB','BA'))
    for name in guard_times+guard_memory:
        m=c['metrics'][name]
        threshold=1.05 if name in guard_memory else 1.02
        orders=m['order_median_candidate_over_baseline']; ratio=m['median_candidate_over_baseline']
        assert ratio is not None and all(orders.get(x) is not None for x in ('AB','BA'))
        if ratio>threshold and all(orders[x]>threshold for x in ('AB','BA')):
            blocks.append({'cpu':cpu,'metric':name,'threshold':threshold})
passed=s['completed_pairs']==12 and all(checks.values()) and not blocks and int(sys.argv[2])==0 and g.get('source_factor_guard_passed') is True and g.get('completed_pairs')==12
r={'status':'source_factor_passed' if passed else 'source_factor_blocked','ingest_checks':checks,'stable_default_regression_triggers':blocks,'extra_protection_exit':int(sys.argv[2]),'extra_protection_passed':g.get('source_factor_guard_passed') is True,'source_factor_passed':passed,'release_gate_passed':False,'official_score':None,'scope':'760f parent raw-retention factor; original source-generic12 plus separate protection12; all samples retained.'}
(p/'decision.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r))
if not passed:sys.exit(20)
PY
