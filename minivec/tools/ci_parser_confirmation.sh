#!/usr/bin/env bash
# Same-source whole-request confirmation under the original production PGO.
# CONFIRM_FEATURE=parser (default), typed, combined, or spill; no arbitrary flags.
# Optional PARSER_CONFIRM_O3=1 also runs 15 standard pairs without PGO.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
revision=$(git rev-parse HEAD)
export CONFIRM_FEATURE=${CONFIRM_FEATURE:-parser}
export FEATURE_CORRECTNESS_ONLY=${FEATURE_CORRECTNESS_ONLY:-0}
case "$FEATURE_CORRECTNESS_ONLY" in 0|1) ;; *) echo 'FEATURE_CORRECTNESS_ONLY must be 0 or 1' >&2;exit 2;;esac
case "$CONFIRM_FEATURE" in parser|typed|combined|spill) ;; *) echo 'CONFIRM_FEATURE must be parser, typed, combined, or spill' >&2;exit 2;;esac
out="$repo/$CONFIRM_FEATURE-confirmation-results"
test ! -e "$out"
mkdir -p "$out/build-input"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-$CONFIRM_FEATURE-confirm-XXXXXX")
snapshot="$scratch/snapshot"
mkdir "$snapshot"
container=
printf '%s\n' archive > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ -n "$container" ]]; then docker rm -v "$container";fi
  PARSER_CONFIRM_EXIT="$code" PARSER_CONFIRM_OUT="$out" python3 - <<'PY'
import json,os
from pathlib import Path
out=Path(os.environ['PARSER_CONFIRM_OUT'])
status={'exit_code':int(os.environ['PARSER_CONFIRM_EXIT']),
        'stage':(out/'stage.txt').read_text().strip(),
        'feature':os.environ['CONFIRM_FEATURE'],
        'official_score':None,'automatic_adoption':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
for path in sorted(out.glob('*.json')):
    print('MINIVEC_FEATURE_CONFIRM_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_FEATURE_CONFIRM_END')
PY
  local report_code=$?
  if [[ "$report_code" != 0 ]]; then
    echo "Feature confirmation evidence collection failed: exit $report_code" >&2
    if [[ "$code" == 0 ]]; then code=$report_code;fi
    printf '{"exit_code":%d,"report_collection_exit_code":%d,"automatic_adoption":false}\n' \
      "$code" "$report_code" > "$out/run-status.json"
  fi
  exit "$code"
}
trap finish EXIT
git archive "$revision" Dockerfile .dockerignore minivec/src minivec/third_party \
  minivec/tests minivec/tools protocol_check | tar -x -C "$snapshot"
git status --porcelain --untracked-files=no > "$out/worktree-status.txt"
lscpu > "$out/lscpu.txt"
case "${PARSER_CONFIRM_O3:-0}" in 0|1) ;; *) echo 'PARSER_CONFIRM_O3 must be 0 or 1' >&2;exit 2;;esac
export PARSER_CONFIRM_O3=${PARSER_CONFIRM_O3:-0}
export PARSER_CONFIRM_OUT="$out" PARSER_CONFIRM_SNAPSHOT="$snapshot" PARSER_CONFIRM_REVISION="$revision"
python3 - <<'PY'
import hashlib,json,os
from pathlib import Path
out=Path(os.environ['PARSER_CONFIRM_OUT']);snapshot=Path(os.environ['PARSER_CONFIRM_SNAPSHOT'])
production=(snapshot/'Dockerfile').read_text()
needle="flags='-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc'"
if production.count(needle)!=1 or production.count('g++ $flags ')!=3:
    raise SystemExit('production compiler commands changed; review the flag injection')
feature=os.environ['CONFIRM_FEATURE']
flags={'parser':['-DMVDB_ENABLE_TOKEN_REUSE'],
       'typed':['-DMVDB_ENABLE_TYPED_JSON'],
       'combined':['-DMVDB_ENABLE_TOKEN_REUSE','-DMVDB_ENABLE_TYPED_JSON'],
       'spill':['-DMVDB_ENABLE_SPILL_FILTER']}[feature]
replacement=needle[:-1]+' '+' '.join(flags)+"'"
profile_compile='g++ $flags -fprofile-generate=/pgo -fprofile-update=atomic src/*.cpp -o /minivec;'
if production.count(profile_compile)!=1:raise SystemExit('profile generation command changed')
profile_proof=profile_compile+' \\\n         sha256sum /minivec > /training/instrumented-binary-sha256.txt;'
baseline=production.replace(profile_compile,profile_proof)
candidate=baseline.replace(needle,replacement)
if candidate.replace(replacement,needle)!=baseline:
    raise SystemExit('candidate changed more than the shared engine compiler flags')
