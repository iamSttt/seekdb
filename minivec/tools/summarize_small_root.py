#!/usr/bin/env python3
"""Fail-closed collection for the 12 + 8 pair actual-root-binary bridge."""
import argparse
import hashlib
import json
from pathlib import Path
from small_compute import CASES, PROTECTION_CASES, check_plan, check_insert_copy_plan, insert_copy_plan, cpu_set, require, sha


def verify_measurement(data, count, cases, identity_sha, identities, insert_reference=None, thread_control=False):
    require(not (insert_reference and thread_control), 'conflicting measurement plans')
    if thread_control:
        from small_compute import thread_control_plan, check_thread_control_plan, thread_control_environment
        require(count == 1 and cases == ('standard10k',), 'thread diagnostic must use one CPU and standard10k')
    expected = 8 if thread_control else len(insert_copy_plan(insert_reference, count)) if insert_reference else len(cases) * 2
    require(data.get('insert_thread_control', False) == thread_control, 'thread diagnostic plan differs')
    require(data.get('insert_copy_reference') == insert_reference, 'measurement plan family differs')
    require(data.get('status') == 'passed' and data.get('expected_pairs') == expected and
            data.get('completed_pairs') == expected and len(data['pairs']) == expected,
            'missing or failed plan')
    require(data['comparison_kind'] == 'whole_version_diagnostic', 'expected explicit version contrast')
    require(data['identity_record_sha256'] == identity_sha, 'identity record changed')
    require(data['db_cpu_count'] == count and len(set(data['db_cpus'])) == count and
            data['protection_only'] == (count == 2 and not insert_reference), 'DB affinity/plan differs')
    require(data['requested_limits']['cpu_quota_us'] == count * 100000 and
            data['requested_limits']['cpu_period_us'] == 100000, 'quota/period differs')
    require(data['query_mode_policy'] == 'exact' and data['requested_limits']['memory_max'] == '2147483648' and
            data['requested_limits']['swap_max'] == '0', 'root diagnostic mode/memory plan differs')
    measured_plan = [{key: p[key] for key in ('scenario', 'seed', 'pair_index', 'order') + (('policy',) if thread_control else ())} for p in data['pairs']]
    if thread_control: check_thread_control_plan(measured_plan)
    elif insert_reference: check_insert_copy_plan(measured_plan, insert_reference, count)
    else: check_plan(measured_plan, cases)
    for role in ('baseline', 'candidate'):
        for key in ('binary_sha256', 'source_commit', 'evidence_sha256', 'environment'):
            require(data['identities'][role][key] == identities[role][key], 'measurement identity differs')
        require(data['identities'][role]['environment'] == {}, 'runtime override in version contrast')
    for pair in data['pairs']:
        require(pair.get('all_responses_identical') is True, 'response comparison failed')
        for role in ('baseline', 'candidate'):
            run = pair[role]; group = run['cgroup']
            expected_environment = thread_control_environment(pair['policy']) if thread_control else {}
            require(run['declared_environment'] == expected_environment, 'run environment differs')
            if thread_control:
                require(run['effective_environment'] == expected_environment and
                        run['thread_control_policy'] == pair['policy'], 'effective thread policy differs')
            require(run['query_mode_policy'] == 'exact', 'actual run mode policy differs')
            modes = run['adapter']['completed_request_modes']
            require(set(modes) == {'exact'} and modes['exact'] > 0 and
                    modes['exact'] == run['adapter']['completed_transport_requests'], 'forwarded protocol modes differ')
            if pair['scenario'] == 'wide768':
                require(len(run['phases']) == 7 and all(len(phase['requests']) == 3 and
                    all(item['search_mode'] == 'exact' for item in phase['requests']) for phase in run['phases'].values()),
                    'projection request modes differ')
            else:
                require(run['query'] and all(phase['modes'] and len(phase['modes']) == len(phase['per_request_seconds']) and
                    all(mode == 'exact' for mode in phase['modes']) for phase in run['query'].values()),
                    'query phase modes differ')
            require(not group['timeout'] and not group.get('request_timeout_observed') and not group.get('cleanup_kill_required') and
                    not group.get('watchdog_error') and not any(group['oom_events_delta'].values()),
                    'resource failure or process cleanup')
            require(group['before']['cpu.max'].split() == [str(count * 100000), '100000'], 'actual quota differs')
            require(group['after']['cpu.max'].split() == [str(count * 100000), '100000'], 'final quota differs')
            for observed in (group['before'], group['after']):
                require(observed['memory.max'] == '2147483648' and observed['memory.swap.max'] == '0', 'actual memory/swap cap differs')
            for ancestor in group['ancestor_constraints']:
                cap = ancestor['memory.max']
                require(not isinstance(cap, str) or not cap.isdigit() or int(cap) >= 2147483648,
                        'ancestor constrains memory below declared diagnostic limit')
            require(not group['after']['cgroup.procs'].strip(), 'DB survives measurement')
            ready = run['adapter']['ready_memberships']
            require(len(ready) == (1 if pair['scenario'] == 'wide768' else 2), 'missing actual ready observations')
            for item in ready:
                require(cpu_set(item['cpus_allowed_list']) == set(data['db_cpus']), 'actual affinity differs')
                require(item['pid'] in item['members'] and item['driver_pid'] not in item['members'], 'driver not isolated')
                require(item['actual_exe'] == data['identities'][role]['binary'], 'actual executable differs')
                if thread_control:
                    require(item['runtime_environment'] == expected_environment, 'actual process thread environment differs')
            phases = run['adapter']['phase_cgroups']
            require(len(phases) == (14 if pair['scenario'] == 'wide768' else
                                    15 if pair['scenario'].startswith('spill_') else 8 if insert_reference or thread_control else 7), 'phase telemetry incomplete')
            for phase in phases:
                require(phase['memory.max'] == '2147483648' and phase['memory.swap.max'] == '0', 'phase memory/swap cap differs')
                require(not phase.get('task_read_errors') and not phase.get('native_tasks_error'), 'task telemetry incomplete')
                require(phase['native_tasks'] and sum(t['main'] for t in phase['native_tasks']) == 1, 'main task absent')
                require(all(cpu_set(t['cpus_allowed_list']) == set(data['db_cpus']) for t in phase['native_tasks']),
                        'worker or main affinity differs')


