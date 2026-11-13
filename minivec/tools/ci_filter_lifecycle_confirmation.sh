#!/usr/bin/env bash
# Isolated I17 admission-gate confirmation. No default or workflow changes.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "$BASH_SOURCE")/../.." && pwd)
cd "$repo"
test ! -e filter-lifecycle-confirmation-results
test ! -e filter-reuse-confirmation-results
out="$repo/filter-lifecycle-confirmation-results"
mkdir -p "$out/source" "$out/logs" "$out/proof" "$out/unit"
export FILTER_LIFECYCLE_OUT="$out" FILTER_LIFECYCLE_REPO="$repo"
printf '%s\n' preflight > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  FILTER_LIFECYCLE_EXIT="$code" python3 - <<'PYCOLLECT'
import hashlib,json,os
from pathlib import Path
out=Path(os.environ['FILTER_LIFECYCLE_OUT']);code=int(os.environ['FILTER_LIFECYCLE_EXIT'])
path=out/'summary.json';summary=json.loads(path.read_text()) if path.exists() else {}
complete=(summary.get('status')=='passed' and summary.get('total_pairs')==30 and
          summary.get('expected_total_pairs')==30 and summary.get('pgo_correctness_gates')==6 and
          summary.get('internal_gate_checks')==4 and
          summary.get('source_profile_binary_verified') is True and
          summary.get('root_feature_flags_isolated') is True)
