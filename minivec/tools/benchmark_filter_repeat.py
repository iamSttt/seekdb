#!/usr/bin/env python3
"""Interleaved I05/I08 controls with fixed query threads; never official scores.

Every round runs all four contrasts (including identical-binary A/A), rotating
their order. Fifteen pairs cannot split AB/BA equally: each contrast has 8/7 in
thread group 1 and the reversed 7/8 in group 2. Seeds are independently balanced
as closely as possible. First requests remain in the primary lifecycle totals.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
from statistics import median

import benchmark_iteration as base


CONTRASTS = ('multi_only', 'payload_only', 'default', 'aa')
VARIANTS = ('legacy', 'multi_only', 'payload_only', 'default')
PHASES = ('query_before_update', 'query_after_restart')


def make_plan(seeds, pairs, threads):
    result = []
    for thread_index, count in enumerate(threads):
        for seed_index, seed in enumerate(seeds):
            for pair_index in range(pairs):
                round_index = seed_index * pairs + pair_index
                shift = (round_index + thread_index) % len(CONTRASTS)
                contrasts = CONTRASTS[shift:] + CONTRASTS[:shift]
                for slot, contrast in enumerate(contrasts):
                    order = ['baseline', 'candidate']
                    if (seed_index + pair_index + CONTRASTS.index(contrast) + thread_index) % 2:
                        order.reverse()
                    result.append({'query_threads': count, 'seed': seed,
                                   'pair_index': pair_index, 'round_index': round_index,
                                   'round_slot': slot, 'contrast': contrast, 'order': order})
    return result


def validate_plan(plan, seeds, pairs, threads):
    expected = {(t, s, p, c) for t in threads for s in seeds
                for p in range(pairs) for c in CONTRASTS}
    identities = [(p['query_threads'], p['seed'], p['pair_index'], p['contrast']) for p in plan]
    base.require(len(identities) == len(expected) and set(identities) == expected,
                 'missing or duplicate planned contrast')
    balance = {}
    for t in threads:
        for c in CONTRASTS:
            group = [p for p in plan if p['query_threads'] == t and p['contrast'] == c]
            counts = Counter(p['order'][0] for p in group)
            base.require(abs(counts['baseline'] - counts['candidate']) <= 1,
                         'unbalanced AB/BA plan')
            balance[f'threads{t}/{c}'] = dict(counts)
    for start in range(0, len(plan), len(CONTRASTS)):
        group = plan[start:start + len(CONTRASTS)]
        base.require({p['contrast'] for p in group} == set(CONTRASTS), 'noninterleaved round')
        base.require(len({(p['query_threads'], p['seed'], p['pair_index']) for p in group}) == 1,
                     'round crossed a seed/thread boundary')
    return balance


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot_threads(session, phase, original):
    result = original(session, phase)
    tasks = []
    root = Path('/proc') / str(session.proc.pid) / 'task'
    try:
        for path in sorted(root.iterdir(), key=lambda p: int(p.name)):
            fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
            tasks.append({'tid': int(path.name), 'main': int(path.name) == session.proc.pid,
                          'cpu_ticks': int(fields[11]) + int(fields[12])})
        result['native_tasks'] = tasks
        result['clock_ticks_per_second'] = os.sysconf('SC_CLK_TCK')
    except (OSError, ValueError) as exc:
        result['native_tasks_error'] = str(exc)
    return result


def observed_policy(result, threads):
    stderr = result['stderr']
    base.require(not stderr['truncated'], 'fanout stderr must be complete')
    text = stderr['tail_utf8']
    decisions = [int(x) for x in re.findall(r'-> ([12]) threads?', text)]
    forced = [int(x) for x in re.findall(r'-> ([12]) threads? \(forced\)', text)]
    snapshots = {s['phase']: s for s in result['resource_snapshots']}
    evidence = {'requested_threads': threads, 'stderr_decisions': decisions,
                'stderr_forced_decisions': forced, 'phase_task_activity': {}}
    base.require(text.count('minivec ready data=') == 2, 'missing process lifecycle stderr')
    if threads == 2:
        base.require(decisions == [2] * 8 and forced == [2] * 8,
                     'expected four forced two-thread decisions in each process')
    else:
        base.require(not decisions and 'fanout ' not in text,
                     'serial process unexpectedly used/probed query pool')
    for phase, start in zip(PHASES, ('after_index_build', 'after_restart')):
        before, after = snapshots[start], snapshots[phase]
        base.require('native_tasks' in before and 'native_tasks' in after,
                     'native thread evidence unavailable')
        old = {t['tid']: t['cpu_ticks'] for t in before['native_tasks']}
        new = after['native_tasks']
        worker_ticks = sum(t['cpu_ticks'] - old.get(t['tid'], 0) for t in new if not t['main'])
        base.require(worker_ticks >= 0, 'nonmonotonic task CPU evidence')
        evidence['phase_task_activity'][phase] = {
            'native_tasks_before': len(before['native_tasks']), 'native_tasks_after': len(new),
            'nonmain_cpu_ticks_delta': worker_ticks,
            'clock_ticks_per_second': after['clock_ticks_per_second'],
            'restart_scope_includes_verification_and_startup_probe': phase == 'query_after_restart'}
        if threads == 1:
            base.require(worker_ticks == 0, 'serial query phase performed nonmain CPU work')
            if phase == 'query_after_restart':
                base.require(len(new) == 1, 'serial restarted process has an unexplained native task')
        else:
            base.require(len(new) >= 2 and worker_ticks > 0,
                         'forced parallel path has no active native worker evidence')
    return evidence


def run_one(binary, workload, args, threads):
    original_snapshot = base.process_snapshot
    old_threads = os.environ.get('MVDB_QUERY_THREADS')
    os.environ['MVDB_QUERY_THREADS'] = str(threads)
    base.process_snapshot = lambda session, phase: snapshot_threads(session, phase, original_snapshot)
    try:
        result = base.one_run(binary, workload, args)
        try:
            result['policy_evidence'] = observed_policy(result, threads)
        except Exception as error:
            # Preserve raw timings/stderr in failed_pair before the caller
            # rejects this run; a failed policy check is not a speed result.
            result['policy_validation_error'] = f'{type(error).__name__}: {error}'
        return result
    finally:
        base.process_snapshot = original_snapshot
        if old_threads is None:
            os.environ.pop('MVDB_QUERY_THREADS', None)
        else:
            os.environ['MVDB_QUERY_THREADS'] = old_threads


def ratio_summary(values):
    return {'pairs': len(values), 'median_baseline_over_candidate': median(values),
            'min': min(values), 'max': max(values), 'candidate_slower_pairs': sum(x < 1 for x in values)}


def summaries(pairs):
    groups = {}
    for p in pairs:
        key = f"threads{p['query_threads']}/{p['contrast']}"
        groups.setdefault(key, []).append(p)
    output = {}
    for key, group in groups.items():
        phases = {}
        for phase in group[0]['baseline']['phase_seconds']:
            ratios = [p['baseline']['phase_seconds'][phase] / p['candidate']['phase_seconds'][phase]
                      for p in group if p['candidate']['phase_seconds'][phase] > 0]
            if ratios:
                phases[phase] = ratio_summary(ratios)
        parity = {}
        for phase in PHASES:
            parity[phase] = {}
            for offset, label in ((2, 'unfiltered_exact'), (3, 'single_filter_ann')):
                ratios = [math.fsum(p['baseline']['query'][phase]['per_request_seconds'][offset::2]) /
                          math.fsum(p['candidate']['query'][phase]['per_request_seconds'][offset::2])
                          for p in group]
                parity[phase][label] = ratio_summary(ratios)
                parity[phase][label]['order_strata'] = {
                    first: ratio_summary([r for p, r in zip(group, ratios) if p['order'][0] == first])
                    for first in ('baseline', 'candidate')
                    if any(p['order'][0] == first for p in group)}
        output[key] = {'primary_all_requests': phases, 'secondary_excluding_first_per_parity': parity}
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-commit', default='unknown')
    parser.add_argument('--seeds', type=int, nargs='+', default=[7319, 8911, 13337])
    parser.add_argument('--pairs', type=int, default=5)
    parser.add_argument('--threads', type=int, nargs='+', default=[1, 2])
    parser.add_argument('--queries', type=int, default=1000)
    parser.add_argument('--batch-size', type=int, default=100)
    parser.add_argument('--k', type=int, default=10)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--case-timeout', type=float, default=1800)
    parser.add_argument('--plan-only', action='store_true')
    args = parser.parse_args()
    if len(set(args.seeds)) != len(args.seeds) or len(set(args.threads)) != len(args.threads):
        parser.error('duplicate seeds/threads')
    if not set(args.threads) <= {1, 2} or min(args.pairs, args.batch_size, args.k) < 1 or args.queries < 256:
        parser.error('positive pairs/batch/k, queries>=256 and threads in {1,2} required')
    if args.output.exists():
        parser.error('refusing to overwrite existing evidence')
    plan = make_plan(args.seeds, args.pairs, args.threads)
    balance = validate_plan(plan, args.seeds, args.pairs, args.threads)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.plan_only:
        base.write_report(args.output, {'status': 'plan_only_not_measured', 'plan': plan, 'balance': balance})
        print(f'Validated {len(plan)} interleaved planned pairs; no benchmark executed')
        return
    if platform.system() != 'Linux' or args.binaries is None:
        parser.error('Linux and --binaries required for measurements')
    cpus = sorted(os.sched_getaffinity(0))[:2]
    base.require(len(cpus) == 2, 'two allowed CPUs required')
    os.sched_setaffinity(0, cpus)
    binaries = {name: (args.binaries / (name + '.bin')).resolve() for name in VARIANTS}
    before_hashes = {name: sha256(path) for name, path in binaries.items()}
    report = {'kind': 'interleaved_fixed_thread_filter_factorial', 'status': 'running',
              'official_score': None, 'adopted': False, 'source_commit': args.source_commit,
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'parameters': {'seeds': args.seeds, 'pairs_per_contrast_seed': args.pairs,
                             'threads': args.threads, 'queries_per_phase': args.queries,
                             'batch_size': args.batch_size, 'k': args.k},
              'host': {'platform': platform.platform(), 'python': platform.python_version(),
                       'cpu_affinity': cpus, 'logical_cpus': os.cpu_count()},
              'environment_before_group_overrides': base.runtime_environment(),
              'binaries': {name: {'path': str(path), 'sha256': before_hashes[name]}
                           for name, path in binaries.items()},
              'harness_sha256': {p.name: sha256(p) for p in (Path(__file__), Path(base.__file__))},
              'plan': plan, 'balance': balance, 'pairs': [],
              'limits': ['Not formal scoring; no score conversion or automatic adoption.',
                         'Fixed query threads test implementation costs; adaptive policy is a separate question.',
                         'A/A uses the exact same legacy path/hash in both labels, with independent processes.',
                         'All first requests stay in primary totals; secondary parity removes one first query per parity.',
                         'Each pair remains the measurement unit; 1000 requests are not 1000 independent replicates.',
                         'Alternating order and interleaving reduce drift; a single CI host does not prove portability.',
                         'Thread counts/CPU are snapshots outside timers, not instruction-level tracing.',
                         'Nonmain CPU totals include all native workers; forced logs corroborate the query path.',
                         'Post-restart task CPU includes verification/startup probe; forced stderr corroborates parallel scans.',
                         'Policy failures retain completed-run raw data in failed_pair; inherited one_run protocol/process exceptions may retain only the completed peer, with failed-side stderr in CI logs. Such failures are not performance observations.',
                         'Original independent sampled TopK, complete response equality and restart checks remain.']}
    base.write_report(args.output, report)
    previous_seed = None
    workload = None
    pair = None
    try:
        for planned in plan:
            if planned['seed'] != previous_seed:
                workload = base.make_workload(base.FULL_CASES['standard10k'], planned['seed'],
                                              args.queries, args.batch_size, args.k)
                previous_seed = planned['seed']
            contrast = planned['contrast']
            candidate = binaries['legacy' if contrast == 'aa' else contrast]
            pair = dict(planned, scenario='standard10k', workload_sha256=workload['sha256'],
                        rows=10000, dimension=128, mixed_filters=True,
                        binary_sha256={'baseline': before_hashes['legacy'],
                                       'candidate': before_hashes['legacy' if contrast == 'aa' else contrast]})
            for side in pair['order']:
                print(f"threads={pair['query_threads']} seed={pair['seed']} pair={pair['pair_index']} "
                      f"contrast={contrast} slot={pair['round_slot']} {side}", flush=True)
                pair[side] = run_one(binaries['legacy'] if side == 'baseline' else candidate,
                                     workload, args, pair['query_threads'])
                base.require('policy_validation_error' not in pair[side],
                             pair[side].get('policy_validation_error', 'policy validation failed'))
            base.require(pair['baseline']['response_sha256'] == pair['candidate']['response_sha256'],
                         'all-response equality failed')
            pair['all_responses_identical'] = True
            report['pairs'].append(pair)
            report['summary'] = summaries(report['pairs'])
            base.write_report(args.output, report)
        base.require(len(report['pairs']) == len(plan), 'incomplete pair matrix')
        report['binary_sha256_after'] = {name: sha256(path) for name, path in binaries.items()}
        base.require(report['binary_sha256_after'] == before_hashes, 'binary changed during measurement')
        report['binary_unchanged'] = True
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}', failed_pair=pair)
        base.write_report(args.output, report)
        raise
    report['completed_utc'] = datetime.now(timezone.utc).isoformat()
    base.write_report(args.output, report)
    print(f'Saved {len(report["pairs"])} verified pairs to {args.output}')


if __name__ == '__main__':
    main()
