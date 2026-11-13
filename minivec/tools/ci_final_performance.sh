#!/usr/bin/env bash
# Called from ci_score after candidate gates and before its final summary.
# Usage: bash SCRIPT /absolute/already-validated-binary sha256:IMAGE_ID
# Equivalent inputs: MVDB_FINAL_BINARY and MVDB_FINAL_IMAGE_ID.
# This helper never compiles or replaces the candidate binary.
set -euo pipefail
candidate_input=${1:-${MVDB_FINAL_BINARY:-}}
candidate_image=${2:-${MVDB_FINAL_IMAGE_ID:-}}
repo=$(git rev-parse --show-toplevel)
cd "$repo"
test -d protocol-results
out="$repo/protocol-results/final-performance"
test ! -e "$out"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-final-performance-XXXXXX")
baseline=969ecca98f817ecdef9b0c378fc99a0d6cef510d
container=
export FINAL_CANDIDATE_INPUT="$candidate_input" FINAL_CANDIDATE_IMAGE="$candidate_image"
export FINAL_BASELINE_COMMIT="$baseline" FINAL_OUT="$out" FINAL_SCRATCH="$scratch"
stage() { printf '%s\n' "$1" > "$out/stage.txt"; }
finish() {
  result=$?
  trap - EXIT
  set +e
  if [ -n "$container" ]; then docker rm -v "$container" >/dev/null 2>&1; fi
  python3 - "$result" <<'PYCOLLECT'
import hashlib, json, os, re, sys
from pathlib import Path
out = Path(os.environ['FINAL_OUT'])
exit_code = int(sys.argv[1])
def read(name):
    try:
        value = json.loads((out / name).read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}
def digest(path):
    try:
        with Path(path).open('rb') as file:
            result = hashlib.sha256()
            for block in iter(lambda: file.read(1024 * 1024), b''):
                result.update(block)
        return result.hexdigest()
    except (OSError, TypeError):
        return None
def write(name, value):
    (out / name).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
context = read('context.json')
after = {side: digest(context.get(side, {}).get('binary')) for side in ('baseline', 'candidate')}
unchanged = {side: after[side] is not None and after[side] == context.get(side, {}).get('binary_sha256_before')
             for side in ('baseline', 'candidate')}
checks = read('baseline-correctness.json')
gate_ok = checks.get('status') == 'passed' and len(checks.get('checks', [])) == 3 and all(
    check.get('returncode') == 0 for check in checks.get('checks', [])) and {
    check.get('test') for check in checks.get('checks', [])} == {'json_protocol', 'topk_serial', 'topk_two_threads'}
expected = {
    'standard': (15, {'standard10k'}, 5, False),
    'boundaries': (90, {'resident30k', 'resident50k', 'low16', 'filter60k', 'update10', 'spill'}, 5, False),
    # Existing matrix names: compound is the low-hit filter, high is all-hit.
    'spill_filters': (6, {'spill_compound', 'spill_high'}, 1, False),
    'projections': (9, {'wide768', 'many16', 'wide128'}, 1, True)}
projection_phases = {'narrow_top10', 'wide_top10', 'narrow_large4096', 'wide_large4096',
                     'narrow_all', 'wide_all', 'wide_distance10'}
def sha_valid(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None
def responses_valid(pair, projection):
    if not isinstance(pair, dict):
        return False
    sides = [pair.get(side, {}) for side in ('baseline', 'candidate')]
    if not all(isinstance(side, dict) for side in sides):
        return False
    if not projection:
        a, b = [side.get('response_sha256') for side in sides]
        return pair.get('all_responses_identical') is True and sha_valid(a) and a == b
    if pair.get('all_responses_identical_and_independently_verified') is not True:
        return False
    if not all(side.get('status') == 'passed' and isinstance(side.get('phases'), dict)
               and set(side['phases']) == projection_phases for side in sides):
        return False
    for phase in projection_phases:
        records = [side['phases'][phase] for side in sides]
        if not all(isinstance(item, dict) and item.get('independent_oracle')
                   and isinstance(item.get('requests'), list) and len(item['requests']) == 3 for item in records):
            return False
        for left, right in zip(records[0]['requests'], records[1]['requests']):
            if not isinstance(left, dict) or not isinstance(right, dict):
                return False
            a, b = left.get('response_sha256'), right.get('response_sha256')
            if not sha_valid(a) or a != b or left.get('response_bytes', 0) <= 0 or left.get('response_bytes') != right.get('response_bytes'):
                return False
    return True
benchmarks = {}
for name, (count, scenarios, pairs_per_seed, projection) in expected.items():
    report = read(name + '.json')
    pairs = report.get('pairs', [])
    expected_ids = {(scenario, seed, index) for scenario in scenarios
                    for seed in (7319, 8911, 13337) for index in range(pairs_per_seed)}
    shaped = isinstance(pairs, list) and all(isinstance(pair, dict)
        and isinstance(pair.get('scenario'), str) and type(pair.get('seed')) is int
        and type(pair.get('pair_index')) is int for pair in pairs)
    identities = [(pair['scenario'], pair['seed'], pair['pair_index']) for pair in pairs] if shaped else []
    complete_ids = shaped and len(identities) == count and len(set(identities)) == count and set(identities) == expected_ids
    binaries = report.get('binaries', {})
    bound = isinstance(binaries, dict) and all(isinstance(binaries.get(side), dict)
        and sha_valid(binaries[side].get('sha256'))
        and binaries[side]['sha256'] == context.get(side, {}).get('binary_sha256_before')
        for side in ('baseline', 'candidate'))
    responses = shaped and all(responses_valid(pair, projection) for pair in pairs)
    valid = report.get('status') == 'passed' and complete_ids and bound and responses
    benchmarks[name] = {'status': report.get('status'), 'pairs': len(pairs) if isinstance(pairs, list) else None,
        'expected_pairs': count, 'complete': valid, 'unique_expected_pair_identities': complete_ids,
        'measured_binary_sha256_matches': bound, 'all_responses_verified': responses,
        'resource_scope': ('wall time and response bytes; no CPU/RSS/IO measurements'
                           if projection else 'process snapshots/proxies; not cgroup or official scores'),
        'summary': report.get('summary')}
total_pairs = sum(item['pairs'] or 0 for item in benchmarks.values())
verified_total_pairs = sum(item['expected_pairs'] for item in benchmarks.values() if item['complete'])
passed = (exit_code == 0 and context.get('candidate_image_binary_binding_verified') is True and gate_ok and
          all(unchanged.values()) and all(item['complete'] for item in benchmarks.values())
          and total_pairs == verified_total_pairs == 120)
stage_name = (out / 'stage.txt').read_text().strip() if (out / 'stage.txt').is_file() else 'setup'
summary = {'status': 'passed' if passed else 'incomplete_or_failed', 'exit_code': exit_code, 'stage': stage_name,
    'scope': 'same already-validated candidate binary; local paired timing, not formal score or E/M/B',
    'candidate_image_id': context.get('candidate', {}).get('image_id'),
    'binary_sha256_before': {side: context.get(side, {}).get('binary_sha256_before') for side in ('baseline', 'candidate')},
    'binary_sha256_after': after, 'binary_unchanged': unchanged, 'baseline_three_correctness_gates_passed': gate_ok,
    'benchmarks': benchmarks, 'total_pairs': total_pairs, 'expected_total_pairs': 120,
    'verified_total_pairs': verified_total_pairs, 'baseline_commit': context.get('baseline_commit'),
    'adoption_decision': 'not inferred by this script',
    'limitations': ['Candidate gates were executed earlier by ci_score; this helper adds three baseline gates.',
        'PGO profile counters and future rebuild hashes may differ; these results bind only this image and binary.',
        'CPU affinity is fixed to two allowed CPUs; shared-runner cache/scheduling effects remain.',
        'Restart includes eager index rebuilding; the first post-UPDATE query includes lazy rebuilding.',
        'Spill filters and wide projections have three pairs each scenario: screening, not adoption proof.',
        'wide128 screens the guard activation boundary with 1024 rows; it does not prove every dimension >=128 is faster.',
        'Projection reports have no CPU/RSS/IO measurements; response bytes are not physical I/O.',
        'Every paired sample is retained. No official score extrapolation or outlier deletion.']}
write('summary.json', summary)
write('run-status.json', {'exit_code': exit_code, 'stage': stage_name, 'proof_status': summary['status']})
artifacts = []
for path in sorted(out.rglob('*')):
    if path.is_file() and path.name != 'hashes.json':
        artifacts.append({'path': path.relative_to(out).as_posix(), 'bytes': path.stat().st_size,
                          'sha256': digest(path)})
write('hashes.json', {'artifacts': artifacts, 'note': 'The manifest excludes itself.'})
if exit_code == 0 and not passed:
    raise SystemExit('Final performance proof is incomplete despite zero process exit code')
PYCOLLECT
  proof_result=$?
  if [ "$result" -eq 0 ] && [ "$proof_result" -ne 0 ]; then result=$proof_result; fi
  exit "$result"
}
trap finish EXIT

stage preflight
# The outer ci_score EXIT trap prints every nested JSON/TXT report on success
# or failure. Raw logs remain in this directory for artifact upload.
python3 - <<'PYPREFLIGHT'
import hashlib, json, os, subprocess
from pathlib import Path
out = Path(os.environ['FINAL_OUT'])
candidate = Path(os.environ['FINAL_CANDIDATE_INPUT'])
if not candidate.is_absolute() or not candidate.is_file() or not os.access(candidate, os.X_OK):
    raise SystemExit('candidate must be an existing executable absolute path')
candidate = candidate.resolve(strict=True)
image_id = os.environ['FINAL_CANDIDATE_IMAGE']
if not image_id.startswith('sha256:') or len(image_id) != 71:
    raise SystemExit('candidate image must be the immutable sha256 image ID')
parent = out.parent
sha = hashlib.sha256(candidate.read_bytes()).hexdigest()
if (parent / 'image-id.txt').read_text().strip() != image_id:
    raise SystemExit('candidate image differs from the ci_score verified image')
if (parent / 'image-binary-sha256.txt').read_text().split()[0] != sha:
    raise SystemExit('candidate bytes differ from the ci_score verified binary')
head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
if (parent / 'runtime-commit.txt').read_text().strip() != head:
    raise SystemExit('checkout changed after candidate verification')
pin = os.environ['FINAL_BASELINE_COMMIT']
if subprocess.check_output(['git', 'rev-parse', pin + '^{commit}'], text=True).strip() != pin:
    raise SystemExit('fixed baseline commit is unavailable; checkout requires fetch-depth: 0')
(out / 'candidate-path.txt').write_text(str(candidate) + '\n')
(out / 'context.json').write_text(json.dumps({'candidate_commit': head,
    'baseline_commit': pin, 'github_run_id': os.environ.get('GITHUB_RUN_ID'),
    'github_run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'),
    'candidate': {'binary': str(candidate), 'image_id': image_id, 'binary_sha256_before': sha},
    'candidate_correctness_source': 'Earlier ci_score image, extracted-binary, source and sanitizer gates; no candidate rebuild here.',
    'candidate_image_binary_binding_verified': False}, indent=2) + '\n')
