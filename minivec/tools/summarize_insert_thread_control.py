#!/usr/bin/env python3
"""Fail-closed eight-pair R/N thread diagnosis; never a default-policy decision."""
import argparse
import json
from pathlib import Path
from statistics import median
from prepare_insert_thread_control import ARCHIVE_SHA, ARCHIVE_BYTES, N_COMMIT, N_SHA, R_COMMIT, R_SHA, TOOLS
from small_compute import counters, require, sha, thread_control_plan
from summarize_small_root import verify_measurement
from summarize_insert_copy_root import standard_metrics, paired_standard, TOOLS as MEASURED_TOOLS

COUNTERS = ('minflt', 'majflt', 'voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches')


def ingest_detail(run):
    phases = {v['phase']: v for v in run['adapter']['phase_cgroups']}
    before, after = phases['before_ingest'], phases['after_ingest']
    a, b = counters(before['cpu.stat']), counters(after['cpu.stat'])
    cpu = {key: b[key] - a[key] for key in ('usage_usec', 'user_usec', 'system_usec', 'throttled_usec')}
    require(all(v >= 0 for v in cpu.values()), 'negative ingest CPU counter')
    for phase in (before, after):
        require(phase.get('extended_counter_scope'), 'extended task counter scope absent')
        for task in phase['native_tasks']:
            require(all(type(task.get(key)) is int and task[key] >= 0 for key in COUNTERS), 'task counter missing/invalid')
    old = {(t['tid'], t['starttime_ticks']): t for t in before['native_tasks']}
    new = {(t['tid'], t['starttime_ticks']): t for t in after['native_tasks']}
    matched, born = [], []
    for identity, task in new.items():
        if identity in old:
            value = {k: task[k] - old[identity][k] for k in COUNTERS}
            require(all(x >= 0 for x in value.values()), 'task counter moved backwards')
            matched.append({'tid': task['tid'], 'main': task['main'], 'delta': value})
        else:
            born.append({'tid': task['tid'], 'main': task['main'],
                         'since_task_creation': {k: task[k] for k in COUNTERS}})
    return {'cpu_delta': cpu, 'matched_tasks': matched, 'new_live_tasks': born,
            'vanished_tasks': [{'tid': key[0], 'starttime_ticks': key[1]} for key in old.keys() - new.keys()],
            'scope': 'Matched TID/starttime deltas plus new live task lifetime counters. Tasks born and exited between snapshots are not observed; not a complete scheduler trace.'}


