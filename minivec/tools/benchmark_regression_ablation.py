#!/usr/bin/env python3
"""Interleaved PGO regression controls: 60 pairs per candidate plus 18 exact A/A.

All first/restart requests remain timed; front64 and remaining936 are reported
separately, not treated as independent replicates. No official score is inferred.
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
from time import perf_counter

import benchmark_iteration as base
import benchmark_report_matrix as matrix
import benchmark_projection as projection
import benchmark_filter_repeat as repeat

SEEDS = (7319, 8911, 13337)
ROLES = ('baseline', 'equality', 'dom')
CONTRASTS = ('equality', 'dom', 'aa')
CASES = ('standard10k', 'resident30k', 'filter60k', 'many16',
         'spill_compound', 'spill_high', 'wide128', 'wide768')
TARGETS = {'resident30k', 'filter60k', 'many16'}
ITERATION = {'standard10k', 'resident30k', 'filter60k'}
PROJECTED = set(projection.CASES)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_plan():
    result = []
    for case_index, name in enumerate(CASES):
        for seed_index, seed in enumerate(SEEDS):
            candidate_pairs = 5 if name in ITERATION else 1
            aa_pairs = 2 if name in TARGETS else 0
            for index in range(max(candidate_pairs, aa_pairs)):
                shift = (case_index + seed_index + index) % len(CONTRASTS)
                rotation = CONTRASTS[shift:] + CONTRASTS[:shift]
                for role in rotation:
                    if index >= (aa_pairs if role == 'aa' else candidate_pairs): continue
                    order = ['baseline', 'candidate']
                    if (case_index + seed_index + index + CONTRASTS.index(role)) % 2: order.reverse()
                    result.append({'scenario': name, 'seed': seed, 'pair_index': index, 'contrast': role,
                                   'order': order, 'query_threads': 2 if name in ('standard10k', 'resident30k') else 1})
    validate_plan(result)
    return result


def validate_plan(plan):
    expected = {(role, name, seed, index) for role in CONTRASTS for name in CASES for seed in SEEDS
                for index in range((2 if name in TARGETS else 0) if role == 'aa' else (5 if name in ITERATION else 1))}
    ids = [(p['contrast'], p['scenario'], p['seed'], p['pair_index']) for p in plan]
    base.require(len(ids) == len(expected) == 138 and set(ids) == expected, 'missing/duplicate 138-pair plan identity')
    for role in CONTRASTS:
        group = [p for p in plan if p['contrast'] == role]
        base.require(len(group) == (18 if role == 'aa' else 60), 'contrast size differs')
        for name in CASES:
            part = [p for p in group if p['scenario'] == name]
            if not part: continue
            balance = Counter(p['order'][0] for p in part)
            base.require(abs(balance['baseline'] - balance['candidate']) <= (0 if role == 'aa' else 1), 'AB/BA imbalance')
    for p in plan:
        base.require(p['query_threads'] == (2 if p['scenario'] in ('standard10k', 'resident30k') else 1), 'thread plan differs')
    return expected


def ratio_summary(values):
    return {'pairs': len(values), 'median_baseline_over_candidate': median(values), 'minimum': min(values),
            'maximum': max(values), 'candidate_slower_pairs': sum(v < 1 for v in values)}


def summarize(pairs):
    groups = {}
    for pair in pairs: groups.setdefault((pair['contrast'], pair['scenario']), []).append(pair)
    output = {}
    for (role, name), group in groups.items():
        projected = name in PROJECTED
        def values(run):
            if projected:
                return {**{'lifecycle/' + k: v for k, v in run['lifecycle_seconds'].items()},
                        **{'query/' + k: v['wall_seconds'] for k, v in run['phases'].items()},
                        'accounted_timer_sum': math.fsum(run['lifecycle_seconds'].values()) + math.fsum(v['wall_seconds'] for v in run['phases'].values())}
            result = dict(run['phase_seconds'])
            for phase, data in run['query'].items():
                requests = data['per_request_seconds']
                for label, subset in [('first', requests[:1]), ('front64', requests[:64]), ('after_first_front64', requests[1:64]), ('after64', requests[64:])]:
                    if subset: result[phase + '/' + label] = math.fsum(subset)
                if name == 'standard10k':
                    for label, start in (('unfiltered_exact_all', 0), ('single_filter_ann_all', 1),
                                         ('unfiltered_exact_after_first', 2), ('single_filter_ann_after_first', 3)):
                        result[phase + '/' + label] = math.fsum(requests[start::2])
                    for begin, end in ((0, 64), (64, 256), (256, 512), (512, 1000)):
                        for parity, label in ((0, 'unfiltered_exact'), (1, 'single_filter_ann')):
                            indices = [i for i in range(max(begin, 2), min(end, len(requests))) if i % 2 == parity]
                            if indices: result[f'{phase}/{label}_window_{indices[0]}_{indices[-1]}'] = math.fsum(requests[i] for i in indices)
            return result
        cached = [(p, values(p['baseline']), values(p['candidate'])) for p in group]
        keys = set(cached[0][1])
        metrics = {}
        for key in sorted(keys):
            ratios = [b[key] / c[key] for _, b, c in cached]
            metrics[key] = {**ratio_summary(ratios),
                'baseline_seconds_median': median(b[key] for _, b, _ in cached),
                'candidate_seconds_median': median(c[key] for _, _, c in cached),
                'by_seed': {str(seed): ratio_summary([r for p, r in zip(group, ratios) if p['seed'] == seed]) for seed in SEEDS if any(p['seed'] == seed for p in group)},
                'by_order': {first: ratio_summary([r for p, r in zip(group, ratios) if p['order'][0] == first]) for first in ('baseline', 'candidate') if any(p['order'][0] == first for p in group)}}
        output[role + '/' + name] = metrics
    return output


def projection_run(binary, workload, args, partial):
    original = projection.exchange
    observations = []
    counts = Counter()
    def observed(session, sql, *, expected=None, affected=None):
        label = expected['label'] if expected is not None else None
        if label and counts[label] == 0:
            observations.append(repeat.snapshot_threads(session, label + '/before', base.process_snapshot))
        value = original(session, sql, expected=expected, affected=affected)
        if label:
            counts[label] += 1
            if counts[label] == args.repetitions:
                observations.append(repeat.snapshot_threads(session, label + '/after', base.process_snapshot))
        return value
    projection.exchange = observed
    try:
        run = projection.one_run(binary, workload, args, partial)
        text = run['stderr']['tail']
        base.require(run['stderr']['bytes'] <= 4096 and 'fanout ' not in text and text.count('minivec ready data=') == 1,
                     'projection serial query policy evidence incomplete')
        base.require(len(observations) == 14 and all('native_tasks' in s for s in observations), 'projection task snapshots missing')
        run['policy_evidence'] = {'requested_query_threads': 1, 'no_query_fanout_diagnostics': True,
            'task_snapshots': observations, 'scope': 'Query pool disabled by fixed override. Native task CPU may include parser work; not a claim that every native worker is idle.'}
        return run
    finally:
        partial['policy_snapshots_partial'] = observations
        projection.exchange = original


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries', type=Path)
    parser.add_argument('--build-context', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plan-only', action='store_true')
    args = parser.parse_args()
    args.queries, args.batch_size, args.k, args.timeout, args.case_timeout, args.repetitions = 1000, 100, 10, 120, 1800, 3
    base.require(not args.output.exists() and not args.output.with_suffix('.inputs').exists(), 'refusing to overwrite evidence')
    plan = make_plan()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.plan_only:
        base.write_report(args.output, {'status': 'plan_only_not_measured', 'plan': plan, 'expected_pairs': 138})
        return
    base.require(platform.system() == 'Linux' and args.binaries is not None and args.build_context is not None, 'Linux, binaries and source/profile context required')
    cpus = sorted(os.sched_getaffinity(0))[:2]
    base.require(len(cpus) == 2, 'two allowed CPUs required'); os.sched_setaffinity(0, cpus)
    context = json.loads(args.build_context.read_bytes())
    binaries = {role: (args.binaries / (role + '-pgo') / 'minivec').resolve() for role in ROLES}
    hashes = {role: sha(path) for role, path in binaries.items()}
    base.require(all(context['profile_source_proof'][r]['binary_sha256'] == hashes[r] for r in ROLES), 'measurement binary differs from PGO proof')
    inputs = args.output.with_suffix('.inputs'); inputs.mkdir()
    report = {'status': 'running', 'kind': 'interleaved_regression_ablation_138', 'official_score': None, 'automatic_adoption': False,
        'created_utc': datetime.now(timezone.utc).isoformat(), 'baseline_commit': context['baseline_commit'], 'candidate_commit': context['candidate_commit'],
        'build_context_sha256': sha(args.build_context), 'host': {'platform': platform.platform(), 'affinity': cpus},
        'binaries': {r: {'path': str(binaries[r]), 'sha256': hashes[r]} for r in ROLES},
        'harness_sha256': {Path(module.__file__).name: sha(Path(module.__file__)) for module in (base, matrix, projection, repeat)},
        'driver_sha256': sha(Path(__file__)), 'environment_before_overrides': base.runtime_environment(), 'plan': plan, 'pairs': [], 'workloads': [],
        'parameters': {'seeds': list(SEEDS), 'queries_iteration': 1000, 'queries_spill': 64, 'projection_repetitions': 3, 'expected_total_pairs': 138},
        'limits': ['Independent paired processes share a host/page cache; A/A is measurement control, not an independent PGO rebuild.',
            'Each candidate has60 pairs, baseline-identical A/A has18. No automatic switch or formal submission.',
            'front64 retains the deterministic64-query prefix and initial data. Additional936 requests alter later process history; historical runs still differ in machine, binary and schedule.',
            'First/restart requests stay primary. A projection timer sum includes ingestion but excludes untimed parsing/oracles/snapshots.',
            'TopK oracles sample0,1,last for iteration; spill predicates and every projected response have complete independent oracles.',
            'Inherited lifecycle transport failures may retain only completed peer and phase stderr; failed observations are never speed samples.']}
    base.write_report(args.output, report)
    pair = None
    try:
        previous, workload = None, None
        for entry in plan:
            key = entry['scenario'], entry['seed']; name, seed = key
            if key != previous:
                started = perf_counter()
                if name in PROJECTED:
                    workload = projection.workload(name, seed)
                    path = inputs / f'{name}-{seed}.jsonl'
                    with path.open('wb') as stream:
                        for sql, affected in workload['requests']: stream.write(projection.encoded({'sql': sql, 'affected_rows': affected}) + b'\n')
                        for phase in workload['phases']: stream.write(projection.encoded({'sql': phase['sql'], 'label': phase['label'], 'columns': phase['columns'], 'expected_rows': len(phase['rows'])}) + b'\n')
                    corpus = sha(path)
                    item = {'scenario': name, 'seed': seed, 'input_sha256': corpus, 'input_path': str(path), 'input_bytes': path.stat().st_size,
                            'rows': workload['rows'], 'dimension': workload['dimension'], 'string_codepoints': workload['string_codepoints']}
                else:
                    workload = base.make_workload(base.FULL_CASES[name], seed, 1000, 100, 10) if name in ITERATION else matrix.make_workload(matrix.CASES[name], seed, 64, 100)
                    corpus = workload['sha256']
                    item = {'scenario': name, 'seed': seed, 'workload_sha256': corpus, 'rows': workload['rows'], 'dimension': workload['dimension'],
                            'queries': len(workload['queries']), 'oracle_indices': workload['oracle_indices']}
                item['generation_and_oracle_seconds_excluded'] = perf_counter() - started
                report['workloads'].append(item); previous = key
            pair = {**entry, 'workload_sha256': corpus, 'baseline': None, 'candidate': None,
                    'query_environment': {'MVDB_QUERY_THREADS': str(entry['query_threads'])},
                    'binary_paths': {'baseline': str(binaries['baseline']), 'candidate': str(binaries['baseline' if entry['contrast'] == 'aa' else entry['contrast']])},
                    'binary_sha256': {'baseline': hashes['baseline'], 'candidate': hashes['baseline' if entry['contrast'] == 'aa' else entry['contrast']]}}
            for side in entry['order']:
                role = 'baseline' if side == 'baseline' or entry['contrast'] == 'aa' else entry['contrast']
                print(f"{name} seed={seed} pair={entry['pair_index']} contrast={entry['contrast']} {side}={role}", flush=True)
                if name in PROJECTED:
                    previous_threads = os.environ.get('MVDB_QUERY_THREADS'); os.environ['MVDB_QUERY_THREADS'] = '1'
                    try:
                        pair[side] = {}; projection_run(binaries[role], workload, args, pair[side])
                    finally:
                        if previous_threads is None: os.environ.pop('MVDB_QUERY_THREADS', None)
                        else: os.environ['MVDB_QUERY_THREADS'] = previous_threads
                else:
                    pair[side] = repeat.run_one(binaries[role], workload, args, entry['query_threads'])
                    base.require('policy_validation_error' not in pair[side], pair[side].get('policy_validation_error', 'policy failure'))
            if name in PROJECTED:
                for label in [p['label'] for p in workload['phases']]:
                    b, c = pair['baseline']['phases'][label], pair['candidate']['phases'][label]
                    base.require([x['response_sha256'] for x in b['requests']] == [x['response_sha256'] for x in c['requests']], 'projection response differs')
                pair['all_responses_identical_and_independently_verified'] = True
            else:
                base.require(pair['baseline']['response_sha256'] == pair['candidate']['response_sha256'], 'lifecycle response differs')
                pair['all_responses_identical'] = True
            report['pairs'].append(pair); report['summary'] = summarize(report['pairs']); base.write_report(args.output, report)
        base.require(len(report['pairs']) == 138 and all(sha(binaries[r]) == hashes[r] for r in ROLES), 'incomplete measurement or mutated binary')
        report.update(status='passed', binary_unchanged=True, completed_utc=datetime.now(timezone.utc).isoformat())
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}', failed_pair=pair)
        base.write_report(args.output, report)
        raise
    base.write_report(args.output, report)
    print(json.dumps({'status': 'passed', 'verified_pairs': 138, 'output': str(args.output)}))


if __name__ == '__main__':
    main()