for role,text in [('baseline',baseline),('candidate',candidate)]:
    (out/'build-input'/('Dockerfile.'+role)).write_text(text)
def tree(root):
    return {str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob('*')) if path.is_file()}
inputs={'source_commit':os.environ['PARSER_CONFIRM_REVISION'],
        'feature':feature,'candidate_flags':flags,
        'engine_source':tree(snapshot/'minivec/src'),
        'third_party':tree(snapshot/'minivec/third_party'),
        'training_source':{name:hashlib.sha256((snapshot/'minivec/tools/pgo'/name).read_bytes()).hexdigest()
                           for name in ['train.cpp','run-training.sh']},
        'dockerfiles':tree(out/'build-input'),
        'training_seed':2026111201,'evaluation_seeds':[7319,8911,13337],
        'candidate_only_flag':' '.join(flags),
        'common_proof_only_command':'sha256sum of the instrumented binary before original training',
        'default_pgo_standard':{'pairs_per_seed':5,'queries_per_phase':1000},
        'pgo_boundaries':{'scenarios':(['low16','update10','spill'] if feature=='spill'
                                       else ['low16','filter60k','update10','spill']),
                          'pairs_per_seed':1,'queries_per_phase':64},
        'pgo_spill_targets':({'scenarios':['spill_compound','spill_high'],
                              'pairs_per_seed':5,'queries_per_phase':128} if feature=='spill' else None),
        'optional_o3_standard':os.environ['PARSER_CONFIRM_O3']=='1'}
(out/'build-input.json').write_text(json.dumps(inputs,indent=2)+'\n')
PY
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' "$snapshot/Dockerfile")
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
variants=(baseline-pgo candidate-pgo)
[[ "$PARSER_CONFIRM_O3" == 1 ]] && variants+=(baseline-o3 candidate-o3)
for variant in "${variants[@]}"; do
  role=${variant%-*};pgo=0
  [[ "$variant" == *-pgo ]] && pgo=1
  target="$out/$variant"
  mkdir -p "$target/training"
  image="minivec:feature-confirm-$(basename "$scratch")-$variant"
  printf '%s\n' "build-$variant" > "$out/stage.txt"
  started=$(date +%s%N)
  # Independent profile generation for each role; no cached RUN or reused gcda.
  docker build --no-cache --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO="$pgo" --target build \
    -f "$out/build-input/Dockerfile.$role" -t "$image" "$snapshot" 2>&1 | tee "$target/build.log"
  stopped=$(date +%s%N)
  printf '{"nanoseconds":%s,"pgo_enabled":%s,"status":"passed"}\n' "$((stopped-started))" "$pgo" > "$target/build-cost.json"
  docker image inspect "$image" > "$target/image.json"
  container=$(docker create --entrypoint /bin/true "$image")
  docker cp "$container:/minivec" "$target/minivec"
  docker cp "$container:/src/src" "$target/built-source"
  docker cp "$container:/src/third_party" "$target/built-third-party"
  for name in train.cpp run-training.sh; do docker cp "$container:/training/$name" "$target/training/$name";done
  if [[ "$pgo" == 1 ]]; then
    docker cp "$container:/pgo" "$target/training/profile-data"
    for name in corpus corpus-sha256.txt profile-sha256.txt verification.jsonl instrumented-binary-sha256.txt; do
      docker cp "$container:/training/$name" "$target/training/$name"
    done
    for threads in 1 2; do
      for phase in 0 1 2; do
        docker cp "$container:/training/responses-$threads-$phase.jsonl" "$target/training/"
        docker cp "$container:/training/stderr-$threads-$phase.log" "$target/training/"
      done
    done
  fi
  docker rm -v "$container";container=
  sha256sum "$target/minivec" > "$target/binary-sha256.txt"
done

