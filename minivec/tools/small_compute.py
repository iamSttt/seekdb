#!/usr/bin/env python3
"""Bounded one/two-CPU paired diagnostics with explicit identity; no score claim."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
from statistics import median
import sys
import threading
import time
import uuid

CASES = ('standard10k', 'resident30k', 'filter60k', 'spill_high_q1', 'spill_high_q16', 'wide768')
PROTECTION_CASES = ('standard10k', 'resident30k', 'spill_high_q16', 'wide768')
SEED = 7319
CGROOT = Path('/sys/fs/cgroup')
FILES = ('cpu.max', 'cpu.stat', 'memory.max', 'memory.swap.max', 'memory.current',
         'memory.peak', 'memory.events', 'memory.events.local', 'io.stat', 'cgroup.events', 'cgroup.procs')
ENV_KEYS = ('MVDB_PROTOCOL', 'MVDB_OUT', 'MVDB_SINGLE_THREAD', 'MVDB_QUERY_THREADS',
            'MVDB_QUERY_FANOUT_ROWS', 'MVDB_WAKE_US_OVERRIDE', 'MVDB_PARSE_THREADS',
            'MVDB_ANN_PROBE_PERCENT', 'MVDB_ANN_OFF')


def require(ok, message):
    if not ok: raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def save(path, value):
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    tmp.replace(path)


def plan(cases=CASES):
    # AB followed by BA within each unchanged workload: four runs A B B A.
    return [{'scenario': name, 'seed': SEED, 'pair_index': index,
             'order': ['baseline', 'candidate'] if index == 0 else ['candidate', 'baseline']}
            for name in cases for index in range(2)]


def check_plan(items, cases=CASES):
    require(cases in (CASES, PROTECTION_CASES), 'unknown fixed plan')
    require(items == plan(cases), 'expected fixed workloads, one seed, two AB/BA pairs each')
    require(len({(p['scenario'], p['seed'], p['pair_index']) for p in items}) == 2 * len(cases), 'duplicate pair')


def insert_copy_plan(reference, count):
    """Separate frozen screen: 16 direct retained pairs + 8 historical guards."""
    require(reference in ('retained', 'ee') and count in (1, 2), 'unknown INSERT screen')
    specs = [('standard10k', 6 if reference == 'retained' else 4)]
    if reference == 'retained': specs.append(('spill_high_q16' if count == 1 else 'wide768', 2))
    return [{'scenario': name, 'seed': SEED, 'pair_index': index,
             'order': ['baseline', 'candidate'] if index % 2 == 0 else ['candidate', 'baseline']}
            for name, pairs in specs for index in range(pairs)]


def check_insert_copy_plan(items, reference, count):
    require(items == insert_copy_plan(reference, count), 'INSERT fixed plan/order differs')


def thread_control_plan():
    """Eight standard10k pairs, interleaving both policies and both orders."""
    items = []
    for index in range(4):
        for policy in (('default', 'single') if index % 2 == 0 else ('single', 'default')):
            forward = (index % 2 == 0) == (policy == 'default')
            items.append({'scenario': 'standard10k', 'seed': SEED, 'pair_index': index,
                          'order': ['baseline', 'candidate'] if forward else ['candidate', 'baseline'],
                          'policy': policy})
    return items


def check_thread_control_plan(items):
    require(items == thread_control_plan(), 'thread-control fixed plan/order/policy differs')


def thread_control_environment(policy):
    require(policy in ('default', 'single'), 'unknown thread-control policy')
    return {} if policy == 'default' else {'MVDB_PARSE_THREADS': '1', 'MVDB_QUERY_THREADS': '1'}


def matched_insert_plan(count):
    """Independent same-build screen: four standard10k pairs per CPU count."""
    require(type(count) is int and count in (1, 2), 'matched INSERT roots require one or two CPUs')
    return [{'scenario': 'standard10k', 'seed': SEED, 'pair_index': index,
             'order': ['baseline', 'candidate'] if index % 2 == 0 else ['candidate', 'baseline']}
            for index in range(4)]


def check_matched_insert_plan(items, count):
    require(items == matched_insert_plan(count), 'matched INSERT roots fixed plan/order differs')


def cpu_set(text):
    # /proc reports adjacent CPUs as a range; taskset accepts either spelling.
    require(isinstance(text, str) and text, 'empty CPU list')
    result = set()
    for part in text.split(','):
        require(re.fullmatch(r'[0-9]+(?:-[0-9]+)?', part) is not None, 'invalid CPU list')
        ends = part.split('-'); first = int(ends[0]); last = int(ends[-1])
        require(0 <= first <= last <= 1048575, 'invalid CPU range')
        current = set(range(first, last + 1))
        require(not result.intersection(current), 'duplicate CPU in list')
        result.update(current)
    return result


def select_cpus(affinity, count, first=None):
    allowed = sorted(set(affinity))
    require(count in (1, 2) and allowed, 'unsupported CPU count or empty affinity')
    if first is None: first = allowed[0]
    require(first in allowed, 'DB CPU must be in inherited allowed set')
    index = allowed.index(first)
    chosen = allowed[index:index + count]
    require(len(chosen) == count, 'not enough inherited CPUs for requested DB affinity')
    return chosen


def pointer(value, keys):
    require(type(keys) is list and keys and all(type(k) is str for k in keys), 'invalid evidence pointer')
    for key in keys: value = value[key]
    return value


def identities(path, allow_versions):
    data = json.loads(path.read_bytes())
    require(set(data) == {'baseline', 'candidate'}, 'expected two explicit identities')
    for role, item in data.items():
        require(re.fullmatch('[0-9a-f]{40}', item['source_commit']) is not None, 'full source commit required')
        binary = (path.parent / item['binary']).resolve()
        evidence = (path.parent / item['evidence_file']).resolve()
        require(sha(binary) == item['binary_sha256'], 'binary SHA mismatch')
        require(sha(evidence) == item['evidence_sha256'], 'identity evidence SHA mismatch')
        proof = json.loads(evidence.read_bytes())
        require(proof.get('status') == 'verified', 'independent verified identity evidence required')
        require(pointer(proof, item['evidence_binary_pointer']) == item['binary_sha256'], 'evidence binary mismatch')
        require(pointer(proof, item['evidence_commit_pointer']) == item['source_commit'], 'evidence source mismatch')
        env = item.setdefault('environment', {} if role == 'baseline' else {'MVDB_QUERY_THREADS': '1'})
        require(type(env) is dict and set(env) <= set(ENV_KEYS), 'unknown environment override')
        require(all(type(v) is str and '\0' not in v for v in env.values()), 'invalid environment value')
        item.update(binary=str(binary), evidence_file=str(evidence))
    if not allow_versions:
        require(data['baseline']['binary_sha256'] == data['candidate']['binary_sha256'], 'default requires same-byte control')
        require(data['baseline']['source_commit'] == data['candidate']['source_commit'], 'same-byte control needs same declared source')
    return data


def snapshot(group):
    out = {'monotonic_seconds': time.monotonic()}
    for name in FILES:
        try: out[name] = (group / name).read_text().strip()
        except OSError as exc: out[name] = {'unavailable': str(exc)}
    return out


def counters(raw):
    if not isinstance(raw, str): return {}
    return {parts[0]: int(parts[1]) for line in raw.splitlines()
            if len(parts := line.split()) == 2 and parts[1].isdigit()}


def io_counters(raw):
    if not isinstance(raw, str): return {}
    return {parts[0]: {k: int(v) for k, v in (field.split('=', 1) for field in parts[1:])}
            for line in raw.splitlines() if (parts := line.split())}


def resource_delta(before, after):
    b, a = counters(before['cpu.stat']), counters(after['cpu.stat'])
    ib, ia = io_counters(before['io.stat']), io_counters(after['io.stat'])
    return {'cpu_stat_delta': {k: a[k] - b.get(k, 0) for k in a},
            'memory_peak_bytes': int(after['memory.peak']),
            'io_stat_delta': {dev: {k: v - ib.get(dev, {}).get(k, 0) for k, v in fields.items()}
                              for dev, fields in ia.items()}}


def task_snapshot(pid, extended=False):
    result = {'pid': pid, 'native_tasks': [], 'clock_ticks_per_second': os.sysconf('SC_CLK_TCK')}
    try:
        for path in sorted((Path('/proc') / str(pid) / 'task').iterdir(), key=lambda p: int(p.name)):
            try:
                fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
                status_lines = (path / 'status').read_text().splitlines()
                allowed = next(line.split(':', 1)[1].strip() for line in status_lines if line.startswith('Cpus_allowed_list:'))
                task = {'tid': int(path.name), 'main': int(path.name) == pid,
                    'cpu_ticks': int(fields[11]) + int(fields[12]), 'starttime_ticks': int(fields[19]),
                    'last_processor': int(fields[36]), 'cpus_allowed_list': allowed}
                if extended:
                    task.update(minflt=int(fields[7]), majflt=int(fields[9]))
                    for key in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches'):
                        task[key] = int(next(line.split(':', 1)[1].strip() for line in status_lines if line.startswith(key + ':')))
                    require(all(task[key] >= 0 for key in ('minflt', 'majflt', 'voluntary_ctxt_switches',
                                                          'nonvoluntary_ctxt_switches')), 'negative task counter')
                result['native_tasks'].append(task)
            except (OSError, ValueError, IndexError, StopIteration) as exc:
                result.setdefault('task_read_errors', []).append({'tid': path.name, 'error': str(exc)})
    except OSError as exc: result['native_tasks_error'] = str(exc)
    if extended:
        result['extended_counter_scope'] = 'Point-in-time live native tasks; exited task counters are not recovered. Match TID and starttime across boundaries.'
    return result


def ancestors(group):
    out = []
    for parent in (group, *group.parents):
        if parent == CGROOT or parent.is_relative_to(CGROOT):
            out.append({'path': str(parent), **snapshot(parent)})
        if parent == CGROOT: break
    return out


@contextmanager
def environment(overrides):
    prior = {k: os.environ.get(k) for k in ENV_KEYS}
    try:
        for k in ENV_KEYS: os.environ.pop(k, None)
        os.environ.update(overrides)
        yield
    finally:
        for k in ENV_KEYS:
            if prior[k] is None: os.environ.pop(k, None)
            else: os.environ[k] = prior[k]


class Group:
    def __init__(self, parent, args, record):
        self.path = parent / ('case-' + uuid.uuid4().hex)
        self.args, self.record = args, record
        self.timer = None

    def __enter__(self):
        self.path.mkdir()
        try:
            (self.path / 'cpu.max').write_text(f'{self.args.cpu_quota_us} {self.args.cpu_period_us}')
            # Missing limits are inherited, not described as unlimited.
            for arg, file in (('memory_max', 'memory.max'), ('swap_max', 'memory.swap.max')):
                value = getattr(self.args, arg)
                if value is not None: (self.path / file).write_text(value)
            require((self.path / 'cgroup.kill').exists(), 'cgroup.kill required for bounded failure cleanup')
            self.record.update(path=str(self.path), before=snapshot(self.path), ancestor_constraints=ancestors(self.path),
                               timeout=False, watchdog_error=None)
            require(all(isinstance(self.record['before'][key], str) for key in
                        ('cpu.stat', 'memory.peak', 'memory.events', 'io.stat')), 'required cgroup telemetry unavailable')
            for arg, file in (('memory_max', 'memory.max'), ('swap_max', 'memory.swap.max')):
                wanted = getattr(self.args, arg)
                require(wanted is None or self.record['before'][file] == wanted, 'actual requested memory/swap cap differs')
            self.timer = threading.Timer(self.args.case_timeout, self.expire)
            self.timer.daemon = True
            self.timer.start()
            return self
        except BaseException:
            self.path.rmdir()
            raise

    def expire(self):
        self.record['timeout'] = True
        try: (self.path / 'cgroup.kill').write_text('1')
        except OSError as exc: self.record['watchdog_error'] = str(exc)

    def __exit__(self, exc_type, exc, tb):
        self.timer.cancel(); self.timer.join()
        if exc is not None: self.record['execution_error'] = f'{type(exc).__name__}: {exc}'
        # Capture OOM/timeout evidence even when the protocol/oracle raised.
        self.record['immediately_after'] = snapshot(self.path)
        if (self.path / 'cgroup.procs').read_text().strip():
            self.record['cleanup_kill_required'] = True
            (self.path / 'cgroup.kill').write_text('1')
            for _ in range(50):
                if not (self.path / 'cgroup.procs').read_text().strip(): break
                time.sleep(.02)
        prior, stable = None, 0
        for _ in range(20):
            after = snapshot(self.path)
            stable = stable + 1 if after['io.stat'] == prior else 0
            prior = after['io.stat']
            if stable >= 3: break
            time.sleep(.1)
        self.record.update(after=after, io_stable_samples=stable)
        self.record['resource_delta'] = resource_delta(self.record['before'], after)
        b, a = counters(self.record['before']['memory.events']), counters(after['memory.events'])
        self.record['oom_events_delta'] = {k: a.get(k, 0) - b.get(k, 0) for k in ('oom', 'oom_kill', 'oom_group_kill')}
        self.record['request_timeout_observed'] = bool(exc_type and issubclass(exc_type, TimeoutError))
        require(not (self.path / 'cgroup.procs').read_text().strip(), 'live processes remain in owned cgroup')
        self.path.rmdir()


@contextmanager
def command_adapter(base, projection, prefix, binary, group, trace, db_cpu=None,
                    expected_environment=None, ingest_task_counters=False):
    """Scoped constructor hooks; no source edit and no wrapper survives a run."""
    original_base, original_projection, original_snapshot, original_exchange = (
        base.Session, projection.Session, base.process_snapshot, projection.exchange)

    def argv(command):
        command = list(command)
        if command[:len(prefix)] == prefix: command = command[len(prefix):]
        require(command and command[0] == str(binary), 'unexpected or repeated command prefix')
        return prefix + command

    def observed_ready(session):
        pid = session.proc.pid
        members = {int(p) for p in (group / 'cgroup.procs').read_text().split()}
        require(pid in members and os.getpid() not in members, 'DB/driver cgroup isolation not established')
        actual_exe = Path(os.readlink(f'/proc/{pid}/exe')).resolve()
        require(actual_exe == binary.resolve(), 'actual process executable is not declared DB')
        allowed = next(line.split(':', 1)[1].strip() for line in Path(f'/proc/{pid}/status').read_text().splitlines()
                       if line.startswith('Cpus_allowed_list:'))
        expected = {db_cpu} if isinstance(db_cpu, int) else set(db_cpu or [])
        require(db_cpu is None or cpu_set(allowed) == expected, 'database affinity differs from requested CPUs')
        observation = {'pid': pid, 'members': sorted(members), 'driver_pid': os.getpid(),
                       'actual_exe': str(actual_exe), 'cpus_allowed_list': allowed}
        if expected_environment is not None:
            actual = {}
            for item in Path(f'/proc/{pid}/environ').read_bytes().split(b'\0'):
                key, separator, value = item.partition(b'=')
                if separator and key.decode('utf8', errors='replace') in ENV_KEYS:
                    name = key.decode('utf8')
                    require(name not in actual, 'duplicate actual runtime environment key')
                    actual[name] = value.decode('utf8')
            require(actual == expected_environment, 'actual DB runtime environment differs from declared diagnostic policy')
            observation['runtime_environment'] = actual
        trace.setdefault('ready_memberships', []).append(observation)

    class Lifecycle(original_base):
        def __init__(self, *args, **kwargs):
            kwargs['command'] = argv(kwargs['command'])
            super().__init__(*args, **kwargs)
        def start(self, *args, **kwargs):
            value = super().start(*args, **kwargs); observed_ready(self); return value
        def request(self, *args, **kwargs):
            mode = kwargs.get('mode', args[1] if len(args) > 1 else 'exact')
            require(mode in ('exact', 'ann'), 'invalid forwarded search mode')
            value = super().request(*args, **kwargs)
            modes = trace.setdefault('completed_request_modes', {})
            modes[mode] = modes.get(mode, 0) + 1
            trace['completed_transport_requests'] = trace.get('completed_transport_requests', 0) + 1
            return value
        def close(self):
            try: return super().close()
            finally:
                if Path(self.log.name).exists(): trace['stderr'] = base.stderr_tail(Path(self.log.name))

    class Projected(original_projection):
        def __init__(self, command, *args, **kwargs): super().__init__(argv(command), *args, **kwargs)
        def read_json(self, context):
            value = super().read_json(context)
            if context == 'ready': observed_ready(self)
            return value

    def process_snapshot(session, phase):
        value = original_snapshot(session, phase)
        extended = ingest_task_counters and phase in ('before_ingest', 'after_ingest')
        trace.setdefault('phase_cgroups', []).append({'phase': phase, **snapshot(group),
                                                     **task_snapshot(session.proc.pid, extended=extended)})
        return value

    counts = {}
    def exchange(session, sql, *, expected=None, affected=None, search_mode="exact"):
        label = expected['label'] if expected else None
        if label and counts.get(label, 0) == 0:
            trace.setdefault('phase_cgroups', []).append({'phase': label + '/before', **snapshot(group), **task_snapshot(session.proc.pid)})
        value = original_exchange(session, sql, expected=expected, affected=affected, search_mode=search_mode)
        require(value['search_mode'] == search_mode, 'projection actual mode differs')
        modes = trace.setdefault('completed_request_modes', {})
        modes[search_mode] = modes.get(search_mode, 0) + 1
        trace['completed_transport_requests'] = trace.get('completed_transport_requests', 0) + 1
        if label:
            counts[label] = counts.get(label, 0) + 1
            if counts[label] == 3:
                trace['phase_cgroups'].append({'phase': label + '/after', **snapshot(group), **task_snapshot(session.proc.pid)})
        return value

    base.Session, projection.Session = Lifecycle, Projected
    base.process_snapshot, projection.exchange = process_snapshot, exchange
    try: yield
    finally:
        base.Session, projection.Session = original_base, original_projection
        base.process_snapshot, projection.exchange = original_snapshot, original_exchange


def workload(name, base, writes, projection, query_mode="mixed"):
    if name == 'wide768': return projection.workload(name, SEED, query_mode=query_mode)
    if name.startswith('spill_'): return writes.make_workload(name, SEED, cycles=4, batch_size=100, query_mode=query_mode)
    return base.make_workload(base.FULL_CASES[name], SEED, 1000, 100, 10, query_mode=query_mode)


def workload_identity(name, work, base):
    if name == 'wide768':
        payload = {'requests': work['requests'], 'phases': work['phases']}
    elif name.startswith('spill_'):
        payload = {key: work[key] for key in ('create', 'inserts', 'before', 'cycles', 'after', 'changed_ids', 'padding_value')}
        payload['restart_ids'] = sorted(work['final_model'])
        payload['restart_payloads'] = {key: [list(work['final_model'][key][0]), *work['final_model'][key][1:]]
                                       if key in work['final_model'] else None for key in work['changed_ids']}
    else:
        payload = {key: work[key] for key in ('create', 'inserts', 'queries', 'update', 'updated_rows',
                   'before_oracle', 'after_oracle', 'oracle_indices', 'query_counts')}
        samples = sorted({0, 1, work['rows'] // 2, work['rows'] - 1, max(0, work['updated_rows'] - 1)})
        payload['restart_sample_vectors'] = {key: list(work['updated_vector'] if key < work['updated_rows'] else work['vectors'][key])
                                             for key in samples}
    payload['query_mode_policy'] = work.get('query_mode_policy', 'mixed')
    if 'queries' in work:
        payload['query_modes'] = work.get('query_modes', ['exact' if payload['query_mode_policy'] == 'exact' or i % 2 == 0 else 'ann'
                                                        for i in range(len(work['queries']))])
    if name == 'wide768': payload['search_mode'] = work.get('search_mode', 'exact')
    digest = hashlib.sha256(base.encoded(payload)).hexdigest()
    return {'scenario': name, 'seed': SEED, 'sha256': digest, 'generator_sha256': work.get('sha256'),
            'rows': work['rows'], 'dimension': work['dimension']}


def compare(pair):
    b, c = pair['baseline'], pair['candidate']
    if pair['scenario'] == 'wide768':
        require(set(b['phases']) == set(c['phases']) and len(b['phases']) == 7, 'projection phases differ')
        for name in b['phases']:
            left, right = b['phases'][name]['requests'], c['phases'][name]['requests']
            require(len(left) == len(right) == 3, 'projection request count differs')
            require(all((x['response_sha256'], x['response_bytes']) == (y['response_sha256'], y['response_bytes'])
                        for x, y in zip(left, right)), 'projection output differs')
    else: require(b['response_sha256'] == c['response_sha256'], 'lifecycle response hash differs')
    pair['all_responses_identical'] = True


def timing_values(run):
    if 'phase_seconds' in run:
        values = dict(run['phase_seconds'])
        for phase, data in run['query'].items():
            samples = data['per_request_seconds']
            for label, subset in (('first', samples[:1]), ('front64', samples[:64]), ('after64', samples[64:])):
                if subset: values[phase + '/' + label] = math.fsum(subset)
        return values
    return {**{'lifecycle/' + k: v for k, v in run['lifecycle_seconds'].items()},
            **{'query/' + k: v['wall_seconds'] for k, v in run['phases'].items()},
            'accounted_timer_sum': math.fsum(run['lifecycle_seconds'].values()) + math.fsum(p['wall_seconds'] for p in run['phases'].values())}


def summary(pairs):
    out = {}
    for p in pairs:
        b, c = timing_values(p['baseline']), timing_values(p['candidate'])
        require(set(b) == set(c), 'phase set mismatch')
        for k in b:
            require(math.isfinite(b[k]) and math.isfinite(c[k]) and b[k] >= 0 and c[k] > 0, 'invalid phase timer')
            name = p['scenario'] + '/' + p['policy'] if 'policy' in p else p['scenario']
            out.setdefault(name, {}).setdefault(k, []).append({
                'pair_index': p['pair_index'], 'order': p['order'], 'baseline_seconds': b[k], 'candidate_seconds': c[k],
                'baseline_over_candidate': b[k] / c[k], 'candidate_slower': c[k] > b[k]})
    return {name: {phase: {'raw_pairs': rows, 'median_baseline_over_candidate': median(r['baseline_over_candidate'] for r in rows),
                           'candidate_slower_pairs': sum(r['candidate_slower'] for r in rows)} for phase, rows in phases.items()}
            for name, phases in out.items()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tools', type=Path, required=True)
    p.add_argument('--identities', type=Path)
    p.add_argument('--launcher', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--plan-only', action='store_true')
    p.add_argument('--query-mode', choices=('mixed', 'exact'), default='mixed')
    p.add_argument('--allow-version-contrast', action='store_true')
    p.add_argument('--cpu-quota-us', type=int, default=100000)
    p.add_argument('--cpu-period-us', type=int, default=100000)
    p.add_argument('--db-cpu', type=int, help='first CPU from inherited affinity; default minimum allowed CPU')
    p.add_argument('--db-cpu-count', type=int, choices=(1, 2), default=1)
    p.add_argument('--protection-only', action='store_true', help='four fixed workloads, eight pairs; requires two DB CPUs')
    p.add_argument('--insert-copy-reference', choices=('retained', 'ee'),
                   help='separate fixed 24-pair INSERT screen, never changes the historical bridge plan')
    p.add_argument('--insert-thread-control', action='store_true',
                   help='separate eight-pair default/single-thread diagnostic, imported binaries only')
    p.add_argument('--matched-insert-roots', action='store_true',
                   help='separate four-pair standard10k screen per CPU, using matched root builds')
    p.add_argument('--memory-max'); p.add_argument('--swap-max')
    p.add_argument('--timeout', type=float, default=120)
    p.add_argument('--case-timeout', type=float, default=1800)
    args = p.parse_args()
    require(not args.output.exists(), 'refuse overwrite')
    require(1000 <= args.cpu_period_us <= 1000000 and args.cpu_quota_us >= 1000, 'invalid quota/period')
    require(all(math.isfinite(v) and v > 0 for v in (args.timeout, args.case_timeout)), 'finite positive timeouts required')
    for value in (args.memory_max, args.swap_max):
        require(value is None or value == 'max' or (value.isdigit() and int(value) >= 0), 'limit must be bytes or max')
    require(not args.protection_only or args.db_cpu_count == 2, 'protection plan requires two DB CPUs')
    cases = PROTECTION_CASES if args.protection_only else CASES
    if args.matched_insert_roots:
        require(not args.insert_thread_control and not args.insert_copy_reference and not args.protection_only and
                args.allow_version_contrast and args.query_mode == 'exact' and
                args.cpu_quota_us == args.db_cpu_count * 100000 and args.cpu_period_us == 100000 and
                args.memory_max == '2147483648' and args.swap_max == '0', 'matched INSERT roots require their fixed exact/CPU/2GiB/swap0 plan')
        items = matched_insert_plan(args.db_cpu_count); check_matched_insert_plan(items, args.db_cpu_count)
        cases = ('standard10k',)
    elif args.insert_thread_control:
        require(not args.insert_copy_reference and not args.protection_only and args.allow_version_contrast and
                args.query_mode == 'exact' and args.db_cpu_count == 1 and args.cpu_quota_us == args.cpu_period_us == 100000 and
                args.memory_max == '2147483648' and args.swap_max == '0', 'thread control requires its fixed exact/1CPU/2GiB/swap0 plan')
        items = thread_control_plan(); check_thread_control_plan(items)
        cases = ('standard10k',)
    elif args.insert_copy_reference:
        require(not args.protection_only and args.allow_version_contrast and args.query_mode == 'exact',
                'INSERT screen requires exact version contrast and its own fixed plan')
        items = insert_copy_plan(args.insert_copy_reference, args.db_cpu_count)
        cases = tuple(dict.fromkeys(item['scenario'] for item in items))
    else:
        items = plan(cases); check_plan(items, cases)
    args.ingest_telemetry = bool(args.insert_copy_reference or args.insert_thread_control or args.matched_insert_roots)
    expected_pairs = len(items)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.plan_only:
        preview = {'status': 'plan_only_not_executed', 'pairs': items, 'expected_pairs': expected_pairs, 'query_mode_policy': args.query_mode}
        if args.matched_insert_roots: preview['matched_insert_roots'] = True
        save(args.output, preview); return
    require(sys.platform.startswith('linux') and args.identities and args.launcher, 'Linux, identities and launcher required')
    source = args.tools.resolve(); sys.path.insert(0, str(source))
    base, delta, writes, projection = [importlib.import_module(name) for name in
        ('benchmark_iteration', 'benchmark_index_delta', 'benchmark_write_query', 'benchmark_projection')]
    ids = identities(args.identities.resolve(), args.allow_version_contrast)
    if args.insert_thread_control or args.matched_insert_roots:
        require(all(item['environment'] == {} for item in ids.values()), 'diagnostic identities must have no hidden overrides')
    require(all(os.access(item['binary'], os.X_OK) for item in ids.values()), 'validated binaries must be executable')
    launcher = args.launcher.resolve(); require(os.access(launcher, os.X_OK), 'launcher must be executable')
    taskset = Path(shutil.which('taskset') or '/nonexistent-taskset').resolve()
    require(taskset.is_file() and os.access(taskset, os.X_OK), 'taskset required for DB-only affinity')
    affinity = sorted(os.sched_getaffinity(0))
    args.db_cpus = select_cpus(affinity, args.db_cpu_count, args.db_cpu)
    args.db_cpu = args.db_cpus[0]
    report = {'status': 'running', 'kind': 'small_compute_diagnostic', 'official_score': None,
        'official_binary_identity_known': False, 'query_mode_policy': args.query_mode, 'expected_pairs': expected_pairs, 'plan': items, 'pairs': [],
        'insert_copy_reference': args.insert_copy_reference,
        'insert_thread_control': args.insert_thread_control,
        'matched_insert_roots': args.matched_insert_roots,
        'comparison_kind': 'whole_version_diagnostic' if args.allow_version_contrast else 'same_binary_environment_control',
        'identities': ids, 'identity_record_sha256': sha(args.identities), 'launcher_sha256': sha(launcher),
        'taskset': {'path': str(taskset), 'sha256': sha(taskset)}, 'db_cpu': args.db_cpu,
        'db_cpus': args.db_cpus, 'db_cpu_count': args.db_cpu_count, 'protection_only': args.protection_only,
        'requested_limits': {k: getattr(args, k) for k in ('cpu_quota_us', 'cpu_period_us', 'memory_max', 'swap_max')},
        'cleared_inherited_runtime_environment': {key: os.environ[key] for key in ENV_KEYS if key in os.environ},
        'host': {'platform': platform.platform(), 'driver_affinity': sorted(os.sched_getaffinity(0))},
        'tool_hashes': {str(path): sha(path) for path in {Path(__file__).resolve(),
            *(Path(m.__file__).resolve() for m in (base, delta, writes, projection)),
            Path(sys.modules[base.Session.__module__].__file__).resolve(),
            Path(sys.modules[projection.Session.__module__].__file__).resolve(), base.PROTOCOL_CHECK / 'config.json'}},
        'workloads': [], 'limitations': [
            'Diagnostic only: requested one/two-CPU and memory/swap limits are explicit, not a verified official resource envelope. Ancestor constraints are retained.',
            'DB quota and selected affinity are explicit; affinity is checked after every ready. Driver affinity is unchanged; driver/oracle stay outside DB cgroup but share host resources.',
            'Comparison kind and both binary/source/environment identities are explicit. A version contrast includes compiler/layout effects; neither comparison proves the official binary identity.',
            'Two AB/BA pairs at one fixed seed screen only. No confidence interval, score conversion or automatic adoption.',
            'Cgroup totals include DB restart verification, filesystem page cache and static launcher; request timers exclude independent oracle work.',
            'Ready timing includes membership/executable/affinity evidence reads. Projection phase snapshots are outside individual request timers.',
            'Phase task snapshots retain TID/starttime/CPU ticks/affinity. Nonmain CPU includes parser work; it is not exclusively query-worker CPU.',
            'IO counters use the shared host filesystem; bounded post-exit stability is not dedicated-device writeback proof.',
            'Iteration keeps its existing first/second/last TopK oracle, all restart IDs and sampled vectors; write-query and projection retain their stronger full oracles.',
            'Base iteration errors may not expose partial phase timers; adapter preserves transport count, stderr, cgroup evidence and preceding completed side.']}
    save(args.output, report)
    if args.insert_copy_reference:
        report['limitations'][3] = 'INSERT screen: six retained and four historical default pairs per CPU, two retained protection pairs per selected case, one fixed seed. Screening only, no statistical floor, score conversion or automatic adoption.'
    if args.insert_thread_control:
        report['limitations'][3] = 'Thread control: four default and four single-thread standard10k pairs, interleaved policies, two AB and two BA per policy, one seed. Explicit parse/query=1 on both sides under single policy; not an adoption or scoring experiment.'
        report['limitations'].append('Ingest task faults/context switches are live-task boundary counters, not all exited threads or isolated allocator/syscall costs.')
    if args.matched_insert_roots:
        report['limitations'][3] = 'Matched-root INSERT screen: four standard10k AB/BA pairs at this CPU count, one seed, no environment override. Build equivalence must be established by the separate preparation proof; no adoption or score claim.'
    parent, current_pair, failure = CGROOT / ('minivec-report-' + uuid.uuid4().hex), None, None
    try:
        parent.mkdir()
        require(not (parent / 'cgroup.procs').read_text().strip(), 'private parent unexpectedly populated')
        (parent / 'cgroup.subtree_control').write_text('+cpu +memory +io')
        report['driver_cgroup'] = Path('/proc/self/cgroup').read_text().strip()
        for name in cases:
            work = workload(name, base, writes, projection, args.query_mode)
            report['workloads'].append(workload_identity(name, work, base))
            for entry in (v for v in items if v['scenario'] == name):
                current_pair = dict(entry)
                for role in entry['order']:
                    print(name, entry.get('policy', ''), entry['pair_index'], role, flush=True)
                    partial = {'cgroup': {}, 'adapter': {}}
                    current_pair[role] = partial
                    effective_environment = (thread_control_environment(entry['policy']) if args.insert_thread_control
                                             else ids[role]['environment'])
                    if args.insert_thread_control:
                        partial.update(effective_environment=dict(effective_environment), thread_control_policy=entry['policy'])
                    elif args.matched_insert_roots:
                        partial['effective_environment'] = dict(effective_environment)
                    with environment(effective_environment), Group(parent, args, partial['cgroup']) as group:
                        args.command_prefix = [str(launcher), str(group.path), str(taskset), '--cpu-list', ','.join(map(str, args.db_cpus))]
                        binary = Path(ids[role]['binary'])
                        require(sha(binary) == ids[role]['binary_sha256'], 'binary changed before run')
                        with command_adapter(base, projection, args.command_prefix, binary, group.path, partial['adapter'], args.db_cpus,
                                             expected_environment=effective_environment if args.insert_thread_control or args.matched_insert_roots else None,
                                             ingest_task_counters=args.insert_thread_control):
                            if name == 'wide768':
                                args.repetitions = 3
                                projection.one_run(binary, work, args, partial)
                            elif name.startswith('spill_'): delta.one_run(binary, work, args, partial)
                            else: partial.update(base.one_run(binary, work, args))
                    require(not partial['cgroup']['timeout'] and not partial['cgroup'].get('cleanup_kill_required') and
                            not any(partial['cgroup']['oom_events_delta'].values()), 'timeout, live-process cleanup or OOM observed')
                    require(sha(binary) == ids[role]['binary_sha256'], 'binary changed after run')
                    partial['declared_environment'] = dict(effective_environment)
                    stderr = partial.get('stderr', partial['adapter'].get('stderr', {}))
                    tail = stderr.get('tail_utf8', stderr.get('tail', ''))
                    partial['thread_policy_observations'] = {
                        'requested_query_threads': effective_environment.get('MVDB_QUERY_THREADS', 'automatic'),
                        'fanout_log_lines': [line for line in tail.splitlines() if 'fanout ' in line],
                        'stderr_complete': not stderr.get('truncated', stderr.get('bytes', 0) > 4096),
                        'task_phase_snapshots': len(partial['adapter'].get('phase_cgroups', [])),
                        'scope': 'Observed decisions and task counters. Nonmain CPU is not exclusively query worker CPU.'}
                    if args.insert_thread_control:
                        partial['thread_policy_observations']['requested_parse_threads'] = effective_environment.get('MVDB_PARSE_THREADS', 'automatic')
                compare(current_pair)
                report['pairs'].append(current_pair); report['summary'] = summary(report['pairs']); save(args.output, report)
            require(workload_identity(name, work, base) == report['workloads'][-1], 'workload mutated during paired runs')
        require(len(report['pairs']) == expected_pairs, 'incomplete plan')
        keys = ('scenario', 'seed', 'pair_index', 'order', 'policy') if args.insert_thread_control else ('scenario', 'seed', 'pair_index', 'order')
        measured_plan = [{key: pair[key] for key in keys} for pair in report['pairs']]
        if args.matched_insert_roots: check_matched_insert_plan(measured_plan, args.db_cpu_count)
        elif args.insert_thread_control: check_thread_control_plan(measured_plan)
        elif args.insert_copy_reference: check_insert_copy_plan(measured_plan, args.insert_copy_reference, args.db_cpu_count)
        else: check_plan(measured_plan, cases)
        report['status'] = 'passed'
    except BaseException as exc:
        failure = exc
        report.update(status='failed', error=f'{type(exc).__name__}: {exc}', failed_pair=current_pair)
    finally:
        try:
            if parent.exists(): parent.rmdir()
        except OSError as exc:
            report['cleanup_error'] = str(exc); report['status'] = 'failed'
            if failure is None: failure = exc
        report['completed_pairs'] = len(report['pairs'])
        save(args.output, report)
    if failure is not None: raise failure


if __name__ == '__main__': main()
