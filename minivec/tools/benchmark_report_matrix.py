#!/usr/bin/env python3
"""Report I02/I05/I08 diagnostic matrix. No official score estimation."""
import argparse
from datetime import datetime, timezone
import hashlib
import heapq
import json
import os
from pathlib import Path
import platform
from time import perf_counter

import benchmark_iteration as base

# Representative combinations, not an exhaustive Cartesian product.
CASES = {
    'compound': (10000, 128, 10, 0, 'compound'),
    'compound_reverse': (10000, 128, 10, 0, 'reverse'),
    'alternate_columns': (10000, 128, 10, 0, 'alternate'),
    'range': (10000, 128, 10, 0, 'range'),
    'high_hit': (10000, 128, 10, 0, 'high'),
    'zero_hit': (10000, 128, 10, 0, 'zero'),
    'small_compound': (256, 16, 10, 0, 'compound'),
    'spill_compound': (5000, 128, 10, 8192, 'compound'),
    'spill_high': (5000, 128, 10, 8192, 'high'),
    'filter1pct': (10000, 128, 10, 0, 'one_percent'),
    'filter10pct': (10000, 128, 10, 0, 'ten_percent'),
    'high768_k1': (10000, 768, 1, 0, None),
    'high768_k100': (10000, 768, 100, 0, None),
    'large100k': (100000, 16, 10, 0, None),
}
DEFAULT_CASES = tuple(name for name in CASES if name != 'spill_high')


def predicate(kind, index):
    """SQL and an independent integer predicate used by the brute-force oracle."""
    a, b = index % 2, index % 1000
    if kind == 'compound':
        return f'a={a} AND b={b}', lambda x, y: x == a and y == b
    if kind == 'reverse':
        return f'b={b} AND a={a}', lambda x, y: x == a and y == b
    if kind == 'alternate':
        return ((f'a={a}', lambda x, y: x == a) if index % 2 else
                (f'b={b}', lambda x, y: y == b))
    if kind == 'range':
        return f'a>={a} AND b>={b} AND b<{b+10}', lambda x, y: x >= a and b <= y < b+10
    if kind == 'high':
        return 'a>=0 AND b>=0', lambda x, y: True
    if kind == 'zero':
        return 'a=0 AND b=-1', lambda x, y: False
    if kind in ('one_percent', 'ten_percent'):
        bound = 10 if kind == 'one_percent' else 100
        return f'b<{bound}', lambda x, y: y < bound
    raise ValueError(kind)


