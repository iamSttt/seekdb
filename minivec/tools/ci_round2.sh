#!/usr/bin/env bash
# Factor comparison against the retained runtime. No score extrapolation.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_PARSE_THREADS
unset MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=5a67f1690c46239cf4340ee40437bab5988c82ca
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-round2-XXXXXX")
test ! -e round2-results
mkdir round2-results "$scratch/baseline"
stage=baseline-archive
dump_reports() {
  result=$?
  set +e
  if [ -f round2-results/stage.txt ]; then stage=$(cat round2-results/stage.txt); fi
  printf '{"exit_code":%s,"stage":"%s"}\n' "$result" "$stage" > round2-results/run-status.json
  for file in round2-results/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_ROUND2_REPORT_BEGIN $file"
    cat "$file"
    echo "MINIVEC_ROUND2_REPORT_END"
  done
  exit "$result"
}
trap dump_reports EXIT
git archive "$baseline" | tar -x -C "$scratch/baseline"
python3 - <<'PY'
from pathlib import Path
for name in ('MVDB_DISABLE_RESIDUAL_BOUND','MVDB_DISABLE_BORROWED_TOKENS','MVDB_KEEP_INSERT_TEXT'):
    if not any(name in path.read_text() for path in Path('minivec/src').glob('*') if path.suffix in ('.h','.cpp')):
        raise SystemExit('missing required ablation control: '+name)
PY

for side in baseline parser_only residual_only combined; do
  stage="$side-build"
  source="$repo"
  flags=()
  case "$side" in
    baseline) source="$scratch/baseline" ;;
    parser_only) flags=(-DMVDB_DISABLE_RESIDUAL_BOUND) ;;
    residual_only) flags=(-DMVDB_DISABLE_BORROWED_TOKENS -DMVDB_KEEP_INSERT_TEXT) ;;
  esac
  dockerfile="$scratch/Dockerfile.$side"
  python3 - "$source/Dockerfile" "$dockerfile" "${flags[@]}" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
needle = 'g++ -O3 -std=c++17'
if text.count(needle) != 1:
    raise SystemExit('unexpected compiler command; refusing ambiguous variant flags')
replacement = 'g++ -O3 ' + (' '.join(sys.argv[3:]) + ' ' if sys.argv[3:] else '') + '-std=c++17'
Path(sys.argv[2]).write_text(text.replace(needle, replacement))
PY
  docker build --platform linux/amd64 -f "$dockerfile" -t "minivec:round2-$side" "$source"
  container=$(docker create "minivec:round2-$side")
  docker cp "$container:/minivec" "$scratch/$side.bin"
  docker rm "$container"
  sha256sum "$scratch/$side.bin" | tee "round2-results/$side-sha256.txt"
done

# Validate the two compile-time ablations as real programs before measuring.
stage=ablation-correctness
for side in parser_only residual_only; do
  python3 minivec/tests/json_protocol.py --binary "$scratch/$side.bin" --timeout 15
  python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
done

# The diagnostic uses the same current harness for every source revision. The
# release Dockerfile/context remains untouched, including its strict .dockerignore.
printf '%s' profile-build > round2-results/stage.txt
compiler_image=$(python3 - <<'PY'
from pathlib import Path
line = Path('Dockerfile').read_text().splitlines()[1].split()
if len(line) != 4 or line[0] != 'FROM' or line[2:] != ['AS','build'] or '@sha256:' not in line[1]:
    raise SystemExit('unexpected pinned compiler image declaration')
print(line[1])
PY
)
for side in baseline parser_only residual_only combined; do
  printf '%s' "profile-build-$side" > round2-results/stage.txt
  source="$repo"
  flags=()
  case "$side" in
    baseline) source="$scratch/baseline" ;;
    parser_only) flags=(-DMVDB_DISABLE_RESIDUAL_BOUND) ;;
    residual_only) flags=(-DMVDB_DISABLE_BORROWED_TOKENS -DMVDB_KEEP_INSERT_TEXT) ;;
  esac
  docker run --rm --network none --platform linux/amd64 \
    --mount "type=bind,source=$source,target=/src,readonly" \
    --mount "type=bind,source=$repo/minivec/tests/profile_query.cpp,target=/profile_query.cpp,readonly" \
    --mount "type=bind,source=$scratch,target=/out" \
    --workdir /src/minivec --entrypoint g++ "$compiler_image" \
    -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -static -pthread -Isrc -Ithird_party "${flags[@]}" /profile_query.cpp \
    src/db.cpp src/exec.cpp src/sql.cpp src/value.cpp src/flat_index.cpp -o "/out/$side.profile"
  "$scratch/$side.profile" --help > "round2-results/profile-$side-help.txt"
