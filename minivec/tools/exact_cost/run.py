#!/usr/bin/env python3
"""One serial R/diagnostic first-cycle oracle comparison, not a speed experiment."""
import argparse, hashlib, json, os, pathlib, platform, shutil, sys, time, types, uuid
HERE=pathlib.Path(__file__).resolve().parent
METRICS={'scalar_distance':0,'long_double_certificate':256,'integer_exact':256,'int8_bound':256,
  'float_bound_context':256,'double_bound':256,'sql_parse':1,'crc_update':16}
EVENTS={'long_double_certified','scalar_to_integer','integer_nonfinite','int8_invalid_or_disabled',
  'int8_cap_unusable','int8_kernel_calls','int8_rejected','float_invalid_or_disabled',
  'float_kernel_calls','float_rejected','double_rejected'}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def need(ok,message):
    if not ok:raise ValueError(message)
def save(path,data):path.write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')
def parent_group_name():return 'minivec-report-exact-cost-'+uuid.uuid4().hex

def ready_evidence(session,binary,group,cpus,environment,small):
    pid=session.proc.pid;p=pathlib.Path('/proc')/str(pid)
    members={int(v) for v in (group/'cgroup.procs').read_text().split()}
    need(pid in members and os.getpid() not in members,'DB and driver isolation differs')
    need((p/'exe').resolve()==binary.resolve(),'actual executable differs')
    env={}
    for item in (p/'environ').read_bytes().split(b'\0'):
        k,sep,v=item.partition(b'=');key=k.decode(errors='replace')
        if sep and key in small.ENV_KEYS:env[key]=v.decode()
    need(env==environment,'actual process runtime knobs differ')
    tasks=small.task_snapshot(pid)
    need(tasks['native_tasks'] and not tasks.get('task_read_errors') and not tasks.get('native_tasks_error'),'incomplete task evidence')
    need(len(tasks['native_tasks'])==1 and tasks['native_tasks'][0]['main'] and tasks['native_tasks'][0]['tid']==pid,'diagnostic requires exactly one main native task')
    for task in tasks['native_tasks']:
        need(small.cpu_set(task['cpus_allowed_list'])==set(cpus),'native task affinity differs')
    return {'pid':pid,'exe':str((p/'exe').resolve()),'binary_sha256':sha(p/'exe'),
            'runtime_environment':env,'members':sorted(members),'tasks':tasks}

