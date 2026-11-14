"""Exact historical metric extraction; no oracle or instrumentation changes."""
import math
from small_compute import counters, require

def standard_metrics(run):
    phases = run['phase_seconds']
    observations = run['adapter']['phase_cgroups']
    names = [p['phase'] for p in observations]
    expected = ['before_ingest', 'after_ingest', 'after_index_build', 'query_before_update',
                'before_shutdown', 'after_restart', 'query_after_restart', 'before_final_shutdown']
    require(names == expected, 'standard phase inventory/order differs')
    by_name = {p['phase']: p for p in observations}
    def cpu(start, end):
        value = counters(end['cpu.stat'])['usage_usec'] - counters(start['cpu.stat'])['usage_usec']
        require(value >= 0, 'negative CPU delta')
        return value
    parts = run['ingest_request_seconds']
    require(set(parts) == {'begin', 'insert', 'commit'} and
            all(math.isfinite(x) and x >= 0 for x in parts.values()) and
            math.isclose(sum(parts.values()), phases['ingest'], rel_tol=1e-12, abs_tol=1e-9),
            'ingest split does not reconcile')
    require(len(run['query']['query_before_update']['per_request_seconds']) == 1000,
            'default query count differs')
    resources = run['cgroup']['resource_delta']
    return {
        'first_lifecycle_wall_seconds': math.fsum(phases[k] for k in
            ('startup', 'create_table', 'ingest', 'index_build', 'query_before_update', 'shutdown')),
        'first_lifecycle_cpu_to_before_shutdown_usec': cpu(run['cgroup']['before'], by_name['before_shutdown']),
        'first_1000_queries_wall_seconds': phases['query_before_update'],
        'first_query_wall_seconds': run['query']['query_before_update']['per_request_seconds'][0],
        'max_process_rss_bytes': max(p['process_memory_proxy']['VmRSS_bytes'] for p in run['resource_snapshots']),
        'first_1000_queries_cpu_usec': cpu(by_name['after_index_build'], by_name['query_before_update']),
        'ingest_wall_seconds': phases['ingest'],
        'ingest_cpu_usec': cpu(by_name['before_ingest'], by_name['after_ingest']),
        **{'ingest_' + key + '_wall_seconds': value for key, value in parts.items()},
        'complete_lifecycle_cpu_usec': resources['cpu_stat_delta']['usage_usec'],
        'memory_peak_bytes': resources['memory_peak_bytes'],
        'device_write_bytes': sum(x.get('wbytes', 0) for x in resources['io_stat_delta'].values())}
