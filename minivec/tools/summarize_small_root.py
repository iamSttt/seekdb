#!/usr/bin/env python3
"""Fail-closed collection for the 12 + 8 pair actual-root-binary bridge."""
import argparse
import hashlib
import json
from pathlib import Path
from small_compute import CASES, PROTECTION_CASES, check_plan, cpu_set, require, sha


def verify_measurement(data, count, cases, identity_sha, identities):
    require(data.get('status') == 'passed' and data.get('expected_pairs') == len(cases) * 2 and
            data.get('completed_pairs') == len(cases) * 2 and len(data['pairs']) == len(cases) * 2,
            'missing or failed plan')
    require(data['comparison_kind'] == 'whole_version_diagnostic', 'expected explicit version contrast')
    require(data['identity_record_sha256'] == identity_sha, 'identity record changed')
    require(data['db_cpu_count'] == count and len(set(data['db_cpus'])) == count and
            data['protection_only'] == (count == 2), 'DB affinity/plan differs')
    require(data['requested_limits']['cpu_quota_us'] == count * 100000 and
            data['requested_limits']['cpu_period_us'] == 100000, 'quota/period differs')
    check_plan([{key: p[key] for key in ('scenario', 'seed', 'pair_index', 'order')}
                for p in data['pairs']], cases)
    for role in ('baseline', 'candidate'):
        for key in ('binary_sha256', 'source_commit', 'evidence_sha256', 'environment'):
            require(data['identities'][role][key] == identities[role][key], 'measurement identity differs')
        require(data['identities'][role]['environment'] == {}, 'runtime override in version contrast')
    for pair in data['pairs']:
        require(pair.get('all_responses_identical') is True, 'response comparison failed')
        for role in ('baseline', 'candidate'):
            run = pair[role]; group = run['cgroup']
            require(run['declared_environment'] == {}, 'run environment differs')
            require(not group['timeout'] and not group.get('request_timeout_observed') and not group.get('cleanup_kill_required') and
                    not group.get('watchdog_error') and not any(group['oom_events_delta'].values()),
                    'resource failure or process cleanup')
            require(group['before']['cpu.max'].split() == [str(count * 100000), '100000'], 'actual quota differs')
            require(group['after']['cpu.max'].split() == [str(count * 100000), '100000'], 'final quota differs')
            require(not group['after']['cgroup.procs'].strip(), 'DB survives measurement')
            ready = run['adapter']['ready_memberships']
            require(len(ready) == (1 if pair['scenario'] == 'wide768' else 2), 'missing actual ready observations')
            for item in ready:
                require(cpu_set(item['cpus_allowed_list']) == set(data['db_cpus']), 'actual affinity differs')
                require(item['pid'] in item['members'] and item['driver_pid'] not in item['members'], 'driver not isolated')
                require(item['actual_exe'] == data['identities'][role]['binary'], 'actual executable differs')
            phases = run['adapter']['phase_cgroups']
            require(len(phases) == (14 if pair['scenario'] == 'wide768' else
                                    15 if pair['scenario'].startswith('spill_') else 7), 'phase telemetry incomplete')
            for phase in phases:
                require(not phase.get('task_read_errors') and not phase.get('native_tasks_error'), 'task telemetry incomplete')
                require(phase['native_tasks'] and sum(t['main'] for t in phase['native_tasks']) == 1, 'main task absent')
                require(all(cpu_set(t['cpus_allowed_list']) == set(data['db_cpus']) for t in phase['native_tasks']),
                        'worker or main affinity differs')


def collect(out, reports, image, binary, exit_code):
    result = {'status': 'failed_or_incomplete', 'exit_code': exit_code, 'expected_pairs': 20,
        'completed_pairs': 0, 'comparison_kind': 'whole_version_diagnostic',
        'new_pgo_builds': 1, 'formal_submission': False, 'official_score': None, 'automatic_adoption': False}
    try:
        proof = json.loads((out / 'programs/root-proof.json').read_bytes())
        ids_path = out / 'programs/identities.json'
        identities = json.loads(ids_path.read_bytes())
        require(proof['status'] == 'verified' and proof['image_id'] == image and
                proof['binary_sha256'] == sha(binary) == sha(out / 'programs/actual-image-minivec'), 'actual candidate changed')
        for role, item in identities.items():
            require(sha(out / 'programs' / item['binary']) == item['binary_sha256'], 'binary changed')
            require(sha(out / 'programs' / item['evidence_file']) == item['evidence_sha256'], 'proof changed')
        result.update(candidate_image_id=image, candidate_binary_sha256=proof['binary_sha256'],
                      candidate_source_commit=proof['source_commit'])
        for count, cases in ((1, CASES), (2, PROTECTION_CASES)):
            data = json.loads((out / f'measurements-{count}cpu.json').read_bytes())
            verify_measurement(data, count, cases, sha(ids_path), identities)
            expected_tools = {'minivec/tools/small_compute.py', 'minivec/tools/benchmark_iteration.py',
                'minivec/tools/benchmark_index_delta.py', 'minivec/tools/benchmark_write_query.py',
                'minivec/tools/benchmark_projection.py', 'minivec/tests/json_protocol.py',
                'protocol_check/_transport.py', 'protocol_check/config.json'}
            observed_tools = {}
            for path, measured_sha in data['tool_hashes'].items():
                relative = Path(path).relative_to(reports.parent).as_posix()
                require(relative not in observed_tools, 'duplicate measured tool')
                require(measured_sha == proof['harness_file_sha256'][relative] == sha(Path(path)),
                        'measured harness differs from Git-verified inputs')
                observed_tools[relative] = measured_sha
            require(set(observed_tools) == expected_tools, 'measured tool inventory differs')
            result['completed_pairs'] += len(data['pairs'])
        require(exit_code == 0, 'measurement process failed')
        result['status'] = 'passed'
    except Exception as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')
    files = [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'hashes.json']
    (out / 'hashes.json').write_text(json.dumps({'artifacts': files}, indent=2) + '\n', encoding='utf8')
    root = json.loads((reports / 'root-gates-summary.json').read_bytes())
    root['passed'] = root.get('passed') is True and result['status'] == 'passed'
    root['checks']['actual_root_small_compute_20_pairs'] = result['status'] == 'passed'
    root['small_compute'] = result
    if result['status'] == 'passed':
        root['limitations'] = [x for x in root['limitations'] if x != 'No new paired performance measurement.']
        root['limitations'].append('Only 12 one-CPU and 8 two-CPU pairs, one seed; no formal score claim or automatic adoption.')
    (reports / 'verification-summary.json').write_text(json.dumps(root, indent=2) + '\n', encoding='utf8')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('output', 'reports', 'binary'): p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--image', required=True); p.add_argument('--exit-code', type=int, required=True)
    args = p.parse_args()
    result = collect(args.output, args.reports, args.image, args.binary, args.exit_code)
    print(json.dumps(result))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__': main()