def collect(repo, out, exit_code):
    result = {'status': 'failed_or_incomplete', 'exit_code': exit_code, 'expected_pairs': 8, 'completed_pairs': 0,
        'new_root_builds': 0, 'new_pgo_builds': 0, 'formal_submission': False, 'automatic_adoption': False,
        'runtime_policy_changed': False, 'candidate_default_status': 'not_adopted',
        'checkout_runtime': 'retained db/exec restored; not built or executed here; N is an imported historical executable'}
    try:
        programs = out / 'programs'
        proof = json.loads((programs / 'import-proof.json').read_bytes())
        ids_path = programs / 'identities.json'; ids = json.loads(ids_path.read_bytes())
        require(proof['status'] == 'verified' and proof['archive_sha256'] == ARCHIVE_SHA and
                proof['archive_bytes'] == ARCHIVE_BYTES and proof['harness_matches_git'] is True and
                proof['prior_root_gates'] == 18 and proof['prior_verified_pairs'] == 24 and
                proof['new_root_builds'] == proof['new_pgo_builds'] == 0 and
                proof['identity_sha256'] == sha(ids_path), 'import proof incomplete')
        require(set(proof['harness_file_sha256']) == set(TOOLS), 'diagnostic input inventory differs')
        for name, digest in proof['harness_file_sha256'].items():
            require(sha(repo / name) == digest, 'diagnostic input changed: ' + name)
        for role, binary, commit in (('baseline', R_SHA, R_COMMIT), ('candidate', N_SHA, N_COMMIT)):
            item = ids[role]
            require(item['binary_sha256'] == binary and item['source_commit'] == commit and item['environment'] == {} and
                    sha(programs / item['binary']) == binary and sha(programs / item['evidence_file']) == item['evidence_sha256'],
                    'imported actual binary/proof differs')
        data = json.loads((out / 'measurements.json').read_bytes())
        verify_measurement(data, 1, ('standard10k',), sha(ids_path), ids, thread_control=True)
        require(data['plan'] == thread_control_plan() and data['workloads'] == [proof['standard_workload']] and
                data['launcher_sha256'] == sha(out / 'cgroup_exec'), 'plan/workload/launcher differs')
        actual_tools = {}
        for path, digest in data['tool_hashes'].items():
            name = Path(path).relative_to(repo).as_posix()
            require(name not in actual_tools and digest == proof['harness_file_sha256'][name] == sha(Path(path)),
                    'measured tool changed')
            actual_tools[name] = digest
        require(set(actual_tools) == MEASURED_TOOLS, 'measured tool inventory differs')
        result.update(harness_commit=proof['harness_commit'], imported_runtime_sources={'baseline': R_COMMIT, 'candidate': N_COMMIT},
                      binary_sha256={'baseline': R_SHA, 'candidate': N_SHA}, prior_candidate_image_id=proof['prior_image_id'])
        result['policies'] = {policy: paired_standard({'pairs': [p for p in data['pairs'] if p['policy'] == policy]})
                              for policy in ('default', 'single')}
        details, differences = [], {}
        for pair in data['pairs']:
            row = {k: pair[k] for k in ('policy', 'pair_index', 'order')}
            row['baseline'], row['candidate'] = (ingest_detail(pair[role]) for role in ('baseline', 'candidate'))
            metrics = {role: standard_metrics(pair[role]) for role in ('baseline', 'candidate')}
            values = {key: metrics['candidate'][key] - metrics['baseline'][key] for key in
                      ('ingest_wall_seconds', 'ingest_cpu_usec', 'first_1000_queries_wall_seconds', 'first_1000_queries_cpu_usec')}
            values.update({key: row['candidate']['cpu_delta'][key] - row['baseline']['cpu_delta'][key]
                           for key in ('user_usec', 'system_usec')})
            row['candidate_minus_baseline'] = values
            differences[(pair['policy'], pair['pair_index'])] = values
            details.append(row)
        result['ingest_detail'] = details
        result['within_runner_policy_difference'] = {}
        for key in differences[('default', 0)]:
            changes = [differences[('single', i)][key] - differences[('default', i)][key] for i in range(4)]
            result['within_runner_policy_difference'][key] = {
                'single_minus_default_of_candidate_minus_baseline': changes, 'median': median(changes)}
        require(exit_code == 0, 'diagnostic process failed')
        result.update(status='passed', completed_pairs=8, conclusion='mechanism_diagnostic_only_manual_interpretation_required')
    except Exception as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
    result['limitations'] = [
        'Eight same-runner pairs: four per policy, one seed/one CPU/exact/2 GiB/swap0. No new image/PGO build; checkout source is not the historical N binary source.',
        'Both parser and query settings change together. A policy interaction cannot isolate one worker pool or prove a scheduler cause; it is not authorization to change the default policy.',
        'All COMMIT/request costs and negative pairs retained. Per-task fault/switch samples cover only boundary-observed tasks; they are not syscall/function/frequency traces.',
        'Prior default N regression and non-adoption remain. This screen is not a score prediction or a replacement for standard-policy protection checks.']
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')
    files = [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'hashes.json']
    (out / 'hashes.json').write_text(json.dumps({'artifacts': files}, indent=2) + '\n', encoding='utf8')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--exit-code', type=int, required=True); args = p.parse_args()
    result = collect(args.repo.resolve(), args.output.resolve(), args.exit_code)
    print(json.dumps({k: result.get(k) for k in ('status', 'completed_pairs', 'error')}))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__': main()
