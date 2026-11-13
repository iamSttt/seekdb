#!/usr/bin/env bash
# I17: same-source I05 x I08 factorial, repeat prior special cases, then
# explicit cache-capacity and short-lifetime workloads. Counters are separate.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/filter-report-results"
test ! -e "$out"
mkdir -p "$out/binaries"
stage=build
finish() {
  code=$?;trap - EXIT;set +e
  printf '{"exit_code":%d,"stage":"%s"}\n' "$code" "$stage" > "$out/run-status.json"
  python3 - "$out" <<'PY'
import sys
from pathlib import Path
for path in sorted(Path(sys.argv[1]).glob('*.json')):
    print('MINIVEC_FILTER_REPORT_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_FILTER_REPORT_END')
PY
  exit "$code"
}
trap finish EXIT
test -f minivec/src/filter_diagnostics.h
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
for side in legacy multi_only payload_only default diag_legacy diag_default diag_spill; do
  flags=()
  case "$side" in
    legacy|diag_legacy) flags=(-DMVDB_DISABLE_MULTI_POSTING -DMVDB_DISABLE_PAYLOAD_BATCH);;
    multi_only) flags=(-DMVDB_DISABLE_PAYLOAD_BATCH);;
    payload_only) flags=(-DMVDB_DISABLE_MULTI_POSTING);;
  esac
  [[ "$side" == diag_* ]] && flags+=(-DMVDB_FILTER_DIAGNOSTICS)
  [[ "$side" == diag_spill ]] && flags+=(-DMVDB_ENABLE_SPILL_FILTER)
  stage="build-$side"
  docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$repo,target=/src,readonly" \
    --mount "type=bind,source=$out/binaries,target=/out" --workdir /src/minivec \
    --entrypoint /bin/bash "$compiler" -c \
    'g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc -Ithird_party "${@:2}" src/*.cpp -o "/out/$1.bin"' \
    _ "$side" "${flags[@]}"
done
stage=correctness
for side in legacy multi_only payload_only default; do
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15
  for threads in 1 2; do
    MVDB_QUERY_THREADS="$threads" python3 minivec/tests/topk_bounds.py --binary "$out/binaries/$side.bin"
  done
done
stage=paired-factorial-and-lifecycle
python3 - "$out" <<'PY'
import hashlib,json,math,os,platform,statistics,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);bins=out/'binaries'
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise SystemExit('two CPUs required')
os.sched_setaffinity(0,cpus)
hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
context={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'compiler':'fixed GCC13.3 O3 static, same sources, no PGO',
 'factors':{'legacy':[],'multi_only':['I05'],'payload_only':['I08'],'default':['I05','I08']},
 'affinity':cpus,'platform':platform.platform(),'binaries':hashes,'official_score':None,
 'limits':['Same-source factorial separates flags, not compiler layout from runtime work.',
           'A second independent CI run is still needed; no automatic default adoption.',
           'Standard mixes unfiltered exact and single-filter ann requests by parity.',
           'Diagnostic builds force one worker and cannot determine timing adoption.']}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
def run(harness,side,name,extra,diagnostic=False):
    env=os.environ.copy()
    if diagnostic:env['MVDB_QUERY_THREADS']='1'
    command=['python3','minivec/tools/'+harness,
       '--baseline',str(bins/('diag_legacy.bin' if diagnostic else 'legacy.bin')),
       '--candidate',str(bins/(side+'.bin')),'--output',str(out/(name+'.json'))]+extra
    subprocess.run(command,check=True,env=env)
# Fifteen pairs for EACH contrast: 3 seeds x 5 AB/BA pairs, 1,000 requests
# before restart and 1,000 after. Old-commit layout differences are removed.
for side in ('multi_only','payload_only','default'):
    run('benchmark_iteration.py',side,'standard-'+side,
        ['--suite','full','--scenario','standard10k','--queries','1000','--pairs','5',
         '--seeds','7319','8911','13337'])
# Direct repeat of the original compound/alternate generator, with 15 pairs
# per scenario. Preserve all timings, including the first lazy posting build.
run('benchmark_report_matrix.py','default','compound-alternate-15pairs',
    ['--scenario','compound','--scenario','alternate_columns','--queries','128',
     '--pairs','5','--seeds','7319','8911','13337'])
