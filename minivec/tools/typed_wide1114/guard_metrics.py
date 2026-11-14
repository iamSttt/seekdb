import math
from metrics import standard_metrics
from small_compute import counters

LABELS={'narrow_large4096', 'wide_all', 'narrow_all', 'narrow_top10', 'wide_large4096', 'wide_top10', 'wide_distance10'}

def require(value,message):
    if not value:raise ValueError(message)

def plan(cpu):
    return [{'scenario':scenario,'seed':7319,'pair_index':i,
             'order':['baseline','candidate'] if i%2==0 else ['candidate','baseline']}
            for scenario,n in [('standard10k',4),('spill_high_q16' if cpu==1 else 'wide768',2)] for i in range(n)]


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

