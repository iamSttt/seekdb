#!/usr/bin/env bash
# Historical production PGO versus the current I07 + I14 integration candidate.
# Each side uses its own committed source/training/Dockerfile; no production edit.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline_revision=969ecca98f817ecdef9b0c378fc99a0d6cef510d
candidate_revision=$(git rev-parse HEAD)
git cat-file -e "$baseline_revision^{commit}"
out="$repo/integration-report-results"
test ! -e "$out"
mkdir -p "$out/build-input"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-report-integration-XXXXXX")
container=
printf '%s\n' archive > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ -n "$container" ]]; then docker rm -v "$container";fi
  INTEGRATION_EXIT="$code" INTEGRATION_OUT="$out" python3 - <<'PY'
import json,os
from pathlib import Path
out=Path(os.environ['INTEGRATION_OUT'])
status={'exit_code':int(os.environ['INTEGRATION_EXIT']),
        'stage':(out/'stage.txt').read_text().strip(),
        'feature':'spill-filter+typed-json','official_score':None,'automatic_adoption':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
for path in sorted(out.glob('*.json')):
    print('MINIVEC_INTEGRATION_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_INTEGRATION_END')
PY
  local report_code=$?
  if [[ "$report_code" != 0 ]]; then
    echo "Integration evidence collection failed: exit $report_code" >&2
    if [[ "$code" == 0 ]]; then code=$report_code;fi
    printf '{"exit_code":%d,"report_collection_exit_code":%d,"automatic_adoption":false}\n' \
      "$code" "$report_code" > "$out/run-status.json"
  fi
  exit "$code"
}
trap finish EXIT
for role in baseline candidate; do
  revision=$baseline_revision
  [[ "$role" == candidate ]] && revision=$candidate_revision
  mkdir "$scratch/$role"
  git archive "$revision" Dockerfile .dockerignore minivec/src minivec/third_party \
    minivec/tests minivec/tools protocol_check | tar -x -C "$scratch/$role"