cd "$snapshot"
python3 - <<'PY'
import hashlib,json,os,platform,subprocess,sys,time
from pathlib import Path
out=Path(os.environ['PARSER_CONFIRM_OUT']);snapshot=Path(os.environ['PARSER_CONFIRM_SNAPSHOT'])
inputs=json.loads((out/'build-input.json').read_text())
variants=['baseline-pgo','candidate-pgo']
if os.environ['PARSER_CONFIRM_O3']=='1':variants+=['baseline-o3','candidate-o3']
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tree(root):return {str(p.relative_to(root)):sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
def save(name,data):(out/name).write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')
evidence={}
for variant in variants:
    target=out/variant;training=target/'training'
    if tree(target/'built-source')!=inputs['engine_source']:
        raise RuntimeError(variant+': built source differs from commit snapshot')
    if tree(target/'built-third-party')!=inputs['third_party']:
        raise RuntimeError(variant+': third-party source differs from snapshot')
    if {name:sha(training/name) for name in inputs['training_source']}!=inputs['training_source']:
        raise RuntimeError(variant+': training source changed')
    item={'binary_sha256':sha(target/'minivec'),
          'source_sha256':tree(target/'built-source'),
          'dockerfile_sha256':sha(out/'build-input'/('Dockerfile.'+variant.split('-')[0])),
          'image_id':json.loads((target/'image.json').read_text())[0]['Id'],
          'pgo':variant.endswith('-pgo')}
    if item['pgo']:
        profiles=tree(training/'profile-data')
        cpp_count=sum(name.endswith('.cpp') for name in inputs['engine_source'])
        if len(profiles)!=cpp_count or any(not key.endswith('.gcda') for key in profiles):
            raise RuntimeError(variant+': unexpected profile files')
        recorded={}
        for line in (training/'profile-sha256.txt').read_text().splitlines():
            digest,name=line.split(None,1);name=name.strip().lstrip('*')
            if not name.startswith('/pgo/'):raise RuntimeError('unexpected profile path')
            recorded[name.removeprefix('/pgo/')]=digest
        if profiles!=recorded:raise RuntimeError(variant+': generated/use profile mismatch')
        instrumented=(training/'instrumented-binary-sha256.txt').read_text().split()
        if len(instrumented)!=2 or len(instrumented[0])!=64 or instrumented[1]!='/minivec':
            raise RuntimeError(variant+': instrumented binary proof is missing')
        oracle=[json.loads(line) for line in (training/'verification.jsonl').read_text().splitlines()]
        if len(oracle)!=6 or any(check.get('status')!='passed' or check.get('responses',0)<=0 for check in oracle):
            raise RuntimeError(variant+': original training oracle did not pass all six phases')
        corpus=tree(training/'corpus');recorded_corpus={}
        for line in (training/'corpus-sha256.txt').read_text().splitlines():
            digest,name=line.split(None,1);name=name.strip().lstrip('*')
            if not name.startswith('/training/corpus/'):raise RuntimeError('unexpected corpus path')
            recorded_corpus[name.removeprefix('/training/corpus/')]=digest
        if corpus!=recorded_corpus:raise RuntimeError(variant+': training corpus checksum mismatch')
        responses={p.name:sha(p) for p in sorted(training.glob('responses-*.jsonl'))}
        if len(responses)!=6 or {check['actual_file'] for check in oracle}!=set(responses):
            raise RuntimeError(variant+': training oracle response evidence is incomplete')
        item.update(instrumented_binary_sha256=instrumented[0],profile_sha256=profiles,
                    training_corpus_sha256=corpus,training_oracle=oracle,
                    training_response_sha256=responses)
    evidence[variant]=item
if evidence['baseline-pgo']['training_corpus_sha256']!=evidence['candidate-pgo']['training_corpus_sha256']:
    raise RuntimeError('baseline/candidate trained on different original corpora')
context={'source_commit':inputs['source_commit'],'compiler':'pinned GCC13.3 O3 static',
         'feature':inputs['feature'],'candidate_flags':inputs['candidate_flags'],
         'candidate_only_flag':inputs['candidate_only_flag'],
         'profile_generate_flags':['-fprofile-generate=/pgo','-fprofile-update=atomic'],
         'profile_use_flags':['-fprofile-use=/pgo','-Werror=missing-profile','-Werror=coverage-mismatch'],
         'profile_source_proof':evidence,'production_dockerfile_unchanged':True,
         'cpu_info':(out/'lscpu.txt').read_text(),'platform':platform.platform(),
         'training_seed':inputs['training_seed'],'evaluation_seeds':inputs['evaluation_seeds'],
         'limitations':['Whole JSONL request/lifecycle measurements, not official scoring.',
           'Each PGO side generated its own profiles with the unchanged production training source/corpus.',
           'Boundaries have three pairs each and remain screening evidence.',
           'Shared machine and page cache; process RSS/IO are proxies, not cgroup scores.',
           'No change is automatically enabled from either microbenchmarks or these results.']}
save('context.json',context)

checks={'status':'running','checks':[]}
save('correctness.json',checks)
for variant in variants:
    binary=str(out/variant/'minivec')
    gates=[
        ('json_protocol',[sys.executable,'minivec/tests/json_protocol.py','--binary',binary,'--timeout','15'],{}),
        ('topk_serial',[sys.executable,'minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'1'}),
        ('topk_parallel',[sys.executable,'minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'2'})]
    if inputs['feature'] in ('typed','combined'):
        gates.append(('utf8_atomicity',[sys.executable,'minivec/tests/test_typed_response.py',
            '--binary',binary,'--output',str(out/variant/'utf8-atomicity.json')],{}))
    for name,args,overrides in gates:
        (out/'stage.txt').write_text(variant+'/'+name+'\n')
        started=time.monotonic()
        try:
            result=subprocess.run(args,env={**os.environ,**overrides},capture_output=True,timeout=240)
            code,stdout,stderr=result.returncode,result.stdout,result.stderr
        except subprocess.TimeoutExpired as error:
            code,stdout,stderr=None,error.stdout or b'',error.stderr or b''
        (out/variant/(name+'.stdout.log')).write_bytes(stdout)
        (out/variant/(name+'.stderr.log')).write_bytes(stderr)
        checks['checks'].append({'variant':variant,'test':name,'returncode':code,
                                'elapsed_seconds':time.monotonic()-started,'environment':overrides})
        save('correctness.json',checks)
        if code!=0:raise RuntimeError(variant+'/'+name+' failed; benchmark not started')
checks['status']='passed';save('correctness.json',checks)
if os.environ['FEATURE_CORRECTNESS_ONLY']=='1':
    for variant,item in evidence.items():
        if sha(out/variant/'minivec')!=item['binary_sha256']:
            raise RuntimeError(variant+': binary changed during correctness checks')
    save('summary.json',{'status':'passed','feature':inputs['feature'],
        'correctness_passed':True,'binary_unchanged':True,'profile_source_verified':True,
        'performance_measured':False,'database_pairs':0,
        'automatic_adoption':False,'official_score':None})
    sys.exit(0)

cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise RuntimeError('confirmation requires two allowed CPUs')
os.sched_setaffinity(0,cpus);context['measurement_affinity']=cpus;save('context.json',context)
def benchmark(mode,label,queries,pairs,scenarios,harness='benchmark_iteration.py'):
    (out/'stage.txt').write_text(mode+'/'+label+'\n')
    args=[sys.executable,'minivec/tools/'+harness]
    if harness=='benchmark_iteration.py':args+=['--suite','full']
    args+=['--baseline',str(out/('baseline-'+mode)/'minivec'),
          '--candidate',str(out/('candidate-'+mode)/'minivec'),
          '--output',str(out/(mode+'-'+label+'.json')),'--queries',str(queries),
          '--pairs',str(pairs),'--seeds','7319','8911','13337']
    for name in scenarios:args+=['--scenario',name]
    subprocess.run(args,check=True)
if inputs['pgo_spill_targets']:
    target=inputs['pgo_spill_targets']
    benchmark('pgo','spill-targets',target['queries_per_phase'],target['pairs_per_seed'],
              target['scenarios'],'benchmark_report_matrix.py')
benchmark('pgo','standard',1000,5,['standard10k'])
if inputs['feature']=='typed':
    (out/'stage.txt').write_text('pgo/typed-projections\n')
    subprocess.run([sys.executable,'minivec/tools/benchmark_projection.py',
        '--baseline',str(out/'baseline-pgo/minivec'),
        '--candidate',str(out/'candidate-pgo/minivec'),
        '--output',str(out/'pgo-projections.json'),
        '--pairs','1','--seeds','7319','8911','13337'],check=True)
boundary=inputs['pgo_boundaries']
benchmark('pgo','boundaries',boundary['queries_per_phase'],boundary['pairs_per_seed'],boundary['scenarios'])
if os.environ['PARSER_CONFIRM_O3']=='1':benchmark('o3','standard',1000,5,['standard10k'])
for variant,item in evidence.items():
    if sha(out/variant/'minivec')!=item['binary_sha256']:
        raise RuntimeError(variant+': binary changed during verification or measurements')
save('summary.json',{'status':'passed','feature':inputs['feature'],'candidate_flags':inputs['candidate_flags'],
     'pgo_standard_pairs':15,'pgo_boundary_screen_pairs':3*len(boundary['scenarios']),
     'pgo_spill_target_confirmation_pairs':30 if inputs['pgo_spill_targets'] else 0,
     'pgo_projection_screen_pairs':6 if inputs['feature']=='typed' else 0,
     'optional_o3_standard_pairs':15 if os.environ['PARSER_CONFIRM_O3']=='1' else 0,
     'correctness_passed':True,'binary_unchanged':True,'profile_source_verified':True,
     'automatic_adoption':False,'official_score':None})
PY
printf '%s\n' completed > "$out/stage.txt"