def make_workload(spec, seed, queries, batch_size):
    rows, dim, k, padding, kind = spec
    work = base.make_workload((rows, dim, 0, padding), seed, queries, batch_size, k)
    if kind is None:
        work['predicate_kind'] = None
        return work
    # Keep vector generation and lifecycle validation identical; build predicates
    # independently of the engine and of its choice of posting path.
    vectors = work['vectors']
    labels = [(key % 2, (key // 2) % 1000) for key in range(rows)]
    extra = f',pad VARCHAR({padding})' if padding else ''
    work['create'] = f'CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dim}){extra},a BIGINT,b BIGINT);'
    pad = ", '" + 'x' * padding + "'" if padding else ''
    literals = [base.vector_literal(v) for v in vectors]
    work['inserts'] = []
    for stop in range(rows, 0, -batch_size):
        start = max(0, stop-batch_size)
        sql = 'INSERT INTO docs VALUES ' + ','.join(
            f'({key},{literals[key]}{pad},{labels[key][0]},{labels[key][1]})'
            for key in range(stop-1, start-1, -1)) + ';'
        work['inserts'].append((sql, stop-start))
    import random
    from array import array
    rng = random.Random(seed ^ 0x19B71)
    query_vectors = [array('f', vectors[0])]
    query_vectors.extend(array('f', (rng.gauss(0, 1) for _ in range(dim))) for _ in range(queries-1))
    sql_queries, counts, oracle = [], [], {}
    for index, query in enumerate(query_vectors):
        sql_predicate, matches = predicate(kind, index)
        keys = [key for key, (a, b) in enumerate(labels) if matches(a, b)]
        sql_queries.append(f'SELECT id,L2_DISTANCE(v,{base.vector_literal(query)}) AS d FROM docs '
                           f'WHERE {sql_predicate} ORDER BY d LIMIT {k};')
        counts.append(min(k, len(keys)))
        # Every composite query has an independent oracle, not just a digest
        # comparison of two implementations that could share the same bug.
        oracle[index] = heapq.nsmallest(k, ((base.squared_distance(vectors[key], query), key) for key in keys))
    work.update(queries=sql_queries, query_counts=counts, before_oracle=oracle,
                after_oracle=oracle, oracle_indices=list(range(queries)), predicate_kind=kind)
    digest = hashlib.sha256(work['create'].encode())
    for sql, _ in work['inserts']:
        digest.update(sql.encode())
    for sql in sql_queries:
        digest.update(sql.encode())
    work['sha256'] = digest.hexdigest()
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scenario', action='append', choices=CASES)
    parser.add_argument('--seeds', type=int, nargs='+', default=[7319, 8911, 13337])
    parser.add_argument('--pairs', type=int, default=5)
    parser.add_argument('--queries', type=int, default=64)
    parser.add_argument('--batch-size', type=int, default=100)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--case-timeout', type=float, default=1800)
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('requires Linux transport and /proc')
    if min(args.pairs, args.batch_size, args.timeout, args.case_timeout) <= 0 or args.queries < 2:
        parser.error('invalid positive measurement parameters')
    if len(set(args.seeds)) != len(args.seeds):
        parser.error('duplicate seeds')
    if args.output.exists() or args.output.with_name(args.output.name+'.tmp').exists():
        parser.error('refusing to overwrite measurements')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.baseline, args.candidate = args.baseline.resolve(), args.candidate.resolve()
    report = {'status': 'running', 'kind': 'report_matrix_screen', 'official_score': None,
              'created_utc': datetime.now(timezone.utc).isoformat(), 'pairs': [],
              'parameters': {'seeds': args.seeds, 'pairs': args.pairs, 'queries': args.queries},
              'host': {'platform': platform.platform(), 'affinity': sorted(os.sched_getaffinity(0))},
              'environment': base.runtime_environment(),
              'binaries': {name: {'path': str(getattr(args, name)),
                         'sha256': hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()}
                           for name in ('baseline', 'candidate')},
              'limits': ['Synthetic representative matrix, not hidden/formal cases.',
                         'RSS/process IO proxies exclude full cgroup and delayed writeback.',
                         'All requests and initial posting preparation remain timed.',
                         'Shared host/cache; alternating order does not remove all noise.']}
    base.write_report(args.output, report)
    pair = None
    try:
        for case_index, name in enumerate(args.scenario or DEFAULT_CASES):
            for seed_index, seed in enumerate(args.seeds):
                started = perf_counter()
                workload = make_workload(CASES[name], seed, args.queries, args.batch_size)
                generation = perf_counter()-started
                for index in range(args.pairs):
                    order = ['baseline', 'candidate']
                    if (case_index+seed_index+index) % 2:
                        order.reverse()
                    pair = {'scenario': name, 'seed': seed, 'pair_index': index, 'order': order,
                            'spec': CASES[name], 'workload_sha256': workload['sha256'],
                            'generation_and_oracle_seconds_excluded': generation}
                    for side in order:
                        print(f'{name} seed={seed} pair={index+1}/{args.pairs} {side}', flush=True)
                        pair[side] = base.one_run(getattr(args, side), workload, args)
                    base.require(pair['baseline']['response_sha256'] == pair['candidate']['response_sha256'],
                                 'response mismatch: '+name)
                    pair['all_responses_identical'] = True
                    report['pairs'].append(pair)
                    report['summary'] = base.paired_summary(report['pairs'])
                    base.write_report(args.output, report)
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}', failed_pair=pair)
        base.write_report(args.output, report)
        raise
    report['completed_utc'] = datetime.now(timezone.utc).isoformat()
    base.write_report(args.output, report)
    print(json.dumps(report['summary'], indent=2))


if __name__ == '__main__':
    main()
