#!/usr/bin/env bash
# Force the quantized dispatcher to AVX2 for all three comparison variants.
# Timing, process memory and process IO are local evidence, never official E/M/B.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_PARSE_THREADS
unset MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=5a67f1690c46239cf4340ee40437bab5988c82ca
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-dot-avx2-XXXXXX")
test ! -e dot-avx2-results
mkdir dot-avx2-results "$scratch/baseline"
stage=baseline-archive
dump_reports() {
  result=$?
  set +e
  if [ -f dot-avx2-results/stage.txt ]; then stage=$(cat dot-avx2-results/stage.txt); fi
  printf '{"exit_code":%s,"stage":"%s"}\n' "$result" "$stage" > dot-avx2-results/run-status.json
  for file in dot-avx2-results/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_DOT_AVX2_REPORT_BEGIN $file"
    cat "$file"
    echo "MINIVEC_DOT_AVX2_REPORT_END"
  done
  exit "$result"
}
trap dump_reports EXIT
git archive "$baseline" | tar -x -C "$scratch/baseline"
stage=ablation-preflight
python3 - "$scratch/baseline" <<'PY'
from pathlib import Path
import sys
name = 'MVDB_DISABLE_QUANTIZED_DOT'
if not any(name in path.read_text() for path in Path('minivec/src').glob('*') if path.suffix in ('.cpp','.h')):
    raise SystemExit('missing required ablation control: '+name)
for source in (Path('.'),Path(sys.argv[1])):
    if 'MVDB_DISABLE_AVX512_QUANTIZED' not in (source/'minivec/src/quantized_bound.h').read_text():
        raise SystemExit('source lacks AVX512 dispatcher control: '+str(source))
PY

# This small program verifies the compiled dispatcher on the actual runner.
# It establishes kernel eligibility, not a trace of every benchmark query.
cat > "$scratch/kernel_probe.cpp" <<'CPP'
#include "quantized_bound.h"
#include <iostream>
int main() {
  bool avx2=false,dot=false;
#if defined(MVDB_SIMD_BOUND_X86)
  avx2=mvdb::quantized_detail::choose_distance()==mvdb::quantized_detail::distance_avx2;
#endif
#ifdef MVDB_DIAG_DOT_API
  dot=mvdb::quantized_dot_available();
#endif
  std::cout<<"{\"selected_quantized_kernel\":\""<<(avx2?"avx2":"unexpected")
    <<"\",\"dot_eligible\":"<<(dot?"true":"false")<<"}\n";
  if(!avx2)return 3;
#if defined(MVDB_DIAG_DOT_API) && !defined(MVDB_DISABLE_QUANTIZED_DOT)
  if(!dot)return 4;
#else
  if(dot)return 5;
#endif
}
CPP
compiler_image=gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818

for side in baseline without_dot dot; do
  stage="$side-build"
  source="$repo"
  flags=(-DMVDB_DISABLE_AVX512_QUANTIZED)
  if [ "$side" = baseline ]; then source="$scratch/baseline"; fi
  if [ "$side" = without_dot ]; then flags+=(-DMVDB_DISABLE_QUANTIZED_DOT); fi
  dockerfile="$scratch/Dockerfile.$side"
  python3 - "$source/Dockerfile" "$dockerfile" "${flags[@]}" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
pin = 'FROM gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 AS build'
if pin not in text.splitlines():
    raise SystemExit('unexpected compiler image; pinned GCC 13.3.0 is required')
needle = 'g++ -O3 -std=c++17'
if text.count(needle) != 1:
    raise SystemExit('unexpected compiler command; refusing ambiguous ablation')
replacement = 'g++ -O3 ' + (' '.join(sys.argv[3:])+' ' if sys.argv[3:] else '') + '-std=c++17'
Path(sys.argv[2]).write_text(text.replace(needle,replacement))
PY
  docker build --platform linux/amd64 -f "$dockerfile" -t "minivec:dot-avx2-$side" "$source"
  container=$(docker create "minivec:dot-avx2-$side")
  docker cp "$container:/minivec" "$scratch/$side.bin"
  docker rm "$container"
  sha256sum "$scratch/$side.bin" | tee "dot-avx2-results/$side-sha256.txt"
  stage="$side-dispatch-probe"
  probe_flags=()
  if [ "$side" != baseline ]; then probe_flags=(-DMVDB_DIAG_DOT_API); fi
  docker run --rm --network none --platform linux/amd64 \
    --mount "type=bind,source=$source,target=/src,readonly" \
    --mount "type=bind,source=$scratch,target=/out" \
    --workdir /src/minivec --entrypoint g++ "$compiler_image" \
    -O3 -std=c++17 -fno-math-errno -static -pthread -Isrc "${flags[@]}" "${probe_flags[@]}" \
    /out/kernel_probe.cpp -o "/out/$side.kernel-probe"
  "$scratch/$side.kernel-probe" | tee "$scratch/kernel-$side.out"
  cp "$scratch/kernel-$side.out" "dot-avx2-results/kernel-$side.json"
done

export DOT_AVX2_SCRATCH="$scratch" DOT_AVX2_BASELINE="$baseline"
stage=correctness-and-paired-benchmark
python3 - <<'PY'
import hashlib, json, os, platform, subprocess, sys, time
from pathlib import Path