PYPREFLIGHT
candidate=$(cat "$out/candidate-path.txt")
test "$(docker image inspect "$candidate_image" --format '{{.Id}}')" = "$candidate_image"
container=$(docker create "$candidate_image")
docker cp "$container:/minivec" "$scratch/candidate-image.bin"
docker rm -v "$container"
container=
cmp "$candidate" "$scratch/candidate-image.bin"

stage baseline-archive
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party minivec/tools/pgo | tar -x -C "$scratch/baseline"
python3 - "$scratch/baseline/Dockerfile" <<'PYPIN'
from pathlib import Path
import hashlib, json, os, sys
root = Path(sys.argv[1]).parent
dockerfile = Path(sys.argv[1]).read_text()
pin = 'FROM gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 AS build'
if pin not in dockerfile.splitlines() or 'ARG ENABLE_PGO=1' not in dockerfile.splitlines():
    raise SystemExit('retained baseline must use pinned GCC13.3 and original default PGO')
required = ['Dockerfile', '.dockerignore', 'minivec/tools/pgo/train.cpp', 'minivec/tools/pgo/run-training.sh']
if not all((root / name).is_file() for name in required):
    raise SystemExit('baseline archive is missing original PGO build inputs')
for line in dockerfile.splitlines():
    fields = line.split()
    if fields and fields[0] == 'COPY' and not fields[1].startswith('--from='):
        if len(fields) != 3 or not (root / fields[1]).exists():
            raise SystemExit('baseline Docker COPY input is unavailable: ' + line)
files = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
         for path in sorted(root.rglob('*')) if path.is_file()}
(Path(os.environ['FINAL_OUT']) / 'baseline-inputs.json').write_text(json.dumps({
    'source_commit': os.environ['FINAL_BASELINE_COMMIT'], 'unmodified_root_dockerfile': True,
    'enable_pgo': 1, 'archived_files_sha256': files}, indent=2) + '\n')

PYPIN
stage baseline-build
baseline_tag="minivec:final-baseline-$(basename "$scratch")"
docker build --network none --platform linux/amd64 --progress=plain --build-arg ENABLE_PGO=1 -t "$baseline_tag" "$scratch/baseline" \
  2>&1 | tee "$out/baseline-build.log"
docker image inspect "$baseline_tag" > "$out/baseline-image.json"
container=$(docker create "$baseline_tag")
docker cp "$container:/minivec" "$scratch/baseline.bin"
docker rm -v "$container"
container=
sha256sum "$scratch/baseline.bin" > "$out/baseline-binary-sha256.txt"

unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
stage correctness-and-benchmark
python3 - <<'PYBENCHMARK'
import hashlib, json, os, platform, subprocess, sys, time
from pathlib import Path
out, scratch = Path(os.environ['FINAL_OUT']), Path(os.environ['FINAL_SCRATCH'])
context = json.loads((out / 'context.json').read_text())
candidate, baseline = Path(context['candidate']['binary']), scratch / 'baseline.bin'
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('Final performance requires two available CPUs')
os.sched_setaffinity(0, cpus)
context.update({'cpu_affinity': cpus, 'platform': platform.platform(),
    'cpu_info': subprocess.check_output(['lscpu'], text=True),
    'candidate_image_binary_binding_verified': True,
    'baseline': {'binary': str(baseline), 'binary_sha256_before': hashlib.sha256(baseline.read_bytes()).hexdigest(),
        'image_id': json.loads((out / 'baseline-image.json').read_text())[0]['Id'],
        'dockerfile_sha256': hashlib.sha256((scratch / 'baseline/Dockerfile').read_bytes()).hexdigest(),
        'compiler': 'Unmodified retained 969ecca root Dockerfile, pinned GCC13.3 static O3 plus original oracle-checked PGO',
        'build_inputs': 'baseline-inputs.json', 'enable_pgo': 1},
    'standard': {'seeds': [7319, 8911, 13337], 'pairs_per_seed': 5, 'queries_argument': 1000, 'pairs': 15},
    'boundaries': {'seeds': [7319, 8911, 13337], 'pairs_per_seed': 5, 'queries_argument': 64, 'pairs': 90,
        'scenarios': ['resident30k', 'resident50k', 'low16', 'filter60k', 'update10', 'spill']},
    'spill_filters': {'seeds': [7319, 8911, 13337], 'pairs_per_seed': 1, 'queries_argument': 64, 'pairs': 6,
        'scenarios': ['spill_compound', 'spill_high'], 'meaning': 'low-hit compound / all-hit; 5000x128d, 8KiB padding'},
    'projections': {'seeds': [7319, 8911, 13337], 'pairs_per_seed': 1, 'repetitions_per_phase': 3, 'pairs': 9,
        'scenarios': ['wide768', 'many16', 'wide128'], 'resources': 'wall time and response bytes only; no CPU/RSS/IO',
        'wide128_scope': '1024 rows, 128-dimensional projected vector, 96-codepoint string; explicit boundary screen'},
    'expected_total_pairs': 120,
    'benchmark_harness_sha256': {name: hashlib.sha256(Path('minivec/tools', name).read_bytes()).hexdigest()
        for name in ['ci_final_performance.sh', 'benchmark_iteration.py', 'benchmark_report_matrix.py', 'benchmark_projection.py']}})