def first_cycle(binary,work,report,out,group,prefix,base,small,environment,cpus):
    config=json.loads((base.PROTOCOL_CHECK/'config.json').read_text())
    config.update(request_timeout=90,startup_timeout=90)
    stderr=out/(report['role']+'-stderr.log')
    session=base.Session(config,stderr,command=prefix+[str(binary),'--data','{data_dir}','--protocol','jsonl'],case_timeout=600)
    requests=report['requests']=[];digest=hashlib.sha256()
    def snapshot(label):
        tasks=small.task_snapshot(session.proc.pid)
        need(tasks['native_tasks'] and not tasks.get('task_read_errors') and not tasks.get('native_tasks_error'),'incomplete phase task evidence')
        need(len(tasks['native_tasks'])==1 and tasks['native_tasks'][0]['main'] and tasks['native_tasks'][0]['tid']==session.proc.pid,'phase has non-main task')
        need(all(small.cpu_set(t['cpus_allowed_list'])==set(cpus) for t in tasks['native_tasks']),'phase task affinity differs')
        caps=small.snapshot(group)
        need(caps['cpu.max']=='100000 100000' and caps['memory.max']=='2147483648' and caps['memory.swap.max']=='0','phase resource caps differ')
        members={int(v) for v in caps['cgroup.procs'].split()}
        need(session.proc.pid in members and os.getpid() not in members,'phase DB/driver cgroup membership differs')
        return {'phase':label,'cgroup':caps,'tasks':tasks,
                'process':base.process_snapshot(session,label)}
    def request(sql,phase,affected=None,index=None,op='execute'):
        t=time.perf_counter();response=session.request(sql,mode='exact',op=op);elapsed=time.perf_counter()-t
        need(response.get('ok') is True,phase+' SQL error '+str(response)[:200])
        if affected is not None:need(response.get('affected_rows')==affected,'affected_rows differs')
        normalized=base.encoded({k:v for k,v in response.items() if k!='id'})
        digest.update(normalized)
        requests.append({'sequence':session.sequence,'phase':phase,'query_index':index,'wall_seconds':elapsed,
          'response_sha256':hashlib.sha256(normalized).hexdigest(),'search_mode':'exact'})
        return response
    try:
        session.start()
        report['ready']=ready_evidence(session,binary,group,cpus,environment,small)
        request(work['create'],'create')
        report['phase_snapshots']=[snapshot('before_ingest')]
        for sql,count in work['inserts']:
            request('BEGIN;','begin');request(sql,'insert',count);request('COMMIT;','commit')
        report['phase_snapshots'].append(snapshot('after_ingest'))
        request('CREATE VECTOR INDEX docs_v ON docs(v);','index')
        report['phase_snapshots'].append(snapshot('after_index'))
        for index,sql in enumerate(work['queries']):
            response=request(sql,'filtered_query' if index%2 else 'unfiltered_query',index=index)
            need(len(response.get('rows',[]))==work['query_counts'][index],'query count differs')
            if index in work['before_oracle']:base.verify_query(response,work['before_oracle'][index],f'query {index}')
        report['phase_snapshots'].append(snapshot('after_query'))
        # The established transport checks the shutdown reply, exit code and extra stdout.
        t=time.perf_counter();session.shutdown()
        requests.append({'sequence':session.sequence,'phase':'shutdown','query_index':None,
          'wall_seconds':time.perf_counter()-t,'response_sha256':None,'search_mode':'exact'})
        report['response_sha256']=digest.hexdigest()
        report['sampled_oracle_indices']=work['oracle_indices']
        report['all_query_responses_compared_to_retained']=False
    finally:
        report['protocol_process_returncode']=session.proc.poll() if session.proc is not None else None
        session.close();report['stderr_sha256']=sha(stderr);report['stderr_bytes']=stderr.stat().st_size

def summarize_diagnostic(records,requests):
    need(len(records)==len(requests),'diagnostic request count differs')
    output={}
    for record,request in zip(records,requests):
        need(record['sequence']==request['sequence'],'diagnostic sequence differs')
        events=record['events'];metrics=record['metrics']
        need(set(events)==EVENTS and set(metrics)==set(METRICS),'diagnostic metric inventory differs')
        need(type(record['thread_cpu_ns']) is int and record['thread_cpu_ns']>=0,'invalid request CPU')
        need(all(type(v) is int and v>=0 for v in events.values()),'invalid event count')
        for name,metric in metrics.items():
            need(set(metric)=={'calls','units','samples','sample_cpu_ns','sample_stride'},'metric fields differ')
            need(all(type(v) is int and v>=0 for v in metric.values()),'invalid metric count')
            need(metric['sample_stride']==METRICS[name] and metric['samples']<=metric['calls'],'sampling declaration/count differs')
            need(metric['samples']>0 or metric['sample_cpu_ns']==0,'unsampled metric has CPU time')
        need(metrics['scalar_distance']['samples']==0,'outer scalar must be count-only')
        calls=lambda k:metrics[k]['calls']
        need(calls('scalar_distance')==events['long_double_certified']+events['scalar_to_integer'],'scalar outcomes do not sum')
        need(calls('integer_exact')==events['scalar_to_integer'],'integer fallback count differs')
        need(calls('int8_bound')==events['int8_invalid_or_disabled']+events['int8_cap_unusable']+events['int8_kernel_calls'],'int8 branches do not sum')
        need(events['int8_rejected']<=events['int8_kernel_calls'],'int8 rejects exceed kernels')
        need(events['float_rejected']<=events['float_kernel_calls'],'float rejects exceed kernels')
        need(calls('float_bound_context')==events['float_invalid_or_disabled']+events['float_kernel_calls'],'unexpected float branch on finite default workload')
        need(calls('double_bound')==0 and events['integer_nonfinite']==0,'unexpected finite default fallback')
        phase=output.setdefault(request['phase'],{'requests':0,'thread_cpu_ns':0,'metrics':{},'events':{}})
        phase['requests']+=1;phase['thread_cpu_ns']+=record['thread_cpu_ns']
        for name,value in events.items():phase['events'][name]=phase['events'].get(name,0)+value
        for name,metric in metrics.items():
            target=phase['metrics'].setdefault(name,{'calls':0,'units':0,'samples':0,'sample_cpu_ns':0,'sample_stride':metric['sample_stride']})
            for key in ('calls','units','samples','sample_cpu_ns'):target[key]+=metric[key]
    for phase in output.values():
        for metric in phase['metrics'].values():
            n=metric['samples'];metric['raw_mean_sample_cpu_ns']=metric['sample_cpu_ns']/n if n else None
            # A deterministic periodic sample is not unbiased. Extrapolation is
            # displayed only as a rough diagnostic and must never be summed.
            metric['rough_inclusive_cpu_ns_not_additive']=(metric['sample_cpu_ns']/n*metric['calls']) if n else None
    return output

