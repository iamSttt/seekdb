"""Fixed standalone proof checks and 3x4 paired micro plan; never a DB/PGO run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
from statistics import median
import subprocess
import sys

SCENARIOS=("fullmantissa128","fullmantissa512","grid128")
CANONICAL_SHA="7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9"

def require(value,message):
    if not value:raise RuntimeError(message)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def shadow_header(text):
    namespace="namespace distance_detail {\n"
    text=text.replace(namespace,namespace+"inline uint64_t diagnostic_calls=0,diagnostic_old_cert=0,diagnostic_tight_rescue=0,diagnostic_integer=0;\n",1)
    begin=text.index("inline double distance_scalar(");end=text.index("// Conservative rejection",begin)
    body=text[begin:end]
    body=body.replace("  if(n<=uint64_t(1)<<32) {","  ++distance_detail::diagnostic_calls;\n  if(n<=uint64_t(1)<<32) {",1)
    old="if(sum-lower>error && upper-sum>error)return result;"
    require(body.count(old)==1,"shadow old-certificate anchor differs")
    body=body.replace(old,"if(sum-lower>error && upper-sum>error){++distance_detail::diagnostic_old_cert;return result;}")
    tight="if(sum-lower>tighter_error && upper-sum>tighter_error)return result;"
    body=body.replace(tight,"if(sum-lower>tighter_error && upper-sum>tighter_error){++distance_detail::diagnostic_tight_rescue;return result;}")
    require(body.count("  return distance_exact(a,b,n);")==1,"shadow fallback anchor differs")
    body=body.replace("  return distance_exact(a,b,n);","  ++distance_detail::diagnostic_integer;\n  return distance_exact(a,b,n);")
    return text[:begin]+body+text[end:]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
    root=Path(__file__).resolve().parent;out=args.output.resolve()
    require(out!=root and root not in out.parents,"output must be outside immutable draft directory")
    out.mkdir(parents=True,exist_ok=False)
    result={'status':'running','scope':'standalone non-PGO x87 scalar distance; not a database benchmark',
            'database_runs':0,'pgo_builds':0,'native_compilation_attempts':0,'native_compilations_completed':0,
            'native_execution_attempts':0,'native_executions_completed':0,'commands':[],
            'plan':{'scenarios':list(SCENARIOS),'pairs_per_scenario':4,'orders':['AB','BA','AB','BA'],
                    'rows':512,'repetitions_per_arm':64,'counter_instrumented_timing':False},
            'limitations':['Micro timings include whole scalar call and guard, not query/index/storage behavior.',
                           'Same source/compiler host build without PGO differs from actual root binaries.',
                           'Shadow counts are not timed; counters cannot be converted to CPU share.',
                           'Unsupported FP-control regressions compare legacy behavior only, not a new correctness claim.',
                           'No official score implication.']}
    def save():(out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    def run(name,command,env=None):
        entry={'name':name,'argv':list(map(str,command))};result['commands'].append(entry);save()
        if name.startswith('build-'):result['native_compilation_attempts']+=1
        is_native=name in ('normal','sanitized','diagnostic') or name.startswith('micro-')
        if is_native:result['native_execution_attempts']+=1
        save()
        try:p=subprocess.run(entry['argv'],cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
        except subprocess.TimeoutExpired as error:
            text=error.stdout or b'';text=text.decode(errors='replace') if isinstance(text,bytes) else text
            (out/(name+'.log')).write_text(text);entry['status']='timeout';save();raise
        (out/(name+'.log')).write_text(p.stdout);entry['exit_code']=p.returncode;save()
        require(p.returncode==0,name+' failed/unsupported; original log retained')
        if name.startswith('build-'):result['native_compilations_completed']+=1
        if is_native:result['native_executions_completed']+=1
        save();return p.stdout
    save()
    try:
        require(platform.system()=='Linux' and platform.machine()=='x86_64','Linux x86_64 only')
        result['source_sha256']={p.relative_to(root).as_posix():sha(p) for p in sorted(root.rglob('*')) if p.is_file() and '__pycache__' not in p.parts}
        proof=json.loads((root/'source-proof.json').read_bytes());require(sha(root/'reference_distance.h')==proof['reference_sha256']==CANONICAL_SHA,'canonical reference differs')
        require(sha(root/'candidate_distance.h')==proof['candidate_sha256'],'candidate source differs');result['source_proof']=proof
        allowed=sorted(os.sched_getaffinity(0));require(bool(allowed),'empty affinity');os.sched_setaffinity(0,{allowed[0]})
        result['affinity']={'before':allowed,'selected':sorted(os.sched_getaffinity(0))};require(result['affinity']['selected']==[allowed[0]],'pin failed')
        for name,path in {'proc-status':'/proc/self/status','proc-cgroup':'/proc/self/cgroup','cpu-max':'/sys/fs/cgroup/cpu.max','memory-max':'/sys/fs/cgroup/memory.max','memory-swap-max':'/sys/fs/cgroup/memory.swap.max'}.items():
            file=Path(path)
            if file.is_file():(out/(name+'.txt')).write_bytes(file.read_bytes())
            else:result.setdefault('unavailable_resource_files',[]).append(path)
        result['compiler']=run('compiler',['g++','--version']).splitlines()[0]
        require(run('compiler-version',['g++','-dumpfullversion']).strip()=='13.3.0','use fixed gcc13.3 toolchain; do not silently change compiler')
        run('cpu',['lscpu'])
        run('fixture-generation',[sys.executable,str(root/'make_fixtures.py'),'--output',str(out/'oracle.bin')])
        oracle=json.loads((out/'oracle.json').read_bytes());require(sha(out/'oracle.bin')==oracle['sha256'],'oracle hash differs');result['oracle']=oracle
        flags=['g++','-std=c++17','-fno-math-errno','-pthread']
        run('build-normal',flags+['-O3',str(root/'test_tighter_bound.cpp'),'-o',str(out/'normal')])
        normal=run('normal',[str(out/'normal'),str(out/'oracle.bin')])
        run('build-sanitized',flags+['-O1','-g','-fno-omit-frame-pointer','-fsanitize=address,undefined','-no-pie',str(root/'test_tighter_bound.cpp'),'-o',str(out/'sanitized')])
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        sanitized=run('sanitized',[str(out/'sanitized'),str(out/'oracle.bin')],env)
        counts=[]
        for log in (normal,sanitized):
            require('tighter_bound=PASS' in log.splitlines(),'missing pass marker')
            items=re.findall(r'^tighter_bound_checks=(\d+)$',log,re.M);require(len(items)==1 and int(items[0])>0,'bad check count');counts.append(int(items[0]))
        require(counts[0]==counts[1],'normal/sanitized count differs');result['correctness_checks']={'normal':counts[0],'sanitized':counts[1]}
        # The only counter-instrumented source/binary lives in a separate directory.
        shadow=out/'shadow';shadow.mkdir()
        for old,new in (('reference_distance.h','reference_shadow.h'),('candidate_distance.h','candidate_shadow.h')):
            (shadow/new).write_text(shadow_header((root/old).read_text()))
        comparison=(root/'comparison.h').read_text().replace('reference_distance.h','reference_shadow.h').replace('candidate_distance.h','candidate_shadow.h')
        (shadow/'shadow_comparison.h').write_text(comparison)
        run('build-diagnostic',flags+['-O3','-I'+str(shadow),str(root/'diagnostic_counts.cpp'),'-o',str(out/'diagnostic')])
        diagnostic=run('diagnostic',[str(out/'diagnostic')])
        result['diagnostic']=[json.loads(line) for line in diagnostic.splitlines()]
        require([x['scenario'] for x in result['diagnostic']]==list(SCENARIOS)+['analytic_midpoint_rescue_not_timed'],'diagnostic case inventory')
        for row in result['diagnostic']:
            old,new=row['old'],row['candidate'];require(old['calls']==new['calls'] and old['old_cert']==new['old_cert'] and old['tight_rescue']==0 and old['integer']==new['tight_rescue']+new['integer'],'counter conservation')
        run('build-micro',flags+['-O3',str(root/'benchmark_tighter_bound.cpp'),'-o',str(out/'micro')])
        require('diagnostic_' not in (root/'reference_distance.h').read_text()+(root/'candidate_distance.h').read_text(),'timing source instrumented')
        pairs=[]
        # Rotate scenarios between pair indices; every case keeps fixed AB/BA balance.
        for index in range(4):
            for scenario in SCENARIOS:
                order='AB' if index%2==0 else 'BA'
                row=json.loads(run(f'micro-{scenario}-{index}',[str(out/'micro'),scenario,order]))
                require(row['scenario']==scenario and row['order']==order and row['rows']==512 and row['repetitions']==64 and row['counter_instrumented'] is False,'micro plan differs')
                measures=row['measurements'];require([m['role'] for m in measures]==['old','candidate'] and measures[0]['checksum']==measures[1]['checksum'],'micro role/checksum mismatch')
                require(all(m['wall_ns']>0 and m['thread_cpu_ns']>0 for m in measures),'invalid clock delta')
                row['pair']=index;pairs.append(row)
        require(len(pairs)==12,'missing pairs');result['pairs']=pairs
        groups={}
        for scenario in SCENARIOS:
            rows=[p for p in pairs if p['scenario']==scenario];require(len({p['dataset_fnv64'] for p in rows})==1,'dataset drift')
            groups[scenario]={}
            for metric in ('wall_ns','thread_cpu_ns'):
                ratios=[p['measurements'][1][metric]/p['measurements'][0][metric] for p in rows]
                groups[scenario][metric]={'candidate_over_old':ratios,'median':median(ratios),'candidate_worse_pairs':sum(r>1 for r in ratios)}
        result['groups']=groups;result['binary_sha256']={name:sha(out/name) for name in ('normal','sanitized','diagnostic','micro')}
        result['shadow_source_sha256']={p.name:sha(p) for p in shadow.iterdir()}
        result['status']='correctness_and_micro_completed';save()
    except Exception as exc:
        result.update(status='failed_or_unsupported',error=str(exc));save();raise

if __name__=='__main__':main()