status={'exit_code':code,'stage':(out/'stage.txt').read_text().strip(),'proof_complete':complete,
        'official_score':None,'automatic_adoption':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
if code or not complete:
    tails=[]
    for path in sorted((out/'logs').glob('*.log')):
        raw=path.read_bytes()
        tails.append({'path':path.relative_to(out).as_posix(),'bytes':len(raw),
                      'sha256':hashlib.sha256(raw).hexdigest(),'tail_utf8':raw[-12000:].decode('utf-8','replace')})
    (out/'failure-log-tails.json').write_text(json.dumps({'logs':tails,'full_logs_retained':True},indent=2)+'\n')
files=[]
for path in sorted(out.rglob('*')):
    if path.is_file() and 'source' not in path.relative_to(out).parts and path.name!='hashes.json':
        files.append({'path':path.relative_to(out).as_posix(),'bytes':path.stat().st_size,
                      'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
(out/'hashes.json').write_text(json.dumps({'artifacts':files,'excludes':['source/','hashes.json']},indent=2)+'\n')
for path in sorted(out.rglob('*')):
    if path.is_file() and 'source' not in path.relative_to(out).parts and path.suffix in {'.json','.txt'}:
        print('MINIVEC_FILTER_LIFECYCLE_BEGIN '+json.dumps(path.relative_to(out).as_posix()))
        print(path.read_text(encoding='utf-8',errors='replace'))
        print('MINIVEC_FILTER_LIFECYCLE_END')
if code==0 and not complete:
    raise SystemExit('Successful exit without complete isolated I17 lifecycle evidence')
PYCOLLECT
  local report_code=$?
  if [[ "$report_code" != 0 ]]; then
    echo "I17 lifecycle evidence collection failed: $report_code" >&2
    if [[ "$code" == 0 ]]; then code=$report_code;fi
    printf '{"exit_code":%d,"report_collection_exit_code":%d}\n' "$code" "$report_code" > "$out/report-collection-status.json"
  fi
  exit "$code"
}
trap finish EXIT
git diff --quiet HEAD -- Dockerfile .dockerignore minivec/src minivec/third_party minivec/tests minivec/tools protocol_check
git rev-parse HEAD > "$out/source-commit.txt"
git archive HEAD Dockerfile .dockerignore minivec/src minivec/third_party minivec/tests minivec/tools protocol_check | tar -x -C "$out/source"
python3 - <<'PYRUN'
import hashlib,json,os,platform,re,shlex,shutil,subprocess,sys,time
from pathlib import Path
out=Path(os.environ['FILTER_LIFECYCLE_OUT']);repo=Path(os.environ['FILTER_LIFECYCLE_REPO'])
source=out/'source';revision=(out/'source-commit.txt').read_text().strip()
target=repo/'filter-reuse-confirmation-results';feature='filter-reuse'
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
baseline_flags=['-DMVDB_DISABLE_MULTI_POSTING'];candidate_flags=['-DMVDB_MULTI_POSTING_REUSE_ONLY']
common_flags=shlex.split('-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc')
def require(test,message):
    if not test:raise RuntimeError(message)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def tree(path):return {p.relative_to(path).as_posix():sha(p) for p in sorted(path.rglob('*')) if p.is_file()}
def load(path):return json.loads(path.read_text())
def save(name,data):(out/name).write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')
def stage(name):
    (out/'stage.txt').write_text(name+'\n');print('FILTER_LIFECYCLE_STAGE '+name,flush=True)
require(platform.system()=='Linux','Requires Linux/Docker and two CPUs')
require(re.fullmatch('[0-9a-f]{40}',revision) is not None,'Invalid source commit')
require(compiler in (source/'Dockerfile').read_text(),'Production compiler image changed')
cpus=sorted(os.sched_getaffinity(0))[:2];require(len(cpus)==2,'Two available CPUs required')
inputs=tree(source)
save('context.json',{'source_commit':revision,'source_files_sha256':inputs,'compiler_image':compiler,
     'baseline_flags':baseline_flags,'candidate_flags':candidate_flags,'root_feature_flags_override':'',
     'expected_pairs':30,'seeds':[7319,8911,13337],'pairs_per_seed':5,'scenarios':['resident_compound_q1','resident_compound_q16'],
     'measurement_affinity':cpus,'original_pgo_training':True,'official_score':None,'automatic_adoption':False})
progress={'status':'running','steps':[],'unit_checks':[]};save('progress.json',progress)
def run(label,args,timeout,env=None):
    stage(label);record={'label':label,'argv':args,'status':'running','timeout_seconds':timeout,
       'stdout':'logs/'+label+'.stdout.log','stderr':'logs/'+label+'.stderr.log'}
    progress['steps'].append(record);save('progress.json',progress);started=time.monotonic()
    try:
        with (out/record['stdout']).open('wb') as stdout,(out/record['stderr']).open('wb') as stderr:
            result=subprocess.run(args,cwd=repo,env=env,stdout=stdout,stderr=stderr,timeout=timeout)
        record.update(returncode=result.returncode,status='passed' if result.returncode==0 else 'failed')
    except BaseException as error:
        record.update(returncode=None,status='failed',error=type(error).__name__+': '+str(error));raise
    finally:
        record['elapsed_seconds']=time.monotonic()-started;save('progress.json',progress)
    require(record['returncode']==0,label+' failed; retained logs contain details')
def copy_proofs():
    if target.is_dir():
        for path in target.glob('*.json'):shutil.copyfile(path,out/'proof'/path.name)
        for role in ('baseline-pgo','candidate-pgo'):
            source_flags=target/role/'training/effective-flags.txt'
            if source_flags.is_file():shutil.copyfile(source_flags,out/'proof'/(role+'-effective-flags.txt'))
try:
    # Diagnostics and allocation injection belong only to these internal-API
    # gates. The independently trained timing executables have no diagnostics.
    for test in ('test_filter_reuse','test_filter_paths'):
        for mode in ('normal','sanitized'):
            label=test+'-'+mode;binary=out/'unit'/label
            flags=['-O2'] if mode=='normal' else ['-O1','-g','-fno-omit-frame-pointer',
                   '-fsanitize=address,undefined','-fno-pie','-no-pie']
            feature_flags=['-DMVDB_MULTI_POSTING_REUSE_ONLY','-DMVDB_FILTER_DIAGNOSTICS']
            run('compile-'+label,['docker','run','--rm','--network','none','--platform','linux/amd64',
                '-v',str(source)+':/code:ro','-v',str(out/'unit')+':/out','-w','/code/minivec',
                compiler,'g++','-std=c++17','-pthread','-Isrc',*flags,*feature_flags,
                'tests/'+test+'.cpp',*['src/'+name+'.cpp' for name in ('db','exec','sql','value','flat_index')],
                '-o','/out/'+label],600)
            digest=sha(binary)
            run('run-'+label,['docker','run','--rm','--network','none','--platform','linux/amd64',
                '-e','ASAN_OPTIONS=detect_leaks=0:halt_on_error=1',
                '-e','UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1',
                '-v',str(out/'unit')+':/tests:ro',compiler,'/tests/'+label],240)
            require(sha(binary)==digest and b'PASS' in (out/'logs'/('run-'+label+'.stdout.log')).read_bytes(),
                    'Internal gate missing unchanged-binary/PASS proof')
            progress['unit_checks'].append({'test':test,'configuration':mode,'status':'passed',
                 'binary_sha256':digest,'test_source_sha256':sha(source/'minivec/tests'/(test+'.cpp')),
                 'feature_flags':feature_flags,'compile_flags':flags,'pgo':False})
            save('progress.json',progress)
    require(len(progress['unit_checks'])==4,'Incomplete internal gate checks')
    env={**os.environ,'CONFIRM_FEATURE':feature,'FEATURE_CORRECTNESS_ONLY':'1','PARSER_CONFIRM_O3':'0'}
    try:run('original-pgo-build-and-correctness',['bash',str(repo/'minivec/tools/ci_parser_confirmation.sh')],2400,env)
    finally:copy_proofs()
    ctx=load(target/'context.json');build=load(target/'build-input.json')
    confirmed=load(target/'summary.json');correct=load(target/'correctness.json');status=load(target/'run-status.json')
    require(ctx['source_commit']==revision==build['source_commit'] and ctx['feature']==build['feature']==feature,
            'Source/feature mismatch')
    require(ctx['baseline_flags']==build['baseline_flags']==baseline_flags and
            ctx['candidate_flags']==build['candidate_flags']==candidate_flags,'Role feature flags changed')
    require(ctx['root_feature_flags_override']==build['root_feature_flags_override']=='' and
            ctx['feature_flags_isolated'] is True and build['feature_flags_isolated'] is True,
            'Root default feature flags were not cleared')
    require(confirmed['status']=='passed' and confirmed['correctness_passed'] is True and
            confirmed['profile_source_verified'] is True and confirmed['binary_unchanged'] is True and
            confirmed['performance_measured'] is False and confirmed['database_pairs']==0 and status['exit_code']==0,
            'Incomplete PGO/correctness proof')
    require(build['engine_source']==tree(source/'minivec/src') and
            build['third_party']==tree(source/'minivec/third_party'),'Build did not use frozen source')
    require(build['training_source']=={name:sha(source/'minivec/tools/pgo'/name) for name in ('train.cpp','run-training.sh')},
            'Original training source changed')
    require(ctx['profile_generate_flags']==['-fprofile-generate=/pgo','-fprofile-update=atomic'] and
            ctx['profile_use_flags']==['-fprofile-use=/pgo','-Werror=missing-profile','-Werror=coverage-mismatch'],
            'Strict PGO generation/use flags changed')
    expected={(role,test) for role in ('baseline-pgo','candidate-pgo') for test in ('json_protocol','topk_serial','topk_parallel')}
    checks=correct['checks']
    require(correct['status']=='passed' and len(checks)==6 and
            {(x['variant'],x['test']) for x in checks}==expected and all(x['returncode']==0 for x in checks),
            'Missing actual PGO executable correctness gate')
    binaries={};profiles={};corpora={}
    for side,flags in (('baseline',baseline_flags),('candidate',candidate_flags)):
        role=side+'-pgo';folder=target/role;binary=folder/'minivec';item=ctx['profile_source_proof'][role]
        require(item['pgo'] is True and sha(binary)==item['binary_sha256'],'Executable SHA mismatch')
        require(item['source_sha256']==build['engine_source']==tree(folder/'built-source'),'Extracted source mismatch')
        require(tree(folder/'built-third-party')==build['third_party'],'Extracted third-party mismatch')
        require(shlex.split((folder/'training/effective-flags.txt').read_text())==item['effective_common_flags']==common_flags+flags and
                sha(folder/'training/effective-flags.txt')==item['effective_flags_sha256'],'Actual compiler feature flags differ')
        profiles[side]=tree(folder/'training/profile-data')
        require(profiles[side]==item['profile_sha256'] and profiles[side],'PGO profile bytes changed/missing')
        corpora[side]=tree(folder/'training/corpus')
        require(corpora[side]==item['training_corpus_sha256'],'Original training corpus proof mismatch')
        require(len(item['training_oracle'])==6 and all(x['status']=='passed' for x in item['training_oracle']),'Training oracle missing')
        binaries[side]={'path':str(binary),'sha256':sha(binary),'image_id':item['image_id']}
    require(corpora['baseline']==corpora['candidate'],'Different training corpus between sides')
    save('build-context.json',{'source_commit':revision,'feature':feature,'pgo_enabled':True,'same_compiler_and_training':True,
         'binaries':binaries,'compiler':ctx['compiler'],'baseline_flags':baseline_flags,'candidate_flags':candidate_flags,
         'root_feature_flags_override':'','feature_flags_isolated':True,'training_source':build['training_source'],
         'profile_generate_flags':ctx['profile_generate_flags'],'profile_use_flags':ctx['profile_use_flags'],
         'source_context':'proof/context.json','source_context_sha256':sha(target/'context.json')})
    progress['verified_pgo_binaries']=binaries;save('progress.json',progress)
    os.sched_setaffinity(0,cpus)
    run('resident-write-query-30pairs',[sys.executable,str(source/'minivec/tools/benchmark_write_query.py'),
        '--baseline',binaries['baseline']['path'],'--candidate',binaries['candidate']['path'],
        '--scenario','resident_compound_q1','--scenario','resident_compound_q16',
        '--pairs','5','--seeds','7319','8911','13337','--cycles','4',
        '--build-context',str(out/'build-context.json'),'--output',str(out/'write-query.json')],1800)
    report=load(out/'write-query.json');pairs=report['pairs']
    expected={(case,seed,index) for case in ('resident_compound_q1','resident_compound_q16')
              for seed in (7319,8911,13337) for index in range(5)}
    require(all(type(p['seed']) is int and type(p['pair_index']) is int for p in pairs),'Non-integer pair identity')
    ids=[(p['scenario'],p['seed'],p['pair_index']) for p in pairs]
    require(report['status']=='passed' and report['total_pairs']==report['expected_total_pairs']==30 and
            len(ids)==len(set(ids))==30 and set(ids)==expected,'Missing or duplicated pair identities')
    require(report['binary_unchanged'] is True and report['build_context']['sha256']==sha(out/'build-context.json'),
            'Benchmark binary/context binding missing')
    for pair in pairs:
        a,b=pair['baseline'],pair['candidate']
        reads=16 if pair['scenario'].endswith('q16') else 1
        require(pair['reads_per_write']==reads and pair['mutation_count']==4,'Wrong read/write plan')
        require(pair['all_responses_identical'] is True and re.fullmatch('[0-9a-f]{64}',a['response_sha256']) is not None and
                a['response_sha256']==b['response_sha256'],'Response equality proof missing')
        for side in (a,b):
            require(side['correctness']['all_query_responses_independent_oracle'] is True and
                    side['resident_storage_observed'] is True,'Resident/oracle proof missing')
            require([x['cycle'] for x in side['write_cycles']]==list(range(4)) and
                    all(len(x['mutations'])==1 for x in side['write_cycles']),
                    'Not four single-write cycles')
            query_phases={'query_before_mutations','query_after_restart',*[f'query_cycle_{i}' for i in range(4)]}
            require(set(side['query'])==query_phases and all(
                    q['count']==reads and len(q['per_request_seconds'])==reads and
                    q['all_queries_independent_oracle'] is True for q in side['query'].values()),
                    'Missing first/steady/restart query oracle or request count')
    for side,item in binaries.items():
        require(report['binaries'][side]['sha256']==item['sha256']==sha(Path(item['path'])),'Measured binary changed')
        require(profiles[side]==tree(target/(side+'-pgo')/'training/profile-data'),'PGO profiles changed during measurements')
    require(tree(source)==inputs,'Frozen source/tests/harness changed')
    require(subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()==revision,'Checkout commit changed')
    for name in ('ci_parser_confirmation.sh','ci_filter_lifecycle_confirmation.sh'):
        require(sha(repo/'minivec/tools'/name)==sha(source/'minivec/tools'/name),'Build entry changed during run')
    progress['status']='passed';save('progress.json',progress)
    save('summary.json',{'status':'passed','source_commit':revision,'pgo_correctness_gates':6,'internal_gate_checks':4,
         'total_pairs':30,'expected_total_pairs':30,'source_profile_binary_verified':True,
         'root_feature_flags_isolated':True,'binaries':binaries,'report':'write-query.json',
         'report_sha256':sha(out/'write-query.json'),'official_score':None,'automatic_adoption':False,
         'limitations':['Thirty paired runs cover only the two resident synthetic write/read patterns.',
                        'Four internal-API normal/sanitizer gates are separate diagnostic builds; PGO timings contain no counters.',
                        'Each side uses independently generated original PGO; profile/code layout can differ.',
                        'Process RSS/CPU/IO are proxies, not cgroup totals; no formal score is inferred.',
                        'First-query rebuild remains in cycle/lifecycle totals; post-restart verification is separate.',
                        'All source/profile/training response artifacts remain in filter-reuse-confirmation-results/.',
                        'No default switch or threshold is adopted automatically.']})
    stage('complete')
except BaseException as error:
    progress.update(status='failed',error=type(error).__name__+': '+str(error));save('progress.json',progress);raise
PYRUN