(out / 'context.json').write_text(json.dumps(context, indent=2) + '\n')
def write(name, value):
    (out / name).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
def as_text(value):
    return value.decode('utf-8', 'replace') if isinstance(value, bytes) else value or ''
checks = {'status': 'running', 'checks': [], 'scope': 'Three baseline release-binary gates, before timing; candidate already gated by ci_score.'}
write('baseline-correctness.json', checks)
tests = [('json_protocol', [sys.executable, 'minivec/tests/json_protocol.py', '--binary', str(baseline), '--timeout', '15'], {}),
    ('topk_serial', [sys.executable, 'minivec/tests/topk_bounds.py', '--binary', str(baseline)], {'MVDB_QUERY_THREADS': '1'}),
    ('topk_two_threads', [sys.executable, 'minivec/tests/topk_bounds.py', '--binary', str(baseline)], {'MVDB_QUERY_THREADS': '2'})]
for label, command, overrides in tests:
    (out / 'stage.txt').write_text('baseline-gate-' + label)
    started = time.monotonic()
    try:
        run = subprocess.run(command, capture_output=True, timeout=240, env={**os.environ, **overrides})
        code, stdout, stderr, failure = run.returncode, as_text(run.stdout), as_text(run.stderr), None
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr, failure = None, as_text(error.stdout), as_text(error.stderr), 'timeout_240_seconds'
    (out / (label + '.stdout.log')).write_text(stdout)
    (out / (label + '.stderr.log')).write_text(stderr)
    print(label, stdout, stderr, flush=True)
    checks['checks'].append({'test': label, 'returncode': code, 'failure': failure,
        'elapsed_seconds': time.monotonic() - started, 'environment_overrides': overrides,
        'stdout_tail': stdout[-4096:], 'stderr_tail': stderr[-4096:]})
    if code != 0:
        checks['status'] = 'failed'
    write('baseline-correctness.json', checks)
    if code != 0:
        raise SystemExit('Baseline correctness gate failed: ' + label)