done

export ROUND2_SCRATCH="$scratch" ROUND2_BASELINE="$baseline"
stage=paired-benchmark
printf '%s' paired-benchmark > round2-results/stage.txt
python3 - <<'PY'
import hashlib, json, os, platform, subprocess
from pathlib import Path
scratch = Path(os.environ['ROUND2_SCRATCH'])
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('round2 comparison requires two available CPUs')
os.sched_setaffinity(0, cpus)
variants = {
    'baseline': [],
    'parser_only': ['MVDB_DISABLE_RESIDUAL_BOUND'],
    'residual_only': ['MVDB_DISABLE_BORROWED_TOKENS', 'MVDB_KEEP_INSERT_TEXT'],
    'combined': [],
}
context = {
    'baseline_commit': os.environ['ROUND2_BASELINE'],
    'candidate_commit': subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip(),
    'tracked_worktree_status': subprocess.check_output(['git','status','--porcelain','--untracked-files=no'], text=True),
    'platform': platform.platform(), 'cpu_affinity': cpus,
    'cpu_info': subprocess.check_output(['lscpu'], text=True),
    'compiler': 'Each root Dockerfile: pinned GCC 13.3.0 static -O3; no LTO. Only listed preprocessor flags differ.',
    'variants': {name: {'preprocessor_defines': flags,
        'binary_sha256': hashlib.sha256((scratch/(name+'.bin')).read_bytes()).hexdigest()}
        for name, flags in variants.items()},
    'standard': {'seeds': [7319,8911,13337], 'pairs_per_seed': 5, 'queries_per_phase': 1000,
                 'pairs_per_variant': 15, 'total_pairs': 45},
    'boundaries': {'seeds': [7319,8911,13337], 'pairs_per_seed': 5, 'queries_per_phase': 64,
                   'scenarios': ['resident30k','resident35k','resident50k','low16','filter60k','update10','spill'],
                   'total_pairs': 105},
    'limitations': 'Same-runner comparisons with process RSS/I/O proxies, not official E/M/B. '
      'Each variant is paired against baseline; blocks are sequential and not direct variant-to-variant pairs. '
      'Observed stderr policy messages do not prove every query used the advertised thread count. '
      'Post-UPDATE first query includes lazy rebuild; restart timer includes open-time index rebuild.',
}
Path('round2-results/context.json').write_text(json.dumps(context, indent=2)+'\n')
common = ['python3','minivec/tools/benchmark_iteration.py','--suite','full',
          '--baseline',str(scratch/'baseline.bin'), '--seeds','7319','8911','13337','--pairs','5']
for name in ('parser_only','residual_only','combined'):
    Path('round2-results/stage.txt').write_text('standard-'+name)
    subprocess.run(common+['--candidate',str(scratch/(name+'.bin')),
        '--scenario','standard10k','--queries','1000','--output','round2-results/standard-'+name+'.json'], check=True)
Path('round2-results/stage.txt').write_text('combined-boundaries')
scenarios = [argument for name in context['boundaries']['scenarios'] for argument in ('--scenario',name)]
subprocess.run(common+['--candidate',str(scratch/'combined.bin'),'--queries','64',
    '--output','round2-results/combined-boundaries.json']+scenarios, check=True)
PY

python3 - <<'PY'
import hashlib, json, os, subprocess
from pathlib import Path
scratch = Path(os.environ['ROUND2_SCRATCH'])
cpus = json.loads(Path('round2-results/context.json').read_text())['cpu_affinity']
os.sched_setaffinity(0, cpus)
reports = {'kind': 'round2_in_process_diagnostics', 'runs': [],
    'scope': 'First pass plus ten repeated passes; no independent-seed inflation. '
      'CPU process totals include worker threads. Execute includes JSON encoding. '
      'Response byte equality across variants is checked outside timers; paired benchmark supplies sampled oracles.'}
