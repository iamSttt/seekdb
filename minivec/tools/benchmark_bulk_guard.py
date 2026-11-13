#!/usr/bin/env python3
"""Isolated bulk typed PGO candidate: 150 paired comparisons plus 42 exact A/A.

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
from ablation_build import sha
import benchmark_regression_ablation as legacy

summarize = legacy.summarize
projection_run = legacy.projection_run

SEEDS = (7319, 8911, 13337)
ROLES = ('baseline', 'bulk')
CONTRASTS = ('bulk', 'aa')
CASES = ('standard10k', 'resident30k', 'filter60k', 'many16',
         'spill_compound', 'spill_high', 'wide128', 'wide768', 'many64', 'many127')
TARGETS = {'resident30k', 'filter60k', 'many16', 'many64', 'many127', 'wide128', 'wide768'}
ITERATION = {'standard10k', 'resident30k', 'filter60k'}
PROJECTED = set(projection.CASES)


def make_plan():
    result = []
    for case_index, name in enumerate(CASES):
        for seed_index, seed in enumerate(SEEDS):
            candidate_pairs = 5
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
                for index in range((2 if name in TARGETS else 0) if role == 'aa' else 5)}
    ids = [(p['contrast'], p['scenario'], p['seed'], p['pair_index']) for p in plan]
    base.require(len(ids) == len(expected) == 192 and set(ids) == expected, 'missing/duplicate 192-pair plan identity')
    for role in CONTRASTS:
        group = [p for p in plan if p['contrast'] == role]
        base.require(len(group) == (42 if role == 'aa' else 150), 'contrast size differs')
        for name in CASES:
            part = [p for p in group if p['scenario'] == name]
            if not part: continue
            balance = Counter(p['order'][0] for p in part)
            base.require(abs(balance['baseline'] - balance['candidate']) <= (0 if role == 'aa' else 1), 'AB/BA imbalance')
    for p in plan:
        base.require(p['query_threads'] == (2 if p['scenario'] in ('standard10k', 'resident30k') else 1), 'thread plan differs')
    return expected


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
        base.write_report(args.output, {'status': 'plan_only_not_measured', 'plan': plan, 'expected_pairs': 192})
        return
    base.require(platform.system() == 'Linux' and args.binaries is not None and args.build_context is not None, 'Linux, binaries and source/profile context required')
    cpus = sorted(os.sched_getaffinity(0))[:2]
    base.require(len(cpus) == 2, 'two allowed CPUs required'); os.sched_setaffinity(0, cpus)
    context = json.loads(args.build_context.read_bytes())
    binaries = {role: (args.binaries / (role + '-pgo') / 'minivec').resolve() for role in ROLES}
    hashes = {role: sha(path) for role, path in binaries.items()}
    base.require(all(context['profile_source_proof'][r]['binary_sha256'] == hashes[r] for r in ROLES), 'measurement binary differs from PGO proof')
    inputs = args.output.with_suffix('.inputs'); inputs.mkdir()
    report = {'status': 'running', 'kind': 'interleaved_bulk_guard_confirmation_192', 'official_score': None, 'automatic_adoption': False,
        'created_utc': datetime.now(timezone.utc).isoformat(), 'baseline_commit': context['baseline_commit'], 'candidate_commit': context['candidate_commit'],
        'build_context_sha256': sha(args.build_context), 'host': {'platform': platform.platform(), 'affinity': cpus},
        'binaries': {r: {'path': str(binaries[r]), 'sha256': hashes[r]} for r in ROLES},
        'harness_sha256': {Path(module.__file__).name: sha(Path(module.__file__)) for module in (base, matrix, projection, repeat, legacy)},
        'driver_sha256': sha(Path(__file__)), 'environment_before_overrides': base.runtime_environment(), 'plan': plan, 'pairs': [], 'workloads': [],
        'parameters': {'seeds': list(SEEDS), 'queries_iteration': 1000, 'queries_spill': 64, 'projection_repetitions': 3, 'expected_total_pairs': 192},
        'limits': ['Independent paired processes share a host/page cache; A/A is measurement control, not an independent PGO rebuild.',
            'The bulk guard candidate has150 pairs, baseline-identical A/A has42. No automatic switch or formal submission.',
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
        base.require(len(report['pairs']) == 192 and all(sha(binaries[r]) == hashes[r] for r in ROLES), 'incomplete measurement or mutated binary')
        report.update(status='passed', binary_unchanged=True, completed_utc=datetime.now(timezone.utc).isoformat())
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}', failed_pair=pair)
        base.write_report(args.output, report)
        raise
    base.write_report(args.output, report)
    print(json.dumps({'status': 'passed', 'verified_pairs': 192, 'output': str(args.output)}))


if __name__ == '__main__':
    main()