done
git status --porcelain --untracked-files=no > "$out/worktree-status.txt"
lscpu > "$out/lscpu.txt"
export INTEGRATION_OUT="$out" INTEGRATION_SCRATCH="$scratch"
export INTEGRATION_BASELINE_REVISION="$baseline_revision" INTEGRATION_CANDIDATE_REVISION="$candidate_revision"
python3 - <<'PY'
import hashlib,json,os
from pathlib import Path
out=Path(os.environ['INTEGRATION_OUT']);scratch=Path(os.environ['INTEGRATION_SCRATCH'])
flags=['-DMVDB_ENABLE_SPILL_FILTER','-DMVDB_ENABLE_TYPED_JSON']
needle="flags='-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc'"
profile_compile='g++ $flags -fprofile-generate=/pgo -fprofile-update=atomic src/*.cpp -o /minivec;'
profile_use='g++ $flags -fprofile-use=/pgo -Werror=missing-profile -Werror=coverage-mismatch src/*.cpp -o /minivec'
continuation=' '+chr(92)+'\n'
proof=(continuation+'         cp /minivec /training/instrumented-minivec;'+continuation+
       '         sha256sum /minivec > /training/instrumented-binary-sha256.txt;')
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tree(root):return {p.relative_to(root).as_posix():sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
roles={}
for role in ['baseline','candidate']:
    snapshot=scratch/role
    production=(snapshot/'Dockerfile').read_text()
    if production.count(needle)!=1 or production.count('g++ $flags ')!=3:
        raise RuntimeError(role+': production compiler commands changed; review flag injection')
    if production.count(profile_compile)!=1 or production.count(profile_use)!=1:
        raise RuntimeError(role+': original profile generation/use commands changed')
    if ('FROM '+compiler+' AS build') not in production.splitlines():
        raise RuntimeError(role+': unexpected compiler image')
    modified=production.replace(profile_compile,profile_compile+proof)
    expected_flags=flags if role=='candidate' else []
    if expected_flags:
        replacement=needle[:-1]+' '+' '.join(expected_flags)+"'"
        modified=modified.replace(needle,replacement)
        restored=modified.replace(replacement,needle)
    else:restored=modified
    if restored.replace(profile_compile+proof,profile_compile)!=production:
        raise RuntimeError(role+': temporary Dockerfile changed beyond flags and binary proof')
    (out/'build-input'/('Dockerfile.'+role)).write_text(modified)
    roles[role]={'source_commit':os.environ['INTEGRATION_'+role.upper()+'_REVISION'],
                 'feature_flags':expected_flags,
                 'engine_source':tree(snapshot/'minivec/src'),
                 'third_party':tree(snapshot/'minivec/third_party'),
                 'training_source':{name:sha(snapshot/'minivec/tools/pgo'/name)
                                    for name in ['train.cpp','run-training.sh']},
                 'production_dockerfile_sha256':sha(snapshot/'Dockerfile'),
                 'build_dockerfile_sha256':sha(out/'build-input'/('Dockerfile.'+role)),
                 'dockerignore_sha256':sha(snapshot/'.dockerignore')}
def differences(left,right):
    return {'added':sorted(right.keys()-left.keys()),'removed':sorted(left.keys()-right.keys()),
            'modified':sorted(name for name in left.keys() & right.keys() if left[name]!=right[name])}
inputs={'kind':'historical-production-versus-current-integration',
        'feature':'spill-filter+typed-json','candidate_flags':flags,'compiler_image':compiler,
        'roles':roles,'same_source_comparison':False,
        'source_differences':{field:differences(roles['baseline'][field],roles['candidate'][field])
                              for field in ['engine_source','third_party','training_source']},
        'harness_commit':roles['candidate']['source_commit'],
        'harness_source':{directory:tree(scratch/'candidate'/directory)
                          for directory in ['minivec/tests','minivec/tools','protocol_check']},
        'proof_only_commands':['copy instrumented binary before original training',
                               'sha256sum instrumented binary before original training'],
        'evaluation_seeds':[7319,8911,13337],
        'measurement_plan':[
            {'name':'standard','harness':'benchmark_iteration.py','scenarios':['standard10k'],
             'queries_per_phase':1000,'pairs_per_seed':5},
            {'name':'boundaries','harness':'benchmark_iteration.py',
             'scenarios':['low16','filter60k','update10','spill'],'queries_per_phase':64,'pairs_per_seed':1},
            {'name':'size-boundaries','harness':'benchmark_report_matrix.py',
             'scenarios':['high768_k1','high768_k100','large100k'],'queries_per_phase':64,'pairs_per_seed':1},
            {'name':'filters','harness':'benchmark_report_matrix.py',
             'scenarios':['compound','alternate_columns','spill_compound','spill_high'],
             'queries_per_phase':128,'pairs_per_seed':1},
            {'name':'projections','harness':'benchmark_projection.py','scenarios':['wide768','many16'],
             'queries_per_phase':None,'pairs_per_seed':1}],
        'expected_total_pairs':54,'automatic_adoption':False,'official_score':None}
(out/'build-input.json').write_text(json.dumps(inputs,indent=2)+'\n')
PY
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' "$scratch/baseline/Dockerfile")
docker pull "$compiler"
for role in baseline candidate; do
  target="$out/$role-pgo"
  mkdir -p "$target/training"
  image="minivec:report-integration-$(basename "$scratch")-$role"
  printf '%s\n' "build-$role-pgo" > "$out/stage.txt"
  started=$(date +%s%N)
  # Independent PGO from each role's own source and original training generator.
  docker build --no-cache --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO=1 --target build \
    -f "$out/build-input/Dockerfile.$role" -t "$image" "$scratch/$role" 2>&1 | tee "$target/build.log"
  stopped=$(date +%s%N)
  printf '{"nanoseconds":%s,"pgo_enabled":true,"status":"passed"}\n' "$((stopped-started))" > "$target/build-cost.json"
  docker image inspect "$image" > "$target/image.json"
  container=$(docker create --entrypoint /bin/true "$image")
  docker cp "$container:/minivec" "$target/minivec"
  docker cp "$container:/src/src" "$target/built-source"
  docker cp "$container:/src/third_party" "$target/built-third-party"
  docker cp "$container:/pgo" "$target/training/profile-data"
  for name in train.cpp run-training.sh corpus corpus-sha256.txt profile-sha256.txt \
      verification.jsonl instrumented-binary-sha256.txt instrumented-minivec; do
    docker cp "$container:/training/$name" "$target/training/$name"
  done
  for threads in 1 2; do
    for phase in 0 1 2; do
      docker cp "$container:/training/responses-$threads-$phase.jsonl" "$target/training/"
      docker cp "$container:/training/stderr-$threads-$phase.log" "$target/training/"
    done
  done
  docker rm -v "$container";container=
  sha256sum "$target/minivec" > "$target/binary-sha256.txt"
done

# Common, committed candidate harness; binaries still come from their own builds.
cd "$scratch/candidate"
python3 - <<'PY'
import hashlib,json,os,platform,re,subprocess,sys,time
from pathlib import Path
out=Path(os.environ['INTEGRATION_OUT']);scratch=Path(os.environ['INTEGRATION_SCRATCH'])
inputs=json.loads((out/'build-input.json').read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tree(root):return {p.relative_to(root).as_posix():sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
def save(name,data):(out/name).write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')
def manifest(path,prefix):
    recorded={}
    for line in path.read_text().splitlines():
        digest,name=line.split(None,1);name=name.strip().lstrip('*')
        if not re.fullmatch('[0-9a-f]{64}',digest) or not name.startswith(prefix):
            raise RuntimeError(str(path)+': invalid checksum entry')
        relative=name.removeprefix(prefix)
        if relative in recorded:raise RuntimeError(str(path)+': duplicate checksum entry')
        recorded[relative]=digest
    return recorded
evidence={}
for role,source in inputs['roles'].items():
    variant=role+'-pgo';target=out/variant;training=target/'training'
    if tree(target/'built-source')!=source['engine_source']:
        raise RuntimeError(variant+': built source differs from its own commit snapshot')
    if tree(target/'built-third-party')!=source['third_party']:
        raise RuntimeError(variant+': third-party source differs from its own snapshot')
    if {name:sha(training/name) for name in source['training_source']}!=source['training_source']:
        raise RuntimeError(variant+': original training source changed')
    if sha(out/'build-input'/('Dockerfile.'+role))!=source['build_dockerfile_sha256']:
        raise RuntimeError(variant+': build Dockerfile changed')
    if sha(scratch/role/'Dockerfile')!=source['production_dockerfile_sha256']:
        raise RuntimeError(variant+': archived production Dockerfile changed')
    profiles=tree(training/'profile-data')
    cpp_count=sum(name.endswith('.cpp') for name in source['engine_source'])
    if len(profiles)!=cpp_count or any(not key.endswith('.gcda') for key in profiles):
        raise RuntimeError(variant+': unexpected profile files')
    if profiles!=manifest(training/'profile-sha256.txt','/pgo/'):
        raise RuntimeError(variant+': generated/use profile mismatch')
    instrumented=manifest(training/'instrumented-binary-sha256.txt','/')
    if instrumented!={'minivec':sha(training/'instrumented-minivec')}:
        raise RuntimeError(variant+': instrumented binary checksum mismatch')
    runtime=manifest(target/'binary-sha256.txt',str(target)+'/')
    if runtime!={'minivec':sha(target/'minivec')}:
        raise RuntimeError(variant+': extracted runtime binary checksum mismatch')
    oracle=[json.loads(line) for line in (training/'verification.jsonl').read_text().splitlines()]
    if len(oracle)!=6 or any(check.get('status')!='passed' or check.get('responses',0)<=0 for check in oracle):
        raise RuntimeError(variant+': original training oracle did not pass all six phases')
    corpus=tree(training/'corpus')
    if corpus!=manifest(training/'corpus-sha256.txt','/training/corpus/'):
        raise RuntimeError(variant+': training corpus checksum mismatch')
    responses={p.name:sha(p) for p in sorted(training.glob('responses-*.jsonl'))}
    if len(responses)!=6 or {check['actual_file'] for check in oracle}!=set(responses):
        raise RuntimeError(variant+': training response evidence is incomplete')
    evidence[role]={'source_commit':source['source_commit'],'feature_flags':source['feature_flags'],
                    'binary_sha256':runtime['minivec'],'source_sha256':tree(target/'built-source'),
                    'build_dockerfile_sha256':source['build_dockerfile_sha256'],
                    'image_id':json.loads((target/'image.json').read_text())[0]['Id'],
                    'instrumented_binary_sha256':instrumented['minivec'],'profile_sha256':profiles,
                    'training_source_sha256':source['training_source'],
                    'training_corpus_sha256':corpus,'training_oracle':oracle,
                    'training_response_sha256':responses}
for directory,recorded in inputs['harness_source'].items():
    if tree(scratch/'candidate'/directory)!=recorded:
        raise RuntimeError('committed harness changed: '+directory)
context={'kind':inputs['kind'],'feature':inputs['feature'],
         'baseline_commit':inputs['roles']['baseline']['source_commit'],
         'candidate_commit':inputs['roles']['candidate']['source_commit'],
         'harness_commit':inputs['harness_commit'],'candidate_flags':inputs['candidate_flags'],
         'same_source_comparison':False,'source_differences':inputs['source_differences'],
         'compiler':inputs['compiler_image'],'profile_source_proof':evidence,
         'profile_generate_flags':['-fprofile-generate=/pgo','-fprofile-update=atomic'],
         'profile_use_flags':['-fprofile-use=/pgo','-Werror=missing-profile','-Werror=coverage-mismatch'],
         'production_dockerfiles_unchanged':True,
         'training_corpora_equal':evidence['baseline']['training_corpus_sha256']==evidence['candidate']['training_corpus_sha256'],
         'cpu_info':(out/'lscpu.txt').read_text(),'platform':platform.platform(),
         'evaluation_seeds':inputs['evaluation_seeds'],'measurement_plan':inputs['measurement_plan'],
         'limitations':['Historical baseline versus current integration; source changes and flags are both present.',
             'Each role uses its own original production PGO training; corpus equality is recorded, not assumed.',
             'Synthetic whole-request/lifecycle measurements; not an official score or feature-only attribution.',
             'Shared host/page cache; RSS and process I/O are proxies, not official cgroup scores.',
             'Only standard10k has 15 pairs; other scenarios are three-pair boundary screens.',
             'No production switch is automatically enabled by this experiment.']}
save('context.json',context)
checks={'status':'running','checks':[]};save('correctness.json',checks)
for role in ['baseline','candidate']:
    binary=str(out/(role+'-pgo')/'minivec')
    # Run UTF-8 atomicity on both actual PGO binaries, including the candidate.
    for name,args,overrides in [
        ('json_protocol',[sys.executable,'minivec/tests/json_protocol.py','--binary',binary,'--timeout','15'],{}),
        ('utf8_atomicity',[sys.executable,'minivec/tests/test_typed_response.py','--binary',binary,
                           '--output',str(out/(role+'-utf8.json'))],{}),
        ('topk_serial',[sys.executable,'minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'1'}),
        ('topk_parallel',[sys.executable,'minivec/tests/topk_bounds.py','--binary',binary],{'MVDB_QUERY_THREADS':'2'})]:
        (out/'stage.txt').write_text(role+'/'+name+'\n');started=time.monotonic()
        try:
            result=subprocess.run(args,env={**os.environ,**overrides},capture_output=True,timeout=240)
            code,stdout,stderr=result.returncode,result.stdout,result.stderr
        except subprocess.TimeoutExpired as error:
            code,stdout,stderr=None,error.stdout or b'',error.stderr or b''
        (out/(role+'-pgo')/(name+'.stdout.log')).write_bytes(stdout)
        (out/(role+'-pgo')/(name+'.stderr.log')).write_bytes(stderr)
        checks['checks'].append({'role':role,'test':name,'returncode':code,
                                'elapsed_seconds':time.monotonic()-started,'environment':overrides,
                                'binary_sha256':evidence[role]['binary_sha256']})
        if code!=0:
            checks['status']='failed';save('correctness.json',checks)
            raise RuntimeError(role+'/'+name+' failed; benchmarks not started')
        save('correctness.json',checks)
checks['status']='passed';save('correctness.json',checks)
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise RuntimeError('integration confirmation requires two allowed CPUs')
os.sched_setaffinity(0,cpus);context['measurement_affinity']=cpus;save('context.json',context)
measurements=[]
for plan in inputs['measurement_plan']:
    (out/'stage.txt').write_text('measure/'+plan['name']+'\n')
    report_path=out/('pgo-'+plan['name']+'.json')
    args=[sys.executable,'minivec/tools/'+plan['harness'],
          '--baseline',str(out/'baseline-pgo/minivec'),'--candidate',str(out/'candidate-pgo/minivec'),
          '--output',str(report_path),'--pairs',str(plan['pairs_per_seed']),
          '--seeds',*[str(seed) for seed in inputs['evaluation_seeds']]]
    if plan['harness']=='benchmark_iteration.py':args+=['--suite','full']
    if plan['queries_per_phase'] is not None:args+=['--queries',str(plan['queries_per_phase'])]
    for scenario in plan['scenarios']:args+=['--scenario',scenario]
    subprocess.run(args,check=True)
    report=json.loads(report_path.read_text())
    expected={(name,seed,pair) for name in plan['scenarios'] for seed in inputs['evaluation_seeds']
              for pair in range(plan['pairs_per_seed'])}
    actual=[(pair['scenario'],pair['seed'],pair['pair_index']) for pair in report['pairs']]
    if report.get('status')!='passed' or len(actual)!=len(expected) or set(actual)!=expected:
        raise RuntimeError(plan['name']+': incomplete or duplicate measurement pairs')
    for role in ['baseline','candidate']:
        if report['binaries'][role]['sha256']!=evidence[role]['binary_sha256']:
            raise RuntimeError(plan['name']+': measured binary differs from built PGO evidence')
    verified_key=('all_responses_identical_and_independently_verified'
                  if plan['harness']=='benchmark_projection.py' else 'all_responses_identical')
    if not all(pair.get(verified_key) is True for pair in report['pairs']):
        raise RuntimeError(plan['name']+': response verification is missing')
    measurements.append({'name':plan['name'],'report':report_path.name,
                         'report_sha256':sha(report_path),'verified_pairs':len(actual)})
for role in ['baseline','candidate']:
    if sha(out/(role+'-pgo')/'minivec')!=evidence[role]['binary_sha256']:
        raise RuntimeError(role+': runtime binary changed during verification or measurement')
total=sum(item['verified_pairs'] for item in measurements)
if total!=inputs['expected_total_pairs']:raise RuntimeError('integration pair total differs from plan')
save('summary.json',{'status':'passed','feature':inputs['feature'],
     'baseline_commit':inputs['roles']['baseline']['source_commit'],
     'candidate_commit':inputs['roles']['candidate']['source_commit'],'candidate_flags':inputs['candidate_flags'],
     'same_source_comparison':False,'measurements':measurements,'verified_total_pairs':total,
     'correctness_passed':True,'pgo_utf8_atomicity_both_roles':True,
     'binary_unchanged':True,'profile_source_verified':True,
     'automatic_adoption':False,'official_score':None})
PY
printf '%s\n' completed > "$out/stage.txt"
