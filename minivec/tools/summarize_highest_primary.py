#!/usr/bin/env python3
"""Collect a EE-primary 24-pair screen; performance is never automatic adoption."""
import argparse
import json
import math
from pathlib import Path
from statistics import median
from small_compute import counters, highest_primary_plan, HIGHEST_PRIMARY_FAMILY, require, sha
from prepare_highest_primary import verify_root_gates, verify_batch8_policy
from summarize_small_root import verify_measurement

TOOLS = {'minivec/tools/small_compute.py', 'minivec/tools/benchmark_iteration.py',
         'minivec/tools/benchmark_index_delta.py', 'minivec/tools/benchmark_write_query.py',
         'minivec/tools/benchmark_projection.py', 'minivec/tests/json_protocol.py',
         'protocol_check/_transport.py', 'protocol_check/config.json'}
R = 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
EE = '9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd'


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


def paired_standard(data):
    metrics = {}
    for pair in data['pairs']:
        if pair['scenario'] != 'standard10k': continue
        b, c = (standard_metrics(pair[role]) for role in ('baseline', 'candidate'))
        require(set(b) == set(c), 'metric inventory differs')
        for name in b:
            require(math.isfinite(b[name]) and math.isfinite(c[name]) and min(b[name], c[name]) >= 0,
                    'invalid standard metric')
            metrics.setdefault(name, []).append({'pair_index': pair['pair_index'], 'order': pair['order'],
                'baseline': b[name], 'candidate': c[name],
                'baseline_over_candidate': b[name] / c[name] if c[name] else None,
                'candidate_over_baseline': c[name] / b[name] if b[name] else None,
                'candidate_greater': c[name] > b[name]})
    out = {}
    for name, rows in metrics.items():
        ratios = [r['candidate_over_baseline'] for r in rows if r['candidate_over_baseline'] is not None]
        out[name] = {'raw_pairs': rows, 'median_candidate_over_baseline': median(ratios) if ratios else None,
                     'candidate_greater_pairs': sum(r['candidate_greater'] for r in rows)}
    return {'metrics': out, 'decision': 'pending_preregistered_independent_review'}


