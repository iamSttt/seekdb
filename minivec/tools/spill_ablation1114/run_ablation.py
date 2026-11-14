#!/usr/bin/env python3
"""Fixed 9ed-vs-spill ablation; neither retained-R identity nor a release gate."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback

HERE=Path(__file__).resolve().parent
VENDOR=HERE.parent/'research1114'
TOOLS=VENDOR/'minivec/tools'

def require(ok,message):
    if not ok:raise ValueError(message)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path):return json.loads(Path(path).read_bytes())
def save(path,value):
    temporary=path.with_name(path.name+'.tmp')
    temporary.write_text(json.dumps(value,indent=2,allow_nan=False)+'\n',encoding='utf8')
    temporary.replace(path)
def verify_inputs():
    provenance=read(HERE/'provenance.json')
    require(sha(HERE/'plan.json')==provenance['plan_sha256'],'frozen plan changed')
    require(sha(HERE/'guard_metrics.py')==provenance['guard_metrics_sha256'],'extracted reader changed')
    require(sha(VENDOR/'source-manifest.json')==provenance['vendor_manifest_sha256'],'vendor manifest changed')
    vendor=read(VENDOR/'source-manifest.json')
    for item in vendor['files']:require(sha(VENDOR/item['path'])==item['sha256'],'measurement vendor changed')
    for item in provenance['policies']:require(sha(HERE/item['path'])==item['sha256'],'original policy changed')
    return {'provenance':provenance,'measurement_vendor':vendor}

VERIFIED=verify_inputs()
PLAN=read(HERE/'plan.json')
sys.path.insert(0,str(VENDOR));sys.path.insert(0,str(TOOLS));sys.path.insert(0,str(HERE))
from guard_metrics import plan,observed_metrics
spec=importlib.util.spec_from_file_location('original_guard_rules',HERE/'policy/evaluate_highest_primary24_screen.py')
rules=importlib.util.module_from_spec(spec);spec.loader.exec_module(rules)

def numerical_guards(arms):
    require(set(arms)=={'1','2'},'both source-ablation CPU arms required')
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
            label=f'parent-source-ablation:{cpu}:{scenario}'
            groups[label]={}
            for name in times+memories:
                stats=rules.summarize_metric(pairs,name)
                groups[label][name]=stats
                threshold=1.05 if name in memories else 1.02
                if rules.stable_regression(stats,threshold,scenario!='standard10k'):
                    blocks.append(dict(group=label,metric=name,threshold=threshold))
                negative.extend(dict(group=label,metric=name,**row) for row in stats['pairs'] if row['candidate_worse'])
    return dict(numerical_ablation_guards_passed=not blocks,guard_triggers=blocks,
        guard_metrics=groups,all_guard_adverse_pairs=negative,
        status='ablation_regression_block' if blocks else 'ablation_complete_no_guard_trigger',
        not_measured=['first-request CPU','spill pure ingest CPU','wide process RSS'],
        full_24_pair_gate_passed=False,release_gate_passed=False,automatic_adoption=False,
        limitation='9ed parent-source versus spill only. No EE-primary or retained-R identity, independent confirmation, score or release inference.')

def identity(role,binary,evidence,directory):
    binary=binary.resolve();evidence=evidence.resolve();expected=PLAN['roles'][role]
    require(binary.is_file() and os.access(binary,os.X_OK),'executable missing: '+role)
    proof=read(evidence);digest=sha(binary)
    require(proof['status']=='verified_pinned_root_build' and proof['source_commit']==expected['source_commit'],
            'actual pinned root build proof missing: '+role)
    require(proof['binary_sha256']==digest==expected['expected_binary_sha256'],'actual ELF identity differs: '+role)
    require(proof['build_inputs']==expected['build_inputs'],'root build source inventory differs: '+role)
    item={'status':'verified','source_commit':expected['source_commit'],'binary_sha256':digest,
          'external_build_evidence':str(evidence),'external_build_evidence_sha256':sha(evidence),
          'scope':'Actual ELF from unchanged pinned root Docker context, with complete input hashes verified; not an official platform executable claim.'}
    path=directory/(role+'-identity.json');save(path,item)
    return dict(source_commit=expected['source_commit'],binary=str(binary),binary_sha256=digest,
        evidence_file=str(path),evidence_sha256=sha(path),evidence_binary_pointer=['binary_sha256'],
        evidence_commit_pointer=['source_commit'],environment={})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','baseline-evidence','candidate-evidence','launcher','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--plan-only',action='store_true');args=parser.parse_args()
    output=args.output.resolve();directory=output.with_suffix('.d')
    require(not output.exists() and not directory.exists(),'refuse overwrite')
    output.parent.mkdir(parents=True,exist_ok=True)
    expected_plan=[dict(cpu_count=cpu,**item) for cpu in (1,2) for item in plan(cpu)]
    require(expected_plan==PLAN['plan'] and len(expected_plan)==12,'schedule differs from frozen plan')
    report=dict(status='frozen_plan_not_executed' if args.plan_only else 'running',
        measurement_family=PLAN['family'],expected_pairs=12,completed_pairs=0,plan=expected_plan,
        baseline_source=PLAN['roles']['baseline']['source_commit'],candidate_source=PLAN['roles']['candidate']['source_commit'],
        raw_plan_scope=PLAN['underlying_selector'],frozen_plan_sha256=sha(HERE/'plan.json'),
        vendor_and_reader_provenance=VERIFIED,wrapper_sha256=sha(__file__),arms={},
        numerical_ablation_guards_passed=False,full_24_pair_gate_passed=False,release_gate_passed=False,
        automatic_adoption=False,official_score=None,formal_submission=False,
        scope='Parent9ed versus2bbe spill overlay, fixed12 pairs. Historical retained selector supplies schedule only; it does not establish R identity.')
    save(output,report)
    if args.plan_only:return
    try:
        require(sys.platform.startswith('linux'),'Linux cgroup-v2 required')
        directory.mkdir()
        ids={role:identity(role,getattr(args,role),getattr(args,role+'_evidence'),directory) for role in ('baseline','candidate')}
        ids_path=directory/'identities.json';save(ids_path,ids);report['identities']=ids
        launcher=args.launcher.resolve();require(launcher.is_file() and os.access(launcher,os.X_OK),'launcher unavailable')
        report['launcher_sha256']=sha(launcher)
        env={k:v for k,v in os.environ.items() if not k.startswith('MVDB_')};env['PYTHONDONTWRITEBYTECODE']='1'
        report['removed_MVDB_names']=sorted(k for k in os.environ if k.startswith('MVDB_'))
        for cpu in (1,2):
            raw_path=directory/f'ablation-{cpu}cpu.json'
            command=[sys.executable,'-B',str(TOOLS/'small_compute.py'),'--tools',str(TOOLS),
                '--identities',str(ids_path),'--launcher',str(launcher),'--output',str(raw_path),
                '--allow-version-contrast','--db-cpu-count',str(cpu),'--highest-primary-reference','retained',
                '--query-mode','exact','--memory-max','2147483648','--swap-max','0',
                '--cpu-quota-us',str(cpu*100000),'--cpu-period-us','100000','--case-timeout','600']
            arm=dict(command=command,raw_file=str(raw_path),status='running');report['arms'][str(cpu)]=arm;save(output,report)
            with (directory/f'ablation-{cpu}cpu.txt').open('wb') as log:
                code=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=False).returncode
            arm['exit_code']=code
            if raw_path.exists():
                raw=read(raw_path);arm.update(data=raw,status=raw.get('status'),raw_sha256=sha(raw_path))
                report['completed_pairs']+=len(raw.get('pairs',[]))
            save(output,report)
            require(code==0 and arm.get('status')=='passed','failed/incomplete CPU arm '+str(cpu))
            raw=arm['data']
            require(raw['measurement_family']=='highest_primary24_v1' and raw['highest_primary_reference']=='retained','wrong original schedule selector')
            require(raw['plan']==plan(cpu) and raw['expected_pairs']==raw['completed_pairs']==len(raw['pairs'])==6,'raw plan incomplete')
            require([{k:item[k] for k in ('scenario','seed','pair_index','order')} for item in raw['pairs']]==plan(cpu),'executed order differs')
            require(raw['query_mode_policy']=='exact' and raw['identity_record_sha256']==sha(ids_path),'mode/identity mismatch')
            require(raw['db_cpu_count']==cpu and raw['launcher_sha256']==sha(launcher),'CPU/launcher changed')
            require(raw['requested_limits']==dict(cpu_quota_us=cpu*100000,cpu_period_us=100000,memory_max='2147483648',swap_max='0'),'limits changed')
            require(all(pair['all_responses_identical'] is True for pair in raw['pairs']),'paired oracle/response gate missing')
        one,two=(report['arms'][str(cpu)]['data'] for cpu in (1,2))
        require(one['host']==two['host'] and one['taskset']==two['taskset'] and one['db_cpus']==two['db_cpus'][:1],'host/taskset/CPU assignment changed')
        require(one['workloads'][0]==two['workloads'][0],'default workload changed across CPU arms')
        require(report['completed_pairs']==12,'missing pairs')
        require(verify_inputs()==VERIFIED,'vendor/reader changed during run')
        report.update(numerical_guards(report['arms']))
    except BaseException as exc:
        report.update(status='failed_or_incomplete_ablation_not_passed',numerical_ablation_guards_passed=False,
            error=type(exc).__name__+': '+str(exc),traceback=traceback.format_exc())
        raise
    finally:save(output,report)

if __name__=='__main__':main()
