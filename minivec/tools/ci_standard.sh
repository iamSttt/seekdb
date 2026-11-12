#!/usr/bin/env bash
# Representative public workload dimensions; all metrics remain local proxies.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_QUERY_THREADS MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-standard-XXXXXX")
test ! -e standard-results
mkdir standard-results "$scratch/baseline"
stage=build
dump_reports() {
  result=$?
  set +e
  printf '{"exit_code":%s,"stage":"%s"}\n' "$result" "$stage" > standard-results/run-status.json
  for file in standard-results/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_STANDARD_REPORT_BEGIN $file"
    cat "$file"
    echo "MINIVEC_STANDARD_REPORT_END"
  done
  exit "$result"
}
trap dump_reports EXIT
git archive a90b2b750e40f020793cddefe01d5b66631afc63 | tar -x -C "$scratch/baseline"
python3 - "$scratch/Dockerfile.lto" <<'PY'
from pathlib import Path
import sys
text = Path('Dockerfile').read_text()
needle = 'g++ -O3 -std=c++17'
if text.count(needle) != 1:
    raise SystemExit('unexpected compiler command; refusing an ambiguous LTO experiment')
Path(sys.argv[1]).write_text(text.replace(needle, 'g++ -O3 -flto=2 -std=c++17'))
PY
for side in baseline candidate lto; do
  stage="$side-build"
  source="$repo"
  dockerfile="$repo/Dockerfile"
  if [ "$side" = baseline ]; then source="$scratch/baseline"; dockerfile="$source/Dockerfile"; fi
  if [ "$side" = lto ]; then dockerfile="$scratch/Dockerfile.lto"; fi
  docker build --platform linux/amd64 -f "$dockerfile" -t "minivec:standard-$side" "$source"
  container=$(docker create "minivec:standard-$side")
  docker cp "$container:/minivec" "$scratch/$side.bin"
  docker rm "$container"
  sha256sum "$scratch/$side.bin" | tee "standard-results/$side-sha256.txt"
done
stage=lto-correctness
python3 minivec/tests/json_protocol.py --binary "$scratch/lto.bin" --timeout 15
python3 minivec/tests/topk_bounds.py --binary "$scratch/lto.bin"
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/lto.bin"
export STANDARD_SCRATCH="$scratch"
stage=standard-paired
python3 - <<'PY'
import json, os, platform, subprocess
from pathlib import Path
cpus = sorted(os.sched_getaffinity(0))[:2]
os.sched_setaffinity(0, cpus)
Path('standard-results/context.json').write_text(json.dumps({
    'baseline_commit': 'a90b2b750e40f020793cddefe01d5b66631afc63',
    'candidate_commit': subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    'tracked_worktree_status': subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],text=True),
    'platform': platform.platform(), 'cpu_affinity': cpus,
    'compiler': 'Pinned root Dockerfile GCC 13.3.0 static -O3; LTO variant adds only -flto=2',
    'limitations': 'Same-runner 2-CPU affinity, process RSS/I/O proxies. No official E or cgroup score.'
},indent=2)+'\n')
common = ['python3','minivec/tools/benchmark_iteration.py','--suite','full',
          '--scenario','standard10k','--queries','1000']
scratch = os.environ['STANDARD_SCRATCH']
for before, after, name in [('baseline','candidate','baseline-candidate'),('candidate','lto','candidate-lto')]:
    subprocess.run(common+['--baseline',scratch+'/'+before+'.bin',
        '--candidate',scratch+'/'+after+'.bin','--output','standard-results/'+name+'.json'],check=True)
PY
stage=complete
