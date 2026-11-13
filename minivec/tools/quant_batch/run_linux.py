"""Fixed, isolated batch8 correctness and microbenchmark. No DB or PGO."""
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path
from statistics import median


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--queries', type=int, default=64)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'status': 'running', 'scope': 'standalone integer kernels; not a database benchmark',
              'database_runs': 0, 'pgo_builds': 0, 'commands': [],
              'platform': platform.platform(), 'native_compilations': 0, 'native_executions': 0,
              'limitations': [
                  'Synthetic full-int8 immutable corpus; no quantization, cap calculation, TopK, filters, floating distance, storage or worker interaction.',
                  'Same-ISA AVX2 row-pointer vs batch-pointer is primary; native AVX512 vs AVX2 batch, if present, is a separate practical control.',
                  'Process CPU and wall measure whole scan loops, not individual calls; no per-call timer is inserted.',
                  'Batch precomputation may do work that production would skip before its first usable threshold; production integration is untested.',
                  'Microbenchmark gains do not establish query gains or predict formal score.']}

    def save():
        (out/'summary.json').write_text(json.dumps(result, indent=2)+'\n')

    def run(name, command, env=None):
        entry = {'name': name, 'argv': command}
        result['commands'].append(entry)
        save()
        try:
            proc = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=300)
        except subprocess.TimeoutExpired as error:
            text = error.stdout or ''
            if isinstance(text, bytes):
                text = text.decode(errors='replace')
            (out/(name+'.log')).write_text(text)
            entry['status'] = 'timeout'
            save()
            raise
        (out/(name+'.log')).write_text(proc.stdout)
        entry['exit_code'] = proc.returncode
        save()
        require(proc.returncode == 0, name+' failed or unsupported; original log retained')
        return proc.stdout

    save()
    try:
        require(platform.system() == 'Linux' and platform.machine() in ('x86_64', 'amd64'),
                'unsupported platform; Linux x86_64 required')
        require(args.pairs == 4 and args.queries == 64, 'fixed plan requires pairs=4 and queries=64')
        source_files = sorted(p for p in root.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
        result['source_sha256'] = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in source_files}
        reference = json.loads((root/'reference-manifest.json').read_bytes())
        for name, expected in reference['files'].items():
            raw = (root/'reference'/name).read_bytes()
            require(len(raw) == expected['bytes'] and hashlib.sha256(raw).hexdigest() == expected['sha256'], 'reference SHA mismatch: '+name)
            require(hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest() == expected['git_blob'], 'reference Git blob mismatch: '+name)
        result['reference'] = reference
        allowed = sorted(os.sched_getaffinity(0))
        require(bool(allowed), 'empty CPU affinity')
        os.sched_setaffinity(0, {allowed[0]})
        result['cpu_affinity'] = {'before': allowed, 'selected': sorted(os.sched_getaffinity(0))}
        require(len(result['cpu_affinity']['selected']) == 1, 'affinity not single CPU')
        for name, path in {
            'proc-status': '/proc/self/status', 'proc-cgroup': '/proc/self/cgroup',
            'cpu-max': '/sys/fs/cgroup/cpu.max', 'cpuset-effective': '/sys/fs/cgroup/cpuset.cpus.effective',
            'memory-max': '/sys/fs/cgroup/memory.max', 'memory-swap-max': '/sys/fs/cgroup/memory.swap.max',
        }.items():
            p = Path(path)
            if p.is_file():
                (out/(name+'.txt')).write_bytes(p.read_bytes())
            else:
                result.setdefault('unavailable_resource_files', []).append(path)
        result['compiler'] = run('compiler', ['g++', '--version']).splitlines()[0]
        run('cpu', ['lscpu'])
        flags = ['g++', '-std=c++17', '-ffp-contract=off', '-fno-math-errno', '-pthread']
        # ISA is explicit only in target attributes. No -march=native or PGO.
        run('build-normal', flags+['-O3', str(root/'test_batch8.cpp'), '-o', str(out/'normal')])
        result['native_compilations'] += 1
        normal = run('correctness-normal', [str(out/'normal')])
        result['native_executions'] += 1
        run('build-sanitized', flags+['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                                     '-no-pie', str(root/'test_batch8.cpp'), '-o', str(out/'sanitized')])
        result['native_compilations'] += 1
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        sanitized = run('correctness-sanitized', [str(out/'sanitized')], env)
        result['native_executions'] += 1
        counts = []
        for text in (normal, sanitized):
            require('batch8=PASS' in text.splitlines(), 'missing native PASS')
            values = re.findall(r'^batch8_checks=([0-9]+)$', text, re.M)
            require(len(values) == 1 and int(values[0]) > 0, 'invalid native checks count')
            counts.append(int(values[0]))
        require(counts[0] == counts[1], 'normal/sanitized counts differ')
        result['correctness_checks'] = {'normal': counts[0], 'sanitized': counts[1]}
        run('build-microbenchmark', flags+['-O3', str(root/'benchmark_batch8.cpp'), '-o', str(out/'microbenchmark')])
        result['native_compilations'] += 1
        raw = run('microbenchmark', [str(out/'microbenchmark'), str(args.pairs), str(args.queries)])
        result['native_executions'] += 1
        pairs = [json.loads(line) for line in raw.splitlines()]
        labels = {x['comparison'] for x in pairs}
        require(labels in ({'same_isa_avx2'}, {'same_isa_avx2', 'native_baseline_vs_avx2_batch'}), 'comparison set mismatch')
        require(len(pairs) == len(labels)*16, 'pair count mismatch')
        groups = {}
        for label in sorted(labels):
            for dimension in (128, 512):
                for row_count in (10000, 10003):
                    values = [x for x in pairs if (x['comparison'], x['dimension'], x['rows']) == (label, dimension, row_count)]
                    require([x['pair'] for x in values] == list(range(4)), 'pair inventory mismatch')
                    for x in values:
                        require(x['queries'] == 64 and x['order'] == ('AB' if x['pair']%2 == 0 else 'BA'), 'measurement plan mismatch')
                        require(x['baseline_checksum'] == x['candidate_checksum'] and x['baseline_checksum'] > 0, 'checksum mismatch')
                        require(x['candidate_isa'] == 'avx2' and x['baseline_isa'] == ('avx2' if label == 'same_isa_avx2' else 'avx512'), 'ISA identity mismatch')
                    group = {}
                    for metric in ('wall', 'cpu'):
                        require(all(x['baseline_'+metric] > 0 and x['candidate_'+metric] > 0 for x in values), 'invalid timing')
                        ratios = [x['candidate_'+metric]/x['baseline_'+metric] for x in values]
                        group[metric] = {'median_candidate_over_baseline': median(ratios), 'ratios': ratios,
                                         'candidate_worse_pairs': sum(r > 1 for r in ratios),
                                         'baseline_seconds': [x['baseline_'+metric] for x in values],
                                         'candidate_seconds': [x['candidate_'+metric] for x in values]}
                    groups[f'{label}/n{dimension}/rows{row_count}'] = group
        result.update(status='native_correctness_and_microbenchmark_completed', pairs=pairs, groups=groups,
                      binary_sha256={name: hashlib.sha256((out/name).read_bytes()).hexdigest()
                                     for name in ('normal', 'sanitized', 'microbenchmark')})
        save()
    except Exception as error:
        result.update(status='failed_or_unsupported', error=str(error))
        save()
        raise


if __name__ == '__main__':
    main()