scratch = Path(os.environ['DOT_AVX2_SCRATCH'])
out = Path('dot-avx2-results')
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('dot comparison requires two available CPUs')
os.sched_setaffinity(0,cpus)
variants = {'baseline': ['MVDB_DISABLE_AVX512_QUANTIZED'], 'without_dot': ['MVDB_DISABLE_AVX512_QUANTIZED','MVDB_DISABLE_QUANTIZED_DOT'], 'dot': ['MVDB_DISABLE_AVX512_QUANTIZED']}
context = {
    'baseline_commit': os.environ['DOT_AVX2_BASELINE'],
    'candidate_commit': subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    'tracked_worktree_status': subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],text=True),
    'platform': platform.platform(), 'cpu_affinity': cpus,
    'cpu_info': subprocess.check_output(['lscpu'],text=True),
    'compiler': 'Pinned GCC13.3 static O3; all variants disable AVX512 quantized dispatch; without_dot also disables dot. No LTO/PGO.',
    'dispatch_scope': 'Only the integer quantized dispatcher is forced to AVX2. Floating-point bound dispatch remains native and identical across variants; this does not emulate an AVX2-only CPU.',
    'dispatch_probes': {name: json.loads((out/('kernel-'+name+'.json')).read_text()) for name in variants},
    'dispatch_probe_scope': 'Same headers/compiler/defines verify actual quantized selector and dot eligibility on this CPU. Probes do not trace allocation or every query path.',
    'variants': {name: {'preprocessor_defines': flags,
        'dockerfile_sha256': hashlib.sha256((scratch/('Dockerfile.'+name)).read_bytes()).hexdigest(),
        'compiler_command': next(line for line in (scratch/('Dockerfile.'+name)).read_text().splitlines() if line.startswith('RUN g++ ')),
        'binary_sha256': hashlib.sha256((scratch/(name+'.bin')).read_bytes()).hexdigest()}
        for name,flags in variants.items()},
    'standard': {'seeds': [7319,8911,13337], 'pairs_per_seed': 5, 'queries_per_phase': 1000,
        'comparisons': [['baseline','dot'],['without_dot','dot']], 'total_pairs': 30},
    'boundaries': {'seeds': [7319,8911,13337], 'pairs_per_seed': 5, 'queries_per_phase': 64,
        'scenarios': ['resident30k','resident35k','resident50k','low16','filter60k','update10','spill'],
        'comparison': ['baseline','dot'], 'total_pairs': 105},
    'limitations': 'Same-runner shared-cache comparisons with process RSS/IO proxies, not official E/M/B. '
      'Comparison blocks are sequential; each block alternates baseline/candidate order within its own pairs. '
      'Raw benchmark reports preserve every timing sample, resource snapshot and bounded stderr capture with truncation metadata. '
      'Policy stderr is not proof of thread execution. Restart includes eager index rebuild before ready; '
      'the first post-UPDATE query includes lazy index rebuild. No outlier deletion or official score extrapolation.'
}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')

checks = {'status':'running', 'checks': [],
    'scope':'All three release binaries: JSONL protocol plus TopK forced serial and forced two threads; correctness timings are not performance claims.'}
def as_text(value):
    return value.decode('utf-8','replace') if isinstance(value,bytes) else value or ''
for side in variants:
    binary = str(scratch/(side+'.bin'))
    tests = [
        ('json_protocol',['python3','minivec/tests/json_protocol.py','--binary',binary,'--timeout','15'],{}),
        ('topk_serial',['python3','minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'1'}),
        ('topk_two_threads',['python3','minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'2'}),
    ]
    for label,command,environment in tests:
        stage = 'correctness-'+side+'-'+label
        (out/'stage.txt').write_text(stage)
        started = time.monotonic()
        try:
            run = subprocess.run(command,capture_output=True,text=True,timeout=240,env={**os.environ,**environment})
            code,stdout,stderr,failure = run.returncode,run.stdout,run.stderr,None
        except subprocess.TimeoutExpired as error:
            code,stdout,stderr,failure = None,as_text(error.stdout),as_text(error.stderr),'timeout_240_seconds'
        (out/(stage+'.stdout.log')).write_text(stdout)
        (out/(stage+'.stderr.log')).write_text(stderr)
        print(stage,flush=True)
        if stdout: print(stdout,end='' if stdout.endswith('\n') else '\n',flush=True)
        if stderr: print(stderr,end='' if stderr.endswith('\n') else '\n',file=sys.stderr,flush=True)
        checks['checks'].append({'variant':side,'test':label,'returncode':code,'failure':failure,
            'elapsed_seconds':time.monotonic()-started,'environment_overrides':environment,
            'stdout_sha256':hashlib.sha256(stdout.encode()).hexdigest(),
            'stderr_sha256':hashlib.sha256(stderr.encode()).hexdigest(),
            'stdout_tail':stdout[-4096:],'stderr_tail':stderr[-4096:]})
        if code != 0: checks['status']='failed'
        (out/'correctness.json').write_text(json.dumps(checks,indent=2,allow_nan=False)+'\n')
        if code != 0: raise SystemExit('correctness gate failed: '+stage)
checks['status']='passed'
(out/'correctness.json').write_text(json.dumps(checks,indent=2,allow_nan=False)+'\n')

common = ['python3','minivec/tools/benchmark_iteration.py','--suite','full',
          '--seeds','7319','8911','13337','--pairs','5']
for before,name in [('baseline','standard-baseline-dot'),('without_dot','standard-ablation-dot')]:
    (out/'stage.txt').write_text(name)
    subprocess.run(common+['--baseline',str(scratch/(before+'.bin')),'--candidate',str(scratch/'dot.bin'),
        '--scenario','standard10k','--queries','1000','--output',str(out/(name+'.json'))],check=True)
(out/'stage.txt').write_text('baseline-dot-boundaries')
scenarios = [argument for name in context['boundaries']['scenarios'] for argument in ('--scenario',name)]
subprocess.run(common+['--baseline',str(scratch/'baseline.bin'),'--candidate',str(scratch/'dot.bin'),
    '--queries','64','--output',str(out/'baseline-dot-boundaries.json')]+scenarios,check=True)
(out/'stage.txt').write_text('complete')
PY
stage=complete