checks['status'] = 'passed'
write('baseline-correctness.json', checks)
common = [sys.executable, 'minivec/tools/benchmark_iteration.py', '--suite', 'full',
    '--baseline', str(baseline), '--candidate', str(candidate), '--seeds', '7319', '8911', '13337', '--pairs', '5']
(out / 'stage.txt').write_text('standard-15-pairs')
subprocess.run(common + ['--scenario', 'standard10k', '--queries', '1000', '--output', str(out / 'standard.json')], check=True)
(out / 'stage.txt').write_text('boundaries-90-pairs')
scenarios = [arg for name in context['boundaries']['scenarios'] for arg in ('--scenario', name)]
subprocess.run(common + ['--queries', '64', '--output', str(out / 'boundaries.json')] + scenarios, check=True)
(out / 'stage.txt').write_text('spill-filters-6-pairs')
subprocess.run([sys.executable, 'minivec/tools/benchmark_report_matrix.py',
    '--baseline', str(baseline), '--candidate', str(candidate),
    '--scenario', 'spill_compound', '--scenario', 'spill_high',
    '--seeds', '7319', '8911', '13337', '--pairs', '1', '--queries', '64',
    '--output', str(out / 'spill_filters.json')], check=True)
(out / 'stage.txt').write_text('projections-9-pairs')
subprocess.run([sys.executable, 'minivec/tools/benchmark_projection.py',
    '--baseline', str(baseline), '--candidate', str(candidate),
    '--scenario', 'wide768', '--scenario', 'many16', '--scenario', 'wide128',
    '--seeds', '7319', '8911', '13337', '--pairs', '1', '--repetitions', '3',
    '--output', str(out / 'projections.json')], check=True)
for side, binary in [('baseline', baseline), ('candidate', candidate)]:
    if hashlib.sha256(binary.read_bytes()).hexdigest() != context[side]['binary_sha256_before']:
        raise SystemExit(side + ' binary changed during validation/measurement')
(out / 'stage.txt').write_text('complete')
PYBENCHMARK
