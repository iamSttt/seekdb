#!/usr/bin/env bash
# Local integer-kernel prototype only: no database or PGO candidate build.
set -euo pipefail
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
source_dir="$repo/minivec/tools/quant_batch"
test ! -e "$output"
mkdir -p "$output"
output=$(cd "$output" && pwd)
image="minivec-quant-prototype:${GITHUB_RUN_ID:-$$}"
container="minivec-quant-prototype-${GITHUB_RUN_ID:-$$}"
created=0
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ "$created" == 1 ]]; then
    docker inspect "$container" > "$output/container-final-inspect.json"
    docker rm -f "$container" > "$output/container-cleanup.log" 2>&1
  fi
  printf '{"exit_code":%d,"database_runs":0,"root_pgo_builds":0}\n' "$code" > "$output/ci-status.json"
  exit "$code"
}
trap finish EXIT
git -C "$repo" rev-parse HEAD > "$output/harness-commit.txt"
git -C "$repo" ls-files -z minivec/tools/quant_batch .github/workflows/minivec-score.yml | \
  (cd "$repo" && xargs -0 sha256sum) > "$output/input-sha256.txt"
docker version > "$output/docker-version.txt"
docker build --platform linux/amd64 --progress=plain --file "$source_dir/Dockerfile.prototype" \
  -t "$image" "$source_dir" 2>&1 | tee "$output/tooling-image-build.log"
docker image inspect "$image" > "$output/tooling-image-inspect.json"
cpu=$(python3 -c 'import os;print(min(os.sched_getaffinity(0)))')
printf '%s\n' "$cpu" > "$output/requested-cpu.txt"
docker create --name "$container" --network=none --cpuset-cpus "$cpu" --cpus=1 \
  --memory=2147483648 --memory-swap=2147483648 --pids-limit=128 \
  -e PYTHONDONTWRITEBYTECODE=1 -v "$source_dir:/prototype:ro" -v "$output:/out:rw" \
  -w /prototype "$image" python3 /prototype/run_linux.py --output /out/results --pairs 4 --queries 64 \
  > "$output/container-id.txt"
created=1
docker inspect "$container" > "$output/container-start-inspect.json"
docker start -a "$container" 2>&1 | tee "$output/run.log"
# docker start -a's client status is not a substitute for the container status.
code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
test "$code" = 0
python3 - "$output" <<'PY'
import json,sys
from pathlib import Path
out=Path(sys.argv[1])
results=out/'results'
quota,period=(int(x) for x in (results/'cpu-max.txt').read_text().split())
assert quota==period and period>0, 'actual CPU quota is not one CPU'
assert (results/'memory-max.txt').read_text().strip()=='2147483648', 'actual memory limit differs'
assert (results/'memory-swap-max.txt').read_text().strip()=='0', 'actual swap limit differs'
summary=json.loads((results/'summary.json').read_bytes())
requested=int((out/'requested-cpu.txt').read_text())
assert summary['cpu_affinity']=={'before':[requested],'selected':[requested]}, 'actual affinity differs'
assert not summary.get('unavailable_resource_files'), 'required resource observation missing'
assert summary['status']=='native_correctness_and_microbenchmark_completed'
(out/'resource-verification.json').write_text(json.dumps({
 'status':'verified','cpu_quota':quota,'cpu_period':period,
 'memory_max':2147483648,'swap_max':0,'cpu_affinity':[requested],
 'database_runs':0,'root_pgo_builds':0},indent=2)+'\n')
PY