def collect(out, reports, image, binary, exit_code):
    result = {'status': 'failed_or_incomplete', 'exit_code': exit_code, 'expected_pairs': 24,
        'completed_pairs': 0, 'measurement_family': HIGHEST_PRIMARY_FAMILY, 'primary_reference': 'ee', 'new_pgo_builds': 1, 'rebuilt_reference_programs': 0,
        'formal_submission': False, 'official_score': None, 'automatic_adoption': False,
        'standard_comparisons': {}, 'protection_summaries': {}}
    try:
        programs = out / 'programs'
        proof = json.loads((programs / 'root-proof.json').read_bytes())
        gates_path = reports / 'root-gates-summary.json'
        gates = json.loads(gates_path.read_bytes())
        require(proof['status'] == 'verified' and proof['image_id'] == image and
                proof['binary_sha256'] == sha(binary) == sha(programs / 'actual-image-minivec'), 'actual root changed')
        quantizer = verify_root_gates(reports)
        require(proof['root_gate_summary_sha256'] == sha(gates_path), 'root gate binding differs')
        result.update(candidate_source_commit=proof['source_commit'], candidate_image_id=image,
                      candidate_binary_sha256=proof['binary_sha256'])
        policy=verify_batch8_policy(reports.parent)
        require(proof['root_input_sha256'].get(policy['path'])==policy['sha256'], 'policy omitted from root input proof')
        result['acceptance_policy']=policy
        binding = json.loads((programs / 'highest-primary-binding-proof.json').read_bytes())
        require(binding['status'] == 'verified' and binding.get('experiment_id')=='batch8-tighter-certificate-highest-primary24' and binding.get('acceptance_policy')==policy and
                binding['measurement_family'] == HIGHEST_PRIMARY_FAMILY and
                binding['primary_reference'] == 'ee' and binding['quantizer_gate'] == quantizer and binding['current_root_proof_sha256'] == sha(programs / 'root-proof.json') and
                binding['ee_import_proof_sha256'] == sha(programs / 'ee/proof.json') and
                binding['candidate_source_commit'] == proof['source_commit'] and
                binding['candidate_binary_sha256'] == proof['binary_sha256'] and binding['candidate_image_id'] == image and
                binding['new_ee_builds'] == binding['new_pgo_builds'] == 0, 'reference import binding differs')
        workloads, cpus, host_binding = {}, {}, None
        candidate_identity = None
        for reference in ('ee', 'retained'):
            ids_path = programs / (reference + '-identities.json')
            ids = json.loads(ids_path.read_bytes())
            require(binding[reference + '_identity_sha256'] == sha(ids_path), 'imported identity changed')
            require(ids['baseline']['binary_sha256'] == (R if reference == 'retained' else EE), 'reference differs')
            require(ids['candidate']['binary_sha256'] == proof['binary_sha256'] and
                    ids['candidate']['source_commit'] == proof['source_commit'], 'candidate root identity differs')
            require(candidate_identity is None or ids['candidate'] == candidate_identity, 'two contrasts use different candidate')
            candidate_identity = ids['candidate']
            for item in ids.values():
                require(sha(programs / item['binary']) == item['binary_sha256'] and
                        sha(programs / item['evidence_file']) == item['evidence_sha256'], 'program/proof changed')
            for count in (1, 2):
                data = json.loads((out / f'{reference}-{count}cpu.json').read_bytes())
                cases = tuple(dict.fromkeys(p['scenario'] for p in highest_primary_plan(reference, count)))
                verify_measurement(data, count, cases, sha(ids_path), ids, highest_reference=reference)
                require(data['plan'] == highest_primary_plan(reference, count), 'recorded plan differs')
                require(data['launcher_sha256'] == sha(out / 'cgroup_exec'), 'launcher changed')
                measured_host = (data['host'], data['taskset'], data['launcher_sha256'])
                require(host_binding is None or measured_host == host_binding, 'contrasts use different host/launcher/taskset')
                host_binding = measured_host
                require(count not in cpus or cpus[count] == data['db_cpus'], 'contrasts use different CPU sets')
                cpus[count] = data['db_cpus']
                actual_tools = {}
                for path, digest in data['tool_hashes'].items():
                    name = Path(path).relative_to(reports.parent).as_posix()
                    require(name not in actual_tools and digest == proof['harness_file_sha256'][name] == sha(Path(path)),
                            'measured harness changed')
                    actual_tools[name] = digest
                require(set(actual_tools) == TOOLS, 'tool inventory incomplete')
                require([w['scenario'] for w in data['workloads']] == list(cases), 'workload inventory differs')
                for workload in data['workloads']:
                    name = workload['scenario']
                    require(name not in workloads or workloads[name] == workload, 'shared scenario identity differs')
                    workloads[name] = workload
                label = f'{reference}-{count}cpu'
                result['standard_comparisons'][label] = paired_standard(data)
                result['protection_summaries'][label] = {k: v for k, v in data['summary'].items() if k != 'standard10k'}
                result['completed_pairs'] += len(data['pairs'])
        require(result['completed_pairs'] == 24 and exit_code == 0, 'screen incomplete')
        require(cpus[1] == cpus[2][:1], 'one/two CPU plans do not share the first allowed CPU')
        result['status'] = 'passed'
        result['adoption'] = 'not_adopted_pending_preregistered_independent_review'
    except Exception as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
    result['limitations'] = [
        'One fixed seed; 6 primary EE and 4 retained default pairs per CPU, 2 retained protection pairs per selected case. Screening, not a statistical floor or formal-score guarantee.',
        'First lifecycle wall is the sum of startup/create/ingest/index/first 1000 queries/shutdown request timers; excludes telemetry/oracle gaps and restart. Corresponding CPU ends before shutdown, so it is separately labelled.',
        'Ingest CPU includes BEGIN/INSERT/COMMIT and engine work between stage snapshots. Split request wall timers isolate those request classes, not executor-copy cost alone.',
        'Cgroup totals include restart verification; after_restart to query_after_restart CPU must not be described as pure query CPU.',
        'Device writes are shared-host cgroup accounting, not dedicated-device sync latency. Preserve every pair and COMMIT; no outlier deletion.',
        'EE executable is imported from the verified previous root build. Original Actions artifact omitted ee-context/.dockerignore; import transparently verifies that single missing member against the immutable Git-proven source archive. No raw archive was altered.',
        'Sampled process RSS is the maximum observed VmRSS across phase snapshots, not a process high-water mark or official memory score. First-query CPU is not sampled and is not inferred.',
        'No cross-host or cross-reference ratio multiplication, no averaging ratios across workloads, no spill gain offsets for default regressions. Mixed material results require review, not adoption.']
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')
    files = [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'hashes.json']
    (out / 'hashes.json').write_text(json.dumps({'artifacts': files}, indent=2) + '\n', encoding='utf8')
    root = json.loads((reports / 'root-gates-summary.json').read_bytes())
    root['passed'] = root.get('passed') is True and result['status'] == 'passed'
    root['checks']['actual_root_highest_primary_24_pairs'] = result['status'] == 'passed'
    root['highest_primary_screen'] = result
    if result['status'] == 'passed':
        root['limitations'] = [x for x in root['limitations'] if x != 'No new paired performance measurement.']
        root['limitations'].extend(result['limitations'])
    (reports / 'verification-summary.json').write_text(json.dumps(root, indent=2) + '\n', encoding='utf8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('output', 'reports', 'binary'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--exit-code', type=int, required=True)
    args = parser.parse_args()
    result = collect(args.output, args.reports, args.image, args.binary, args.exit_code)
    print(json.dumps({'status': result['status'], 'completed_pairs': result['completed_pairs'], 'error': result.get('error')}))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__': main()