# Explicit capacity patterns use one, two and three predicate columns.
run('benchmark_filter_patterns.py','default','cache-patterns-15pairs',
    ['--scenario','AAAA','--scenario','ABAB','--scenario','ABCABC','--queries','128',
     '--pairs','5','--seeds','7319','8911','13337'])
# Q=1 is genuinely one SELECT per phase, with no hidden warm-up query.
for queries in (1,4,16):
    run('benchmark_filter_patterns.py','default',f'short-q{queries}',
        ['--queries',str(queries),'--pairs','1','--seeds','7319','8911','13337'])
# Mechanism evidence, separate programs and reports. Full stderr events are
# retained by the pattern harness; process timings here are diagnostic only.
run('benchmark_filter_patterns.py','diag_default','mechanism',
    ['--queries','16','--pairs','1','--seeds','7319','--diagnostic'],diagnostic=True)
mechanism=json.loads((out/'mechanism.json').read_text())
mechanism_checks=[]
for pair in mechanism['pairs']:
    for side in ('baseline','candidate'):
        events=pair[side]['stderr']['filter_diagnostic_events']
        assert events, 'diagnostic executable emitted no counters'
        assert any(sum(e['posting_lookups']) for e in events), 'posting counters missing'
        scans=[e for e in events if e['scan_calls']]
        assert len(scans)==32, 'expected 16 filtered requests in each process lifetime'
        builds=sum(sum(e['posting_builds']) for e in scans)
        hits=sum(sum(e['posting_hits']) for e in scans)
        expected={'AAAA':2,'ABAB':32 if side=='baseline' else 4,'ABCABC':32}
        if pair['scenario'] in expected:
            assert builds==expected[pair['scenario']], 'unexpected cache build count'
            assert builds+hits==32, 'one posting lookup must build or hit per request'
        mechanism_checks.append({'scenario':pair['scenario'],'side':side,'scans':len(scans),
          'posting_builds':builds,'posting_hits':hits,
          'posting_evictions':sum(sum(e['posting_evictions']) for e in scans),
          'vector_visits':sum(e['vector_visits'] for e in scans),
          'row_fetches':sum(e['row_fetches'] for e in scans),
          'exact_distances':sum(e['exact_distances'] for e in scans),
          'predicate_checks':{str(col):sum(e['predicate_checks'][col] for e in scans) for col in (2,3,4)}})
(out/'mechanism-checks.json').write_text(json.dumps({'raw':mechanism_checks},indent=2)+'\n')
spill_env=os.environ.copy();spill_env['MVDB_QUERY_THREADS']='1'
subprocess.run(['python3','minivec/tools/benchmark_spill_counters.py',
 '--baseline',str(bins/'diag_default.bin'),'--candidate',str(bins/'diag_spill.bin'),
 '--output',str(out/'spill-logical-io.json'),'--queries','8','--seed','7319'],
 check=True,env=spill_env)
# Summary is an aid for examining startup amortisation, not a substituted
# benchmark: matched per-pair cold/warm values and all raw samples remain.
amortization=[]
for source in ['cache-patterns-15pairs.json','compound-alternate-15pairs.json']:
    report=json.loads((out/source).read_text())
    for pair in report['pairs']:
        for phase in ('query_before_update','query_after_restart'):
            b=pair['baseline']['query'][phase]['per_request_seconds']
            c=pair['candidate']['query'][phase]['per_request_seconds']
            cold=c[0]-b[0];warm=statistics.mean(c[1:])-statistics.mean(b[1:])
            amortization.append({'source':source,'scenario':pair['scenario'],'seed':pair['seed'],
              'pair_index':pair['pair_index'],'phase':phase,'cold_delta_ms':1000*cold,
              'warm_delta_ms_per_query':1000*warm,
              'predicted_break_even_queries':1 if cold<=0 and warm<=0 else
                 (1+math.ceil(cold/-warm) if cold>0 and warm<0 else None),
              'note':'Estimate from this finite sequence; changing predicates/cache phase can invalidate stationarity.'})
(out/'amortization.json').write_text(json.dumps({'raw':amortization,'official_score':None},indent=2)+'\n')
assert hashes=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
(out/'summary.json').write_text(json.dumps({'status':'passed','standard_pairs':45,
 'compound_alternate_pairs':30,'capacity_pattern_pairs':45,'short_lifetime_pairs':45,
 'mechanism_pairs':5,'binary_unchanged':True,'official_score':None,
 'adoption':'undecided; assess default parity, lifecycle amortisation, and independent repeat'},indent=2)+'\n')
PY
stage=completed