expected = {}
for seed_index, seed in enumerate((7319,8911,13337)):
    names = ['baseline','parser_only','residual_only','combined']
    names = names[seed_index:] + names[:seed_index]
    for name in names:
        Path('round2-results/stage.txt').write_text('profile-'+name+'-'+str(seed))
        responses = scratch/('profile-'+name+'-'+str(seed)+'.jsonl')
        try:
            run = subprocess.run([str(scratch/(name+'.profile')), '--seed',str(seed),
                '--queries','1000','--repeats','10','--responses',str(responses)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180,
                env={**os.environ, 'TMPDIR': str(scratch)})
            returncode, stdout, stderr, failure = run.returncode, run.stdout, run.stderr, None
        except subprocess.TimeoutExpired as error:
            returncode, stdout, stderr, failure = None, error.stdout or b'', error.stderr or b'', 'timeout_180_seconds'
        record = {'variant': name, 'seed': seed, 'returncode': returncode, 'failure': failure,
            'profile_binary_sha256': hashlib.sha256((scratch/(name+'.profile')).read_bytes()).hexdigest(),
            'stderr': {'total_bytes': len(stderr), 'tail_source_bytes': min(len(stderr),4096),
                       'truncated': len(stderr)>4096, 'tail_utf8': stderr[-4096:].decode('utf-8','replace')}}
        if returncode == 0:
            try:
                record['report'] = json.loads(stdout)
                body = responses.read_bytes()
                record['responses_sha256'] = hashlib.sha256(body).hexdigest()
                record['responses_bytes'] = len(body)
                # The rotated first variant provides an arbitrary reference.
                # Require byte equality, not merely a weak checksum.
                reference = expected.setdefault(seed, body)
                record['responses_equal_to_seed_reference'] = body == reference
            except (ValueError, OSError) as error:
                record['failure'] = str(error)
        if returncode != 0 or record['failure']:
            record['stdout_tail'] = stdout[-4096:].decode('utf-8','replace')
        reports['runs'].append(record)
        Path('round2-results/profiles.json').write_text(json.dumps(reports,indent=2,allow_nan=False)+'\n')
        if returncode != 0 or record['failure'] or not record.get('responses_equal_to_seed_reference',False):
            raise SystemExit('in-process diagnostic failed or response mismatch')
PY
printf '%s' quantized-dot-build > round2-results/stage.txt
docker run --rm --network none --platform linux/amd64 \
  --mount "type=bind,source=$repo,target=/src,readonly" \
  --mount "type=bind,source=$scratch,target=/out" \
  --workdir /src --entrypoint g++ "$compiler_image" \
  -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  minivec/tests/profile_quantized_dot.cpp -o /out/quantized-dot
printf '%s' quantized-dot-run > round2-results/stage.txt
python3 - <<'PY'
import hashlib, json, os, subprocess
from pathlib import Path
scratch = Path(os.environ['ROUND2_SCRATCH'])
cpus = json.loads(Path('round2-results/context.json').read_text())['cpu_affinity']
os.sched_setaffinity(0, cpus)
binary = scratch/'quantized-dot'
try:
    run = subprocess.run([str(binary)], capture_output=True, timeout=180)
    code, stdout, stderr = run.returncode, run.stdout, run.stderr
except subprocess.TimeoutExpired as error:
    code, stdout, stderr = None, error.stdout or b'', error.stderr or b''
try:
    report = json.loads(stdout)
    if type(report) is not dict:
        raise ValueError('diagnostic output is not an object')
except ValueError as error:
    report = {'status': 'failed', 'error': str(error), 'stdout_tail': stdout[-4096:].decode('utf-8','replace')}
report['execution'] = {'returncode': code, 'timeout_seconds': 180, 'cpu_affinity': cpus,
    'compiler': 'Pinned GCC 13.3.0, static -O3 -std=c++17 -fno-math-errno -pthread',
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'stderr_tail': stderr[-4096:].decode('utf-8','replace'), 'stderr_total_bytes': len(stderr)}
Path('round2-results/quantized-dot.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
if code != 0 or report.get('status') not in ('passed','skipped_no_avx2'):
    raise SystemExit('standalone quantized-dot diagnostic failed')
Path('round2-results/stage.txt').write_text('complete')
PY
stage=complete