def main():
    p=argparse.ArgumentParser();p.add_argument('--repo',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True)
    args=p.parse_args();repo=args.repo.resolve();out=args.output.resolve();programs=out/'programs'
    sys.path.insert(0,str(repo/'minivec/tools'))
    import benchmark_iteration as base
    import small_compute as small
    proof=json.loads((programs/'source-proof.json').read_text())
    require_env={'MVDB_QUERY_THREADS':'1','MVDB_PARSE_THREADS':'1'}
    launcher=(out/'cgroup_exec').resolve();taskset=pathlib.Path(shutil.which('taskset')).resolve()
    allowed=sorted(os.sched_getaffinity(0));need(allowed,'empty affinity');cpus=[allowed[0]]
    settings=types.SimpleNamespace(cpu_quota_us=100000,cpu_period_us=100000,memory_max='2147483648',swap_max='0',case_timeout=600)
    report={'status':'running','scope':'diagnostic-only, non-PGO shadow source; no performance adoption',
      'runtime_source_commit':proof['runtime_source_commit'],'source_proof_sha256':sha(programs/'source-proof.json'),
      'host':{'platform':platform.platform(),'driver_affinity':allowed},'db_cpus':cpus,
      'requested_limits':vars(settings),'runtime_environment':require_env,'runs':[],
      'tool_sha256':{str(x):sha(x) for x in (pathlib.Path(base.__file__),pathlib.Path(small.__file__),
        base.PROTOCOL_CHECK/'_transport.py',base.PROTOCOL_CHECK/'config.json',launcher,taskset,*HERE.glob('*.py'),*HERE.glob('*.h'),*HERE.glob('*.cpp'))},
      'limitations':['No new PGO training; this diagnostic ELF has different code/layout than actual retained R.',
        'Forced parse/query=1 measures a serial mechanism; it does not identify automatic-policy costs.',
        'CLOCK_THREAD_CPUTIME_ID samples are inclusive/nested, deterministic periodic, and perturbed. No overhead subtraction or additive CPU claim.',
        'First lifecycle only, no restart/update. Only three independent TopK oracle queries; every response compared to actual retained R.',
        'The 1CPU/2GiB/swap0 envelope is a diagnostic configuration, not a verified official configuration.',
        'Startup, worker calibration, fprintf diagnostics and post-return stdout handling are not all covered by request-scope CPU timers.']}
    path=out/'measurements.json';save(path,report)
    work=base.make_workload(base.FULL_CASES['standard10k'],7319,1000,100,10,query_mode='exact')
    report['workload']={'scenario':'standard10k','rows':10000,'dimension':128,'queries':1000,'batch_size':100,'k':10,
      'seed':7319,'query_mode':'exact','sql_sha256':work['sha256'],'half_equality_filtered':True}
    parent=small.CGROOT/parent_group_name()
    try:
        parent.mkdir();need(not (parent/'cgroup.procs').read_text().strip(),'parent populated')
        (parent/'cgroup.subtree_control').write_text('+cpu +memory +io')
        for role,name in [('retained','retained-minivec'),('diagnostic','diagnostic-minivec')]:
            binary=(programs/name).resolve();before=sha(binary)
            if role=='retained':need(before==proof['retained_binary_sha256'],'retained executable changed')
            run={'role':role,'binary_sha256':before,'cgroup':{}};report['runs'].append(run);save(path,report)
            with small.environment(require_env),small.Group(parent,settings,run['cgroup']) as group:
                need(run['cgroup']['before']['cpu.max']=='100000 100000','actual cpu quota differs')
                # Preserve all inherited constraints; reject a tighter ancestor envelope.
                for ancestor in run['cgroup']['ancestor_constraints']:
                    for key,limit in [('memory.max',2147483648),('memory.swap.max',0)]:
                        value=ancestor.get(key)
                        if isinstance(value,dict):
                            need(ancestor['path']==str(small.CGROOT) and set(value)=={'unavailable'},'non-root ancestor limit unreadable')
                            continue
                        need(value in (None,'max') or int(value)>=limit,'ancestor cap tighter than requested')
                    quota=ancestor.get('cpu.max','max 100000')
                    if isinstance(quota,dict):
                        need(ancestor['path']==str(small.CGROOT) and set(quota)=={'unavailable'},'non-root ancestor quota unreadable')
                        continue
                    quota=quota.split()
                    need(quota[0]=='max' or int(quota[0])>=int(quota[1]),'ancestor CPU below 1')
                prefix=[str(launcher),str(group.path),str(taskset),'--cpu-list',str(cpus[0])]
                first_cycle(binary,work,run,out,group.path,prefix,base,small,require_env,cpus)
            need(sha(binary)==before,'binary changed during measurement')
            need(not run['cgroup']['timeout'] and not run['cgroup'].get('cleanup_kill_required') and
                 not run['cgroup'].get('request_timeout_observed') and not run['cgroup'].get('watchdog_error') and
                 not any(run['cgroup']['oom_events_delta'].values()),'timeout/OOM/unclean process exit')
            for label in ('before','immediately_after','after'):
                caps=run['cgroup'][label]
                need(caps['cpu.max']=='100000 100000' and caps['memory.max']=='2147483648' and caps['memory.swap.max']=='0','final resource cap mismatch')
            save(path,report)
        a,b=report['runs']
        need(a['response_sha256']==b['response_sha256'],'response transcript mismatch')
        need([x['response_sha256'] for x in a['requests']]==[x['response_sha256'] for x in b['requests']],'per-request response differs')
        for run in report['runs']:run['all_query_responses_compared_to_retained']=True
        log=(out/'diagnostic-stderr.log').read_text()
        records=[json.loads(line[len('MVDB_COST_DIAG '):]) for line in log.splitlines() if line.startswith('MVDB_COST_DIAG ')]
        report['diagnostic_summary']=summarize_diagnostic(records,b['requests'])
        save(out/'diagnostic-records.json',records)
        report['timer_empty_overhead']=json.loads((out/'timer-overhead.json').read_text())
        report['status']='passed'
    except BaseException as exc:
        report['status']='failed';report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        save(path,report)
        if parent.exists():parent.rmdir()
    print(json.dumps({'status':report['status'],'requests_per_role':len(report['runs'][0]['requests']),
      'runtime_source_commit':report['runtime_source_commit'],'diagnostic_only':True}))
if __name__=='__main__':main()