def verify_compatibility(directory, identities, proof, image):
    summary = json.loads((directory / 'summary.json').read_bytes())
    data = json.loads((directory / 'persistence.json').read_bytes())
    identity = json.loads((directory / 'identity.json').read_bytes())
    require(summary['status'] == data['status'] == 'passed' and summary['exit_code'] == 0 and
            summary['compatibility_checks'] == len(data['checks']) == 18, 'CRC compatibility failed or incomplete')
    actual = [(x['kind'], x.get('mode'), x.get('file'), x.get('producer'), x.get('consumer')) for x in data['checks']]
    expected = [('identical_durable_bytes', mode, name, None, None) for mode, names in
        (('wal', ('data.log',)), ('snapshot', ('data.log', 'snapshot.bin', 'snapshot.lsn'))) for name in names]
    expected += [('old_new_interchange', mode, None, producer, consumer) for mode in ('wal', 'snapshot')
                 for producer in ('baseline', 'candidate') for consumer in ('baseline', 'candidate')]
    expected += [(kind, mode, None, None, None) for mode in ('wal', 'snapshot') for kind in ('bad_committed_crc', 'torn_tail')]
    expected += [('bad_snapshot_crc_rejected', None, None, None, consumer) for consumer in ('baseline', 'candidate')]
    require(len(set(actual)) == 18 and set(actual) == set(expected), 'CRC compatibility case inventory differs')
    for item in data['checks']:
        if item['kind'] in ('bad_committed_crc', 'torn_tail'):
            require(item['expected_visible_ids'] == ([1, 2, 7, 15] if item['kind'] == 'bad_committed_crc' else [1, 2, 7, 15, 40]),
                    'CRC corruption oracle differs')
    for role in ('baseline', 'candidate'):
        require(data['binaries'][role]['sha256'] == summary['actual_reader_binary_sha256'][role] ==
                identity[role + '_binary_sha256'] == identities[role]['binary_sha256'], 'CRC actual reader differs')
    require(summary['candidate_image_id'] == identity['candidate_image_id'] == image and
            summary['candidate_source_commit'] == identity['candidate_source_commit'] == proof['source_commit'],
            'CRC root image/source differs')
    for path, digest in identity['harness_sha256'].items():
        require(proof['harness_file_sha256'][path] == digest, 'CRC harness differs from Git proof')
    manifest = json.loads((directory / 'hashes.json').read_bytes())['artifacts']
    observed = {}
    for item in manifest:
        target = (directory / item['path']).resolve()
        require(target.is_relative_to(directory.resolve()) and item['path'] not in observed and target.is_file(),
                'unsafe, duplicate or missing CRC artifact')
        require(target.stat().st_size == item['bytes'] and sha(target) == item['sha256'], 'CRC artifact changed')
        observed[item['path']] = item['sha256']
    require(set(observed) == {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file() and p.name != 'hashes.json'},
            'CRC artifact inventory differs')
    return {'status': 'passed', 'checks': 18, 'summary_sha256': sha(directory / 'summary.json'),
            'manifest_sha256': sha(directory / 'hashes.json'), 'actual_reader_binary_sha256': summary['actual_reader_binary_sha256']}


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
        result['crc_compatibility'] = verify_compatibility(reports / 'crc-checks/compat', identities, proof, image)
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
    root['checks']['actual_crc_old_new_18_checks'] = result.get('crc_compatibility', {}).get('status') == 'passed'
    root['small_compute'] = result
    if result['status'] == 'passed':
        root['limitations'] = [x for x in root['limitations'] if x != 'No new paired performance measurement.']
        root['limitations'].append('Only 12 one-CPU and 8 two-CPU exact-only pairs with 2 GiB local memory cap and swap disabled, one seed. Diagnostic settings, not a verified official envelope; no score claim or automatic adoption.')
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
