#!/usr/bin/env python3
"""Fixed retained-R protection only; no release/EE/full-24-pair pass claim."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import traceback

HERE=Path(__file__).resolve().parent
VENDOR=HERE/'vendor'
TOOLS=VENDOR/'minivec/tools'
R='33a52a7dd4426af059ec2334b7c93a07c38c93c9'
R_SHA='acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
LABELS={'narrow_top10','wide_top10','narrow_large4096','wide_large4096','narrow_all','wide_all','wide_distance10'}

def require(value,message):
    if not value:raise ValueError(message)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path):return json.loads(Path(path).read_bytes())
def save(path,data):
    temporary=path.with_name(path.name+'.tmp')
    temporary.write_text(json.dumps(data,indent=2,allow_nan=False)+'\n',encoding='utf8')
    temporary.replace(path)
def plan(cpu):
    return [{'scenario':scenario,'seed':7319,'pair_index':i,
             'order':['baseline','candidate'] if i%2==0 else ['candidate','baseline']}
            for scenario,n in [('standard10k',4),('spill_high_q16' if cpu==1 else 'wide768',2)] for i in range(n)]
def verify_inputs():
    prepared=read(HERE/'prepared.json')
    require(sha(VENDOR/'source-manifest.json')==prepared['vendor_manifest_sha256'],'vendor manifest changed')
    provenance=read(VENDOR/'source-manifest.json')
    for item in provenance['files']:
        require(sha(VENDOR/item['path'])==item['sha256'],'vendor bytes changed: '+item['path'])
    for item in prepared['historical_policy']:
        require(sha(HERE/Path(item['file'].replace('\\','/')))==item['sha256'],'policy bytes changed')
    require(sha(HERE/'reference-inputs.json')==prepared['reference_manifest_sha256'],'reference manifest changed')
    return provenance

verify_inputs()  # Validate bytes before importing historical executable modules.
sys.path.insert(0,str(VENDOR))
sys.path.insert(0,str(TOOLS))
from metrics import standard_metrics
from small_compute import counters
spec=importlib.util.spec_from_file_location('historical_guard_rules',HERE/'evidence/evaluate_highest_primary24_screen.py')
rules=importlib.util.module_from_spec(spec);spec.loader.exec_module(rules)

def observed_metrics(run,scenario):
    if scenario=='standard10k':return standard_metrics(run)
    snapshots=run['adapter']['phase_cgroups']
    by_name={p['phase']:p for p in snapshots}
    require(len(by_name)==len(snapshots),'duplicate phase snapshot')
    def cpu(start,end):
        result=counters(by_name[end]['cpu.stat'])['usage_usec']-counters(by_name[start]['cpu.stat'])['usage_usec']
        require(result>=0,'negative phase CPU');return result
    resource=run['cgroup']['resource_delta']
    result={'complete_lifecycle_cpu_usec':resource['cpu_stat_delta']['usage_usec'],
            'memory_peak_bytes':resource['memory_peak_bytes']}
    if scenario=='spill_high_q16':
        names=['after_ingest','after_index_build','query_before_mutations']
        for i in range(4):names.extend([f'writes_cycle_{i}',f'query_cycle_{i}'])
        names.extend(['before_shutdown','after_restart','query_after_restart','before_final_shutdown'])
        require(list(by_name)==names,'spill phase order/inventory differs')
        phases=run['phase_seconds']
        excluded={'restart','restart_verification_wall','query_after_restart','final_shutdown','measured_lifecycle'}
        require(excluded.issubset(phases) and {'startup','shutdown'}.issubset(phases),'spill lifecycle boundary absent')
        first=math.fsum(value for name,value in phases.items() if name not in excluded)
        restart=math.fsum(phases[name] for name in ('restart','query_after_restart','final_shutdown'))
        require(math.isclose(first+restart,phases['measured_lifecycle'],rel_tol=1e-12,abs_tol=1e-12),'spill lifecycle does not reconcile')
        samples=run['query']['query_before_mutations']['per_request_seconds']
        require(len(samples)==16,'spill initial phase must contain sixteen queries')
        result.update(first_lifecycle_wall_seconds=first,
            query_before_update_wall_seconds=phases['query_before_mutations'],
            query_before_update_cpu_usec=cpu('after_index_build','query_before_mutations'),
            first_query_wall_seconds=samples[0],ingest_wall_seconds=phases['ingest'],
            max_process_rss_bytes=max(x['process_memory_proxy']['VmRSS_bytes'] for x in run['resource_snapshots']))
    elif scenario=='wide768':
        require(set(run['phases'])==LABELS,'wide seven-phase inventory differs')
        require(set(by_name)=={label+'/'+end for label in LABELS for end in ('before','after')},'wide CPU phase inventory differs')
        require(set(run['lifecycle_seconds'])=={'startup','create_and_insert','shutdown'},'wide lifecycle inventory differs')
        query_wall=math.fsum(p['wall_seconds'] for p in run['phases'].values())
        result.update(lifecycle_stage_wall_sum=math.fsum(run['lifecycle_seconds'].values())+query_wall,
            seven_query_phases_wall_seconds=query_wall,
            seven_query_phases_cpu_usec=sum(cpu(label+'/before',label+'/after') for label in LABELS))
    else:raise ValueError('unplanned scenario')
    require(all(type(x) in (int,float) and math.isfinite(x) and x>=0 for x in result.values()),'invalid derived metric')
    return result

def numerical_guards(arms):
    require(set(arms)=={'1','2'},'both retained CPU arms required')
    groups={};blocks=[];negative=[]
    for cpu in (1,2):
        data=arms[str(cpu)]['data']
        require(data['status']=='passed' and data['plan']==plan(cpu) and len(data['pairs'])==6,'incomplete arm')
        for scenario in ('standard10k','spill_high_q16' if cpu==1 else 'wide768'):
            pairs=[]
            for raw in data['pairs']:
                if raw['scenario']!=scenario:continue
                sides={role:observed_metrics(raw[role],scenario) for role in ('baseline','candidate')}
                require(set(sides['baseline'])==set(sides['candidate']),'metric inventories differ')
                pairs.append({'pair_index':raw['pair_index'],'order':raw['order'],
                    'metrics':{name:{role:sides[role][name] for role in sides} for name in sides['baseline']}})
            require(len(pairs)==(4 if scenario=='standard10k' else 2),'guard pair count differs')
            times,memories=rules.guard_metric_names(scenario)
            label=f'retained:{cpu}:{scenario}'
            groups[label]={}
            for name in times+memories:
                stats=rules.summarize_metric(pairs,name)  # Rejects missing/nonfinite/zero denominator.
                groups[label][name]=stats
                threshold=1.05 if name in memories else 1.02
                if rules.stable_regression(stats,threshold,scenario!='standard10k'):
                    blocks.append(dict(group=label,metric=name,threshold=threshold))
                negative.extend(dict(group=label,metric=name,**row) for row in stats['pairs'] if row['candidate_worse'])
    return dict(numerical_retained_guards_passed=not blocks,guard_triggers=blocks,
                guard_metrics=groups,all_guard_adverse_pairs=negative,
                status='retained_regression_block' if blocks else 'retained_guards_complete_no_trigger',
                not_measured=['first-request CPU','spill pure ingest CPU','wide process RSS'],
                full_24_pair_gate_passed=False,release_gate_passed=False,automatic_adoption=False,
                limitation='R protection only. No EE primary or independent confirmation is supplied by this report.')

def identity(role,binary,commit,evidence,directory):
    binary=binary.resolve();evidence=evidence.resolve()
    require(binary.is_file() and os.access(binary,os.X_OK),'executable missing: '+role)
    proof=read(evidence);digest=sha(binary)
    if role=='baseline':
        require(proof.get('status')=='verified_current_root_build' and proof['source_commit']==R and
                proof['binary_sha256']==digest==R_SHA,'current R build proof missing/mismatched')
        require(proof['source_sha256']==read(HERE/'reference-inputs.json')['source_sha256'],'R build source inventory differs')
    else:
        declared=proof.get('source_commit',proof.get('runtime_commit'))
        measured=proof.get('binary_sha256',proof.get('image_binary_sha256'))
        require(declared==commit and measured==digest,'candidate build evidence mismatch')
    item={'status':'verified','source_commit':commit,'binary_sha256':digest,
          'external_evidence':str(evidence),'external_evidence_sha256':sha(evidence),
          'scope':'File/source identity binding only; does not independently certify full correctness gate inventory.'}
    path=directory/(role+'-identity.json');save(path,item)
    return dict(source_commit=commit,binary=str(binary),binary_sha256=digest,evidence_file=str(path),
        evidence_sha256=sha(path),evidence_binary_pointer=['binary_sha256'],evidence_commit_pointer=['source_commit'],environment={})

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','baseline-build-proof','candidate-evidence','launcher','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--candidate-source',required=True)
    p.add_argument('--plan-only',action='store_true')
    args=p.parse_args();require(re.fullmatch('[0-9a-f]{40}',args.candidate_source),'full candidate commit required')
    output=args.output.resolve();directory=output.with_suffix('.d')
    require(not output.exists() and not directory.exists(),'refuse overwrite')
    output.parent.mkdir(parents=True,exist_ok=True)
    report=dict(status='prepared_not_executed' if args.plan_only else 'running',expected_pairs=12,completed_pairs=0,
        vendor_provenance=verify_inputs(),wrapper_sha256=sha(__file__),reference_source=R,reference_binary_sha256=R_SHA,
        candidate_source=args.candidate_source,plan=[dict(cpu_count=c,**item) for c in (1,2) for item in plan(c)],
        arms={},numerical_retained_guards_passed=False,full_24_pair_gate_passed=False,release_gate_passed=False,
        automatic_adoption=False,official_score=None,formal_submission=False,
        scope='Original retained portion only: default4 per CPU, spill2 on CPU1, wide2 on CPU2; no missing guard passes.')
    save(output,report)
    if args.plan_only:return
    try:
        require(sys.platform.startswith('linux'),'Linux cgroup-v2 required')
        directory.mkdir()
        ids={'baseline':identity('baseline',args.baseline,R,args.baseline_build_proof,directory),
             'candidate':identity('candidate',args.candidate,args.candidate_source,args.candidate_evidence,directory)}
        ids_path=directory/'identities.json';save(ids_path,ids)
        report['identities']=ids
        launcher=args.launcher.resolve();require(launcher.is_file() and os.access(launcher,os.X_OK),'launcher unavailable')
        report['launcher_sha256']=sha(launcher)
        env={k:v for k,v in os.environ.items() if not k.startswith('MVDB_')}
        env['PYTHONDONTWRITEBYTECODE']='1'
        report['removed_MVDB_names']=sorted(k for k in os.environ if k.startswith('MVDB_'))
        for cpu in (1,2):
            raw_path=directory/f'retained-{cpu}cpu.json'
            command=[sys.executable,'-B',str(TOOLS/'small_compute.py'),'--tools',str(TOOLS),
                '--identities',str(ids_path),'--launcher',str(launcher),'--output',str(raw_path),
                '--allow-version-contrast','--db-cpu-count',str(cpu),'--highest-primary-reference','retained',
                '--query-mode','exact','--memory-max','2147483648','--swap-max','0',
                '--cpu-quota-us',str(cpu*100000),'--cpu-period-us','100000','--case-timeout','600']
            arm=dict(command=command,raw_file=str(raw_path),status='running');report['arms'][str(cpu)]=arm;save(output,report)
            with (directory/f'retained-{cpu}cpu.log').open('wb') as log:
                rc=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=False).returncode
            arm['exit_code']=rc
            if raw_path.exists():
                raw=read(raw_path);arm.update(data=raw,status=raw.get('status'),raw_sha256=sha(raw_path))
                report['completed_pairs']+=len(raw.get('pairs',[]))
            save(output,report)
            require(rc==0 and arm.get('status')=='passed','failed/incomplete CPU arm '+str(cpu))
            raw=arm['data']
            require(raw['measurement_family']=='highest_primary24_v1' and raw['highest_primary_reference']=='retained','wrong helper selector')
            require(raw['plan']==plan(cpu) and raw['expected_pairs']==raw['completed_pairs']==len(raw['pairs'])==6,'raw plan incomplete')
            require([{k:x[k] for k in ('scenario','seed','pair_index','order')} for x in raw['pairs']]==plan(cpu),'executed order differs')
            require(raw['query_mode_policy']=='exact' and raw['identity_record_sha256']==sha(ids_path),'mode/identity mismatch')
            require(raw['db_cpu_count']==cpu and raw['launcher_sha256']==sha(launcher),'CPU/launcher changed')
            require(raw['requested_limits']==dict(cpu_quota_us=cpu*100000,cpu_period_us=100000,memory_max='2147483648',swap_max='0'),'limits changed')
            require(all(pair['all_responses_identical'] is True for pair in raw['pairs']),'paired oracle/response gate missing')
        one,two=(report['arms'][str(cpu)]['data'] for cpu in (1,2))
        require(one['host']==two['host'] and one['taskset']==two['taskset'] and one['db_cpus']==two['db_cpus'][:1],'host/taskset/CPU assignment changed')
        require(one['workloads'][0]==two['workloads'][0],'default workload changed across CPU arms')
        require(report['completed_pairs']==12,'missing required pairs')
        require(verify_inputs()==report['vendor_provenance'],'vendor changed during run')
        report.update(numerical_guards(report['arms']))
    except BaseException as exc:
        report.update(status='failed_or_incomplete_guards_not_passed',numerical_retained_guards_passed=False,
                      error=type(exc).__name__+': '+str(exc),traceback=traceback.format_exc())
        raise
    finally:save(output,report)
if __name__=='__main__':main()
