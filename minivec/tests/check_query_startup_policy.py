#!/usr/bin/env python3
"""Linux-only real-affinity / environment / existing-pool compatibility test.

Builds only a tiny test probe; it does not build or benchmark the database.
No test result is claimed until this script has actually run on Linux.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    repo_source = Path(__file__).resolve().parents[1] / 'src'
    parser.add_argument('--source', type=Path, default=repo_source,
                        help='repository minivec/src directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() != repo_source:
        raise SystemExit('source must be the adjacent repository minivec/src')
    if sys.platform != 'linux':
        raise SystemExit('requires Linux; no tests executed')
    allowed = sorted(os.sched_getaffinity(0))
    if len(allowed) < 2:
        raise SystemExit('requires two allowed CPUs for the preservation control')
    assert not args.output.exists(), 'refuse overwrite'
    here = Path(__file__).resolve().parent
    cases = []
    with tempfile.TemporaryDirectory(prefix='mvdb-startup-policy-') as scratch:
        normal = Path(scratch) / 'policy'
        failure = Path(scratch) / 'policy-affinity-error'
        command = ['c++', '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra',
                   '-Werror', '-I', str(args.source.resolve()), '-I', str(here),
                   str(here / 'test_query_startup_policy.cpp')]
        subprocess.run(command + ['-o', str(normal)], check=True)
        subprocess.run(command + ['-Wl,--wrap=sched_getaffinity', '-o', str(failure)], check=True)

        def run(name, cpus, overrides, binary=normal, **expected):
            env = {k: v for k, v in os.environ.items() if not k.startswith('MVDB_')}
            env.update(overrides)
            result = subprocess.run(
                ['taskset', '-c', ','.join(map(str, cpus)), str(binary)],
                env=env, text=True, capture_output=True, timeout=10, check=True)
            fields = dict(line.split('=', 1) for line in result.stdout.splitlines())
            for key, value in expected.items():
                assert fields[key] == str(value), (name, key, fields[key], value)
            cases.append({'name': name, 'affinity': cpus, 'environment': overrides,
                          'observed': fields, 'stderr': result.stderr})

        one = allowed[:1]
        two = allowed[:2]
        run('automatic single CPU retains real worker and calibration', one, {},
            threshold='9223372036854775807', mode=2, min_rows='9223372036854775807',
            threads_before=1, threads_after=2, wanted=0)
        run('automatic two CPUs unchanged', two, {'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='<unset>', mode=2, min_rows=1024, wanted=1)
        run('explicit force remains force on one CPU', one,
            {'MVDB_QUERY_THREADS': '2', 'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='<unset>', mode=1, wanted=1)
        run('explicit serial still avoids worker and calibration', one,
            {'MVDB_QUERY_THREADS': '1'}, threshold='<unset>', mode=0,
            threads_before=1, threads_after=1, wanted=0)
        run('explicit threshold remains effective', one,
            {'MVDB_QUERY_FANOUT_ROWS': '0', 'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='0', mode=2, min_rows=0, wanted=1)
        run('explicit empty thread value retains prior default', one,
            {'MVDB_QUERY_THREADS': '', 'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='<unset>', mode=2, min_rows=1024, wanted=1)
        run('explicit empty threshold retains prior default', one,
            {'MVDB_QUERY_FANOUT_ROWS': '', 'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='', mode=2, min_rows=1024, wanted=1)
        run('single-thread override remains strongest', one,
            {'MVDB_SINGLE_THREAD': '1', 'MVDB_QUERY_THREADS': '2'},
            threshold='<unset>', mode=0, threads_after=1, wanted=0)
        run('explicit single-thread zero retains prior default', one,
            {'MVDB_SINGLE_THREAD': '0', 'MVDB_WAKE_US_OVERRIDE': '0'},
            threshold='<unset>', mode=2, min_rows=1024, wanted=1)
        run('parser override is neither rewritten nor blocking', one,
            {'MVDB_PARSE_THREADS': '2'}, threshold='9223372036854775807',
            parse='2', mode=2, threads_after=2, wanted=0)
        run('affinity read failure preserves default', one,
            {'MVDB_WAKE_US_OVERRIDE': '0'}, binary=failure,
            threshold='<unset>', mode=2, min_rows=1024, wanted=1)
    report = {'status': 'passed', 'cases': cases, 'case_count': len(cases),
              'source_file_sha256': {
                  name: hashlib.sha256((repo_source / name).read_bytes()).hexdigest()
                  for name in ('query_startup_policy.h', 'query_pool.h')},
              'database_builds': 0, 'database_performance_pairs': 0,
              'scope': 'x86_64 Linux actual affinity and original query_pool compatibility; no quota-only inference'}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps({'status': 'passed', 'cases': len(cases)}))


if __name__ == '__main__':
    main()
