"""Unexecuted backup: frozen compensated algorithm, three fixed whole-call cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
from statistics import median
import subprocess
import sys

SCENARIOS = ('fullmantissa128', 'fullmantissa512', 'grid128')
PINS = {
    'reference_distance.h': '7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9',
    'compensated_distance.h': '8cb12b2cf9c4c76d87099584a46f6e82e429b12448c7c0a7ff902317ba10585c',
    'test_compensated_distance.cpp': '421f18da3f8817dfed53ea16fa7043504b96bca91f9e43a70e00529a5fc14d5d',
    'oracle.bin': 'a159c84b49ad4a2c018aa208677cd67143395d362ce214fa7a34db83adcd3608',
    'micro_data.h': 'f290d0686747aad93165428148ba26f169c6f6d7c9497262de2c73bd52776296',
    'prototype-manifest.json': '26efc5b88c733e7a40ed0644a985b0853da1559213325ef458f2a7f75d702749',
}


def require(value, message):
    if not value:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_sources(root):
    manifest = json.loads((root / 'runner-manifest.json').read_bytes())
    items = manifest['files']
    require(len({x['path'] for x in items}) == len(items), 'duplicate manifest member')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(actual == {x['path'] for x in items} | {'runner-manifest.json'}, 'source inventory differs')
    for item in items:
        rel = Path(item['path'])
        require(not rel.is_absolute() and '..' not in rel.parts, 'invalid member path')
        path = root / rel
        require(path.stat().st_size == item['bytes'] and sha(path) == item['sha256'], 'source changed: '+str(rel))
    for name, digest in PINS.items():
        require(sha(root / name) == digest, 'frozen algorithm/reference/input changed: '+name)
    require('tighter_long_double_environment' not in (root / 'reference_distance.h').read_text(), 'new tighter certificate imported')
    return manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    out = args.output.resolve()
    require(out != root and root not in out.parents, 'output overlaps immutable source')
    out.mkdir(parents=True, exist_ok=False)
    result = {'status': 'running', 'scope': 'standalone warm whole scalar call; no DB or PGO',
              'database_runs': 0, 'pgo_builds': 0, 'commands': [],
              'native_compilation_attempts': 0, 'native_compilations_completed': 0,
              'native_execution_attempts': 0, 'native_executions_completed': 0,
              'plan': {'scenarios': list(SCENARIOS), 'pairs_per_scenario': 4,
                       'orders': ['AB', 'BA', 'AB', 'BA'], 'rows': 512,
                       'repetitions_per_arm': 64, 'whole_call': True, 'counter_instrumented_timing': False},
              'limitations': ['One compilation without PGO; not an actual-root database binary.',
                              'Every candidate call includes shape/ISA/FP/coordinate checks and scalar fallback.',
                              'Both entries are warmed; first-use CPU feature initialization is not timed.',
                              'Three synthetic corpora do not represent SQL/TopK/filter/storage behavior.',
                              'No cross-run comparison to tighter results, adoption or score inference.']}

    def save():
        (out / 'summary.json').write_text(json.dumps(result, indent=2)+'\n')

    def run(name, command, env=None, native_kind=None):
        entry = {'name': name, 'argv': list(map(str, command)), 'native_kind': native_kind}
        result['commands'].append(entry)
        if native_kind:
            result['native_'+native_kind+'_attempts'] += 1
        save()
        try:
            with (out / (name+'.log')).open('w') as log:
                process = subprocess.Popen(entry['argv'], cwd=root, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                entry.update(status='started', pid=process.pid)
                save()
                try:
                    code = process.wait(timeout=180)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    entry.update(status='timeout', exit_code=process.returncode)
                    save()
                    raise
        except Exception as exc:
            entry.setdefault('status', 'launch_failed')
            entry['error'] = str(exc)
            save()
            raise
        entry.update(exit_code=code, status='completed' if code == 0 else 'failed_or_unsupported')
        save()
        require(code == 0, name+' failed/unsupported; log retained')
        if native_kind:
            result['native_'+native_kind+'_completed'] += 1
        save()
        return (out / (name+'.log')).read_text()

    save()
    try:
        require(platform.system() == 'Linux' and platform.machine() == 'x86_64', 'Linux x86_64 only')
        manifest = verify_sources(root)
        result['runner_manifest_sha256'] = sha(root / 'runner-manifest.json')
        result['source_sha256'] = {x['path']: x['sha256'] for x in manifest['files']}
        result['source_proof'] = json.loads((root / 'source-proof.json').read_bytes())
        allowed = sorted(os.sched_getaffinity(0))
        require(len(allowed) == 1, 'caller must provide exactly one CPU affinity')
        result['affinity'] = {'before': allowed, 'selected': allowed}
        observed = {}
        for name, path in {'proc-status': '/proc/self/status', 'proc-cgroup': '/proc/self/cgroup',
                           'cpu-max': '/sys/fs/cgroup/cpu.max', 'memory-max': '/sys/fs/cgroup/memory.max',
                           'memory-swap-max': '/sys/fs/cgroup/memory.swap.max'}.items():
            value = Path(path).read_text()
            (out / (name+'.txt')).write_text(value)
            observed[name] = value.strip()
        result['resources'] = observed
        save()
        quota, period = map(int, observed['cpu-max'].split())
        require(quota == period and period > 0, 'one CPU quota required')
        require(observed['memory-max'] == '2147483648' and observed['memory-swap-max'] == '0', '2 GiB/no-swap required')
        result['compiler'] = run('compiler', ['g++', '--version']).splitlines()[0]
        require(run('compiler-version', ['g++', '-dumpfullversion']).strip() == '13.3.0', 'fixed GCC 13.3.0 required')
        run('cpu', ['lscpu'])
        run('fixture-generation', [sys.executable, root / 'make_fixtures.py', '--output', out / 'oracle.bin'])
        oracle = json.loads((out / 'oracle.json').read_bytes())
        require(oracle['cases'] == 7842 and sha(out / 'oracle.bin') == oracle['sha256'] == PINS['oracle.bin'], 'frozen exact oracle differs')
        result['oracle'] = oracle
        flags = ['g++', '-std=c++17', '-ffp-contract=off', '-fno-math-errno', '-pthread']
        run('build-normal', flags+['-O3', root / 'test_compensated_distance.cpp', '-o', out / 'normal'], native_kind='compilation')
        normal = run('normal', [out / 'normal', out / 'oracle.bin'], native_kind='execution')
        run('build-sanitized', flags+['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-no-pie', root / 'test_compensated_distance.cpp', '-o', out / 'sanitized'], native_kind='compilation')
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        sanitized = run('sanitized', [out / 'sanitized', out / 'oracle.bin'], environment, native_kind='execution')
        counts = []
        for log in (normal, sanitized):
            require('compensated_distance=PASS' in log.splitlines(), 'missing correctness pass marker')
            matches = re.findall(r'^compensated_distance_checks=(\d+)$', log, re.M)
            require(len(matches) == 1 and int(matches[0]) > 0, 'invalid check count')
            counts.append(int(matches[0]))
        require(counts[0] == counts[1], 'normal/sanitized count differs')
        result['correctness_checks'] = {'normal': counts[0], 'sanitized': counts[1]}
        run('build-micro', flags+['-O3', root / 'benchmark_compensated_fixed.cpp', '-o', out / 'micro'], native_kind='compilation')
        result['pairs'] = []
        for index in range(4):
            for scenario in SCENARIOS:
                order = 'AB' if index % 2 == 0 else 'BA'
                row = json.loads(run(f'micro-{scenario}-{index}', [out / 'micro', scenario, order], native_kind='execution'))
                require(row['scenario'] == scenario and row['order'] == order and row['rows'] == 512 and row['repetitions'] == 64, 'micro plan differs')
                require(row['dimension'] == (512 if scenario == 'fullmantissa512' else 128), 'dimension differs')
                require(row['counter_instrumented'] is False and row['whole_call'] is True and row['untimed_row_oracle_checks'] == 512, 'measurement scope differs')
                measurements = row['measurements']
                require([m['role'] for m in measurements] == ['old', 'candidate'] and measurements[0]['checksum'] == measurements[1]['checksum'], 'roles/checksums differ')
                require(all(m['wall_ns'] > 0 and m['thread_cpu_ns'] > 0 for m in measurements), 'invalid clocks')
                row['pair'] = index
                result['pairs'].append(row)
                save()
        require(len(result['pairs']) == 12, 'missing pairs')
        result['groups'] = {}
        for scenario in SCENARIOS:
            rows = [p for p in result['pairs'] if p['scenario'] == scenario]
            require(len(rows) == 4 and len({p['dataset_fnv64'] for p in rows}) == 1, 'dataset drift')
            group = {}
            for metric in ('wall_ns', 'thread_cpu_ns'):
                ratios = [p['measurements'][1][metric]/p['measurements'][0][metric] for p in rows]
                group[metric] = {'candidate_over_old': ratios, 'median': median(ratios), 'candidate_worse_pairs': sum(r > 1 for r in ratios)}
            result['groups'][scenario] = group
        result['binary_sha256'] = {name: sha(out / name) for name in ('normal', 'sanitized', 'micro')}
        result['status'] = 'correctness_and_fixed_micro_completed'
        save()
    except Exception as exc:
        result.update(status='failed_or_unsupported', error=str(exc))
        save()
        raise


if __name__ == '__main__':
    main()
