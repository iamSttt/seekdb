#!/usr/bin/env bash
# I06 mixed mutation gates + I06/I07/I17 whole-request lifecycle screening.
# Production defaults stay unchanged. Each feature gets its own original PGO.
set -euo pipefail
# Imported frozen harnesses must not add __pycache__ files to the source proof.
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
test ! -e six-direction-results
test ! -e delta-confirmation-results
test ! -e spill-confirmation-results
test ! -e filter-confirmation-results
out="$repo/six-direction-results"
mkdir -p "$out/source" "$out/logs" "$out/unit"
export SIX_FINISH_OUT="$out" SIX_FINISH_REPO="$repo"
printf '%s\n' preflight > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  SIX_FINISH_EXIT="$code" python3 - <<'PYCOLLECT'
import hashlib,json,os
from pathlib import Path
out=Path(os.environ['SIX_FINISH_OUT'])
code=int(os.environ['SIX_FINISH_EXIT'])
summary_path=out/'summary.json'
summary=json.loads(summary_path.read_text()) if summary_path.exists() else {}
proof_ok=summary.get('status')=='passed' and summary.get('unit_gates_passed')==12 and summary.get('total_pairs')==27
status={'exit_code':code,'stage':(out/'stage.txt').read_text().strip(),
        'proof_complete':proof_ok,'official_score':None,'automatic_adoption':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
files=[]
for path in sorted(out.rglob('*')):
    if path.is_file() and 'source' not in path.relative_to(out).parts and path.name!='hashes.json':
        files.append({'path':path.relative_to(out).as_posix(),'bytes':path.stat().st_size,
                      'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
(out/'hashes.json').write_text(json.dumps({'artifacts':files,'excludes':['source/','hashes.json']},indent=2)+'\n')
for path in sorted(out.rglob('*')):
    if path.is_file() and 'source' not in path.relative_to(out).parts and path.suffix in {'.json','.txt'}:
        print('MINIVEC_SIX_FINISH_BEGIN '+json.dumps(path.relative_to(out).as_posix()))
        print(path.read_text(encoding='utf-8',errors='replace'))
        print('MINIVEC_SIX_FINISH_END')
if code==0 and not proof_ok:
    raise SystemExit('Successful process exit without all I06/I07/I17 evidence')
PYCOLLECT
  local report_code=$?
  if [[ "$report_code" != 0 ]]; then
    echo "Six-direction evidence collection failed: exit $report_code" >&2
    if [[ "$code" == 0 ]]; then code=$report_code;fi
    printf '{"exit_code":%d,"report_collection_exit_code":%d}\n' "$code" "$report_code" > "$out/report-collection-status.json"
  fi
  exit "$code"
}
trap finish EXIT

# Freeze tests/harnesses as well as engine inputs; never test uncommitted code.
git diff --quiet HEAD -- Dockerfile .dockerignore minivec/src minivec/third_party minivec/tests minivec/tools protocol_check
git rev-parse HEAD > "$out/source-commit.txt"
git archive HEAD Dockerfile .dockerignore minivec/src minivec/third_party minivec/tests minivec/tools protocol_check | tar -x -C "$out/source"
python3 - <<'PYRUN'
import hashlib,json,os,platform,re,shutil,subprocess,sys,time
from pathlib import Path
out=Path(os.environ['SIX_FINISH_OUT']);repo=Path(os.environ['SIX_FINISH_REPO'])
source=out/'source';revision=(out/'source-commit.txt').read_text().strip()
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
def require(test,message):
    if not test:raise RuntimeError(message)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tree(path):return {p.relative_to(path).as_posix():sha(p) for p in sorted(path.rglob('*')) if p.is_file()}
def save(name,value):(out/name).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
def load(path):return json.loads(path.read_text())
def stage(name):
    (out/'stage.txt').write_text(name+'\n');print('SIX_FINISH_STAGE '+name,flush=True)
require(platform.system()=='Linux','Requires Linux, Docker, Python3 and two available CPUs')
require(re.fullmatch('[0-9a-f]{40}',revision) is not None,'Expected full source commit')
require(compiler in (source/'Dockerfile').read_text(),'Pinned production compiler changed')
cpus=sorted(os.sched_getaffinity(0))[:2]
require(len(cpus)==2,'Two available CPUs are required')
inputs=tree(source)
context={'source_commit':revision,'compiler_image':compiler,'source_files_sha256':inputs,
         'host':{'platform':platform.platform(),'available_affinity':sorted(os.sched_getaffinity(0)),
                 'measurement_affinity':cpus},
         'unit_scope':'Separate non-PGO internal-API test executables, compiled from the same proven engine source; not the PGO engine binaries.',
         'benchmark_scope':'The exact extracted and correctness-checked baseline/candidate PGO engine binaries.',
         'expected_unit_gates':12,'expected_delta_pairs':9,'expected_spill_pairs':12,'expected_filter_pairs':6,
         'screen_only':True,'automatic_adoption':False,'official_score':None}
save('context.json',context)
progress={'status':'running','steps':[],'unit_builds':{},'unit_gates':[],'feature_proofs':{}}
save('progress.json',progress)
def run(label,args,timeout,env=None):
    stage(label)
    record={'label':label,'argv':args,'status':'running','timeout_seconds':timeout,
            'stdout':'logs/'+label+'.stdout.log','stderr':'logs/'+label+'.stderr.log'}
    progress['steps'].append(record);save('progress.json',progress)
    started=time.monotonic()
    try:
        with (out/record['stdout']).open('wb') as stdout,(out/record['stderr']).open('wb') as stderr:
            result=subprocess.run(args,cwd=repo,env=env,stdout=stdout,stderr=stderr,timeout=timeout)
        record.update(returncode=result.returncode,status='passed' if result.returncode==0 else 'failed')
    except Exception as error:
        record.update(returncode=None,status='failed',error=type(error).__name__+': '+str(error))
        raise
    finally:
        record['elapsed_seconds']=time.monotonic()-started;save('progress.json',progress)
    require(record['returncode']==0,label+' failed; see retained stdout/stderr')
    return record
def prove_feature(feature):
    target=repo/(feature+'-confirmation-results')
    required=['context.json','summary.json','build-input.json','correctness.json','run-status.json']
    proofdir=out/'proofs'/feature;proofdir.mkdir(parents=True,exist_ok=True)
    for name in required:shutil.copyfile(target/name,proofdir/name)
    ctx=load(target/'context.json');summary=load(target/'summary.json');build=load(target/'build-input.json')
    correct=load(target/'correctness.json');status=load(target/'run-status.json')
    candidate_flags={'delta':['-DMVDB_ENABLE_INDEX_DELTA'],'spill':['-DMVDB_ENABLE_SPILL_FILTER'],'filter':[]}[feature]
    baseline_flags=['-DMVDB_DISABLE_MULTI_POSTING'] if feature=='filter' else []
    require(ctx['source_commit']==revision==build['source_commit'],'Feature source commit mismatch')
    require(ctx['feature']==feature and ctx['candidate_flags']==candidate_flags==build['candidate_flags'] and
            ctx['baseline_flags']==baseline_flags==build['baseline_flags'],
            'Feature macro mapping mismatch')
    require(summary['status']=='passed' and summary['correctness_passed'] is True and
            summary['profile_source_verified'] is True and summary['binary_unchanged'] is True and
            summary['performance_measured'] is False and summary['database_pairs']==0 and status['exit_code']==0,
            'Incomplete feature build/profile/correctness proof')
    require(build['engine_source']==tree(source/'minivec/src') and
            build['third_party']==tree(source/'minivec/third_party'),'Feature build differs from frozen source')
    require(build['training_source']=={name:sha(source/'minivec/tools/pgo'/name)
            for name in ('train.cpp','run-training.sh')},'Training source differs from frozen source')
    require(ctx['profile_generate_flags']==['-fprofile-generate=/pgo','-fprofile-update=atomic'] and
            ctx['profile_use_flags']==['-fprofile-use=/pgo','-Werror=missing-profile','-Werror=coverage-mismatch'],
            'Strict PGO generation/use flags changed')
    expected={(role,test) for role in ('baseline-pgo','candidate-pgo') for test in ('json_protocol','topk_serial','topk_parallel')}
    checks=correct['checks']
    require(correct['status']=='passed' and len(checks)==6 and
            {(x['variant'],x['test']) for x in checks}==expected and all(x['returncode']==0 for x in checks),
            'Missing PGO engine correctness gate')
    binaries={}
    for side in ('baseline','candidate'):
        role=side+'-pgo';binary=target/role/'minivec';item=ctx['profile_source_proof'][role]
        require(item['pgo'] is True and sha(binary)==item['binary_sha256'],'PGO executable proof mismatch')
        require(item['source_sha256']==build['engine_source']==tree(target/role/'built-source'),
                'PGO extracted source mismatch')
        require(item['profile_sha256']==tree(target/role/'training/profile-data'),'PGO profile bytes changed')
        require(len(item['training_oracle'])==6 and all(x['status']=='passed' for x in item['training_oracle']),
                'PGO training oracle incomplete')
        binaries[side]={'path':str(binary),'sha256':sha(binary),'image_id':item['image_id']}
    proof={'source_commit':revision,'candidate_flags':candidate_flags,'baseline_flags':baseline_flags,'binaries':binaries,
           'source_and_profile_verified':True,'correctness_gates':6,
           'retained_build_directory':str(target),
           'copied_proof_sha256':{name:sha(target/name) for name in required}}
    progress['feature_proofs'][feature]=proof;save('progress.json',progress)
    return binaries
def confirm(feature):
    env={**os.environ,'CONFIRM_FEATURE':feature,'FEATURE_CORRECTNESS_ONLY':'1','PARSER_CONFIRM_O3':'0'}
    try:run('pgo-'+feature,['bash',str(repo/'minivec/tools/ci_parser_confirmation.sh')],2400,env)
    finally:
        # Preserve incomplete build/oracle evidence as well as command logs.
        target=repo/(feature+'-confirmation-results');proofdir=out/'proofs'/feature
        proofdir.mkdir(parents=True,exist_ok=True)
        if target.is_dir():
            for path in target.glob('*.json'):shutil.copyfile(path,proofdir/path.name)
    return prove_feature(feature)
def unit_build(label,test,flags):
    args=['docker','run','--rm','--network','none','--platform','linux/amd64',
          '-v',str(source)+':/code:ro','-v',str(out/'unit')+':/out','-w','/code/minivec',compiler,
          'g++','-std=c++17','-pthread','-Isrc',*flags,'tests/'+test+'.cpp',
          'src/db.cpp','src/flat_index.cpp','src/value.cpp']
    if test!='test_resident_update':args+=['src/exec.cpp','src/sql.cpp']
    if test=='test_resident_update':args+=['-DMVDB_TEST_WRAP_PWRITE','-Wl,--wrap=pwrite']
    args+=['-o','/out/'+label]
    run('compile-'+label,args,600)
    binary=out/'unit'/label
    progress['unit_builds'][label]={'test_source':test+'.cpp','test_source_sha256':sha(source/'minivec/tests'/(test+'.cpp')),
        'binary_sha256':sha(binary),'flags':flags,'engine_source_sha256':tree(source/'minivec/src'),
        'pgo':False,'compiler_image':compiler}
    save('progress.json',progress)
def unit_run(binary,mode=None):
    label='unit-'+binary+('-'+mode.removeprefix('--') if mode else '')
    args=['docker','run','--rm','--network','none','--platform','linux/amd64',
          '-e','ASAN_OPTIONS=detect_leaks=0:abort_on_error=1',
          '-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1',
          '-v',str(out/'unit')+':/tests:ro',compiler,'/tests/'+binary]
    if mode:args.append(mode)
    record=run(label,args,240)
    require(sha(out/'unit'/binary)==progress['unit_builds'][binary]['binary_sha256'],'Unit binary changed')
    require(b'PASS ' in (out/record['stdout']).read_bytes(),'Unit PASS evidence missing')
    progress['unit_gates'].append({'binary':binary,'mode':mode or 'default','status':'passed',
        'binary_sha256':sha(out/'unit'/binary),'step':label});save('progress.json',progress)
def benchmark(feature,harness,cases,filename,binaries,extra=()):
    os.sched_setaffinity(0,cpus)
    args=[sys.executable,str(source/'minivec/tools'/harness),
          '--baseline',binaries['baseline']['path'],'--candidate',binaries['candidate']['path'],
          '--seeds','7319','8911','13337','--pairs','1','--output',str(out/filename),*extra]
    for case in cases:args+=['--scenario',case]
    run('benchmark-'+feature,args,1800)
    report=load(out/filename);pairs=report['pairs'];expected={(case,seed,0) for case in cases for seed in (7319,8911,13337)}
    ids=[(p['scenario'],p['seed'],p['pair_index']) for p in pairs]
    require(report['status']=='passed' and report['total_pairs']==report['expected_total_pairs']==len(expected) and
            len(ids)==len(set(ids))==len(expected) and set(ids)==expected,'Incomplete or duplicate benchmark identities')
    require(report['binary_unchanged'] is True and all(p['all_responses_identical'] is True for p in pairs),
            'Benchmark responses or executable checks incomplete')
    for pair in pairs:
        a,b=pair['baseline'],pair['candidate']
        require(re.fullmatch('[0-9a-f]{64}',a['response_sha256']) is not None and
                a['response_sha256']==b['response_sha256'],'Paired response hash mismatch')
        for side in (a,b):
            require(side['correctness']['all_query_responses_independent_oracle'] is True,
                    'Independent per-query oracle evidence missing')
            if feature=='spill':require(side.get('real_spill_observed') is True,'Real spill proof missing')
            if feature=='filter':require(side.get('resident_storage_observed') is True,'Resident storage proof missing')
    if feature in ('spill','filter'):
        require(report['build_context']['sha256']==sha(out/(feature+'-build-context.json')),
                'Lifecycle build-context binding mismatch')
    for side,item in binaries.items():
        require(report['binaries'][side]['sha256']==item['sha256']==sha(Path(item['path'])),
                'Measured PGO executable differs from its proof')
    return {'report':filename,'sha256':sha(out/filename),'pairs':len(pairs),'status':'passed',
            'source_commit':revision,'binaries':binaries,'all_identities_verified':True}
def lifecycle_context(feature,binaries):
    ctx=load(out/('proofs/'+feature+'/context.json'));build=load(out/('proofs/'+feature+'/build-input.json'))
    filename=feature+'-build-context.json'
    save(filename,{'pgo_enabled':True,'same_compiler_and_training':True,'feature':feature,
        'source_commit':revision,'binaries':binaries,'compiler':ctx['compiler'],
        'candidate_flags':ctx['candidate_flags'],'baseline_flags':ctx['baseline_flags'],
        'profile_generate_flags':ctx['profile_generate_flags'],'profile_use_flags':ctx['profile_use_flags'],
        'training_source':build['training_source'],'source_context':'proofs/'+feature+'/context.json',
        'source_context_sha256':sha(out/('proofs/'+feature+'/context.json')),
        'proof':'Both independently trained builds passed ci_parser_confirmation source/profile/corpus checks.'})
    return ['--build-context',str(out/filename)]
try:
    # Use the real committed entry; its own git archive and proof check freeze
    # all build inputs independently. The saved copy hashes that entry as well.
    delta=confirm('delta')
    normal=['-O2'];enabled=['-O2','-DMVDB_ENABLE_INDEX_DELTA']
    sanitized=['-O1','-g','-DMVDB_ENABLE_INDEX_DELTA','-fsanitize=address,undefined',
               '-fno-omit-frame-pointer','-fno-pie','-no-pie']
    builds=[('mutations-baseline','test_index_delta_mutations',normal),
            ('mutations-delta','test_index_delta_mutations',enabled),
            ('mutations-spill','test_index_delta_mutations',enabled+['-DMVDB_SPILL_BYTES=1','-DMVDB_FLAT_INDEX_MEMORY_BYTES=1024']),
            ('mutations-sanitized','test_index_delta_mutations',sanitized),
            ('faults-delta','test_index_delta',enabled),('faults-sanitized','test_index_delta',sanitized),
            ('resident-delta','test_resident_update',enabled),('resident-sanitized','test_resident_update',sanitized)]
    for label,test,flags in builds:
        unit_build(label,test,flags);unit_run(label)
        if label in ('mutations-delta','mutations-sanitized'):
            unit_run(label,'--ann-half');unit_run(label,'--ann-off')
    require(len(progress['unit_gates'])==12,'Expected twelve internal-API gates')
    delta_result=benchmark('delta','benchmark_index_delta.py',
        ['mixed_small','same_pk_churn','over_64_limit'],'index-delta.json',delta)
    spill=confirm('spill')
    spill_result=benchmark('spill','benchmark_write_query.py',
        ['spill_low_q1','spill_high_q1','spill_low_q16','spill_high_q16'],'write-query.json',spill,
        lifecycle_context('spill',spill))
    filtering=confirm('filter')
    filter_result=benchmark('filter','benchmark_write_query.py',
        ['resident_compound_q1','resident_compound_q16'],'filter-write-query.json',filtering,
        lifecycle_context('filter',filtering))
    require(tree(source)==inputs,'Frozen source/test/harness files changed')
    require(subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()==revision,'Checkout commit changed')
    for name in ('ci_parser_confirmation.sh','ci_index_delta_complete.sh'):
        require(sha(repo/'minivec/tools'/name)==sha(source/'minivec/tools'/name),'Build entry changed during run')
    for feature,proof in progress['feature_proofs'].items():
        for side,item in proof['binaries'].items():
            require(sha(Path(item['path']))==item['sha256'],feature+'/'+side+' changed after measurement')
    progress['status']='passed';save('progress.json',progress)
    save('summary.json',{'status':'passed','source_commit':revision,'unit_gates_passed':12,
         'pgo_engine_correctness_gates':18,'benchmarks':{'delta':delta_result,'spill':spill_result,'filter':filter_result},
         'total_pairs':27,'expected_total_pairs':27,'all_source_profile_binary_bindings_verified':True,
         'official_score':None,'automatic_adoption':False,
         'limitations':['Three seeds and one pair per scenario are screening, not final adoption evidence.',
                       'Internal-API tests use separately compiled non-PGO executables; timing uses the proven PGO engines.',
                       'ASan leak detection is disabled; ASan/UBSan error detection remains enabled.',
                       'Process resource snapshots are proxies; no official cgroup limits or score are inferred.',
                       'After an external timeout only already-flushed reports and logs can be retained.',
                       'Original PGO evidence remains in delta/spill/filter-confirmation-results/.',
                       'I17 changes only the multi-filter cache; I08 is identical on both sides and I07 is not combined.']})
    stage('complete')
except BaseException as error:
    progress.update(status='failed',error=type(error).__name__+': '+str(error));save('progress.json',progress)
    raise
PYRUN
