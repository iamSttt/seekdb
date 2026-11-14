#!/usr/bin/env python3
"""External perf sampling of the actual EE ELF; independent of timing comparisons."""
import argparse
import base64
from collections import Counter
from contextlib import contextmanager
import gzip
import importlib
import json
import os
from pathlib import Path
import platform
import re
import select
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import traceback
from types import SimpleNamespace
import uuid

from best_screen_1114 import ROOT, TOOLS, EE, require, save, sha, verify_vendor

EE_SHA = '9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd'
REPETITIONS = 12
FREQUENCY = 999
MIN_PHASE_SAMPLES = 300
MAX_RAW_ARCHIVE_BYTES = 8 * 1024 * 1024
INDEX_SQL = 'CREATE VECTOR INDEX docs_v ON docs(v);'
DOCS = [
    'https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-record.txt',
    'https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-stat.txt',
    'https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf.txt']


class ProfileBlocked(RuntimeError):
    pass


@contextmanager
def no_mvdb_environment():
    prior = {k: v for k, v in os.environ.items() if k.startswith('MVDB_')}
    try:
        for key in prior:
            os.environ.pop(key, None)
        yield
    finally:
        for key in list(os.environ):
            if key.startswith('MVDB_'):
                os.environ.pop(key, None)
        os.environ.update(prior)


def archive_raw(output, summary):
    """Transport raw binary evidence through the existing CI text-log channel."""
    for path in output.glob('run-*/**/perf.data'):
        compressed = path.with_name('perf.data.gz')
        if not compressed.exists():
            with path.open('rb') as src, gzip.open(compressed, 'wb') as dst:
                shutil.copyfileobj(src, dst)
    members = []
    for path in output.rglob('*'):
        if not path.is_file() or 'buildid-cache' in path.parts:
            continue
        if path.name in ('perf.data', 'symbol-strip-verification') or path.name.startswith('perf-raw'):
            continue
        if path.suffix in ('.json', '.txt', '.log', '.stdout', '.stderr', '.gz'):
            members.append(path)
    archive = output / 'perf-raw.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        for path in sorted(members):
            tar.add(path, arcname=path.relative_to(output).as_posix(), recursive=False)
    metadata = {'archive': archive.name, 'archive_bytes': archive.stat().st_size,
                'archive_sha256': sha(archive), 'maximum_encoded_archive_bytes': MAX_RAW_ARCHIVE_BYTES,
                'raw_perf_data_files': sum(p.name == 'perf.data.gz' for p in members),
                'members': [{'path': p.relative_to(output).as_posix(), 'bytes': p.stat().st_size,
                             'sha256': sha(p)} for p in sorted(members)],
                'raw_archive_transported': False,
                'scope': 'Manifest and base64 text emitted by the existing CI report trap; decode bytes and verify SHA256 before extracting.'}
    if archive.stat().st_size <= MAX_RAW_ARCHIVE_BYTES:
        encoded = output / 'perf-raw.tar.gz.b64.txt'
        with archive.open('rb') as src, encoded.open('wb') as dst:
            while True:
                block = src.read(57)
                if not block:
                    break
                dst.write(base64.b64encode(block) + b'\n')
        metadata.update(status='prepared_for_text_transport', base64_file=encoded.name,
                        base64_bytes=encoded.stat().st_size, base64_sha256=sha(encoded),
                        raw_archive_transport_prepared=True)
    else:
        metadata.update(status='over_limit_raw_not_archived_to_logs', raw_archive_transport_prepared=False,
                        reason='Raw compressed archive exceeds fixed8MiB bound; no partial/truncated base64 emitted.')
    save(output / 'perf-raw-archive.json', metadata)
    summary['raw_archive'] = {k: v for k, v in metadata.items() if k != 'members'}


def checked(command, directory, label, environment, timeout=60):
    """Retain complete tool output, including unavailable/unsupported failures."""
    stdout, stderr = directory / (label + '.stdout'), directory / (label + '.stderr')
    record = {'command': list(map(str, command)), 'stdout': str(stdout), 'stderr': str(stderr)}
    with stdout.open('wb') as out, stderr.open('wb') as err:
        try:
            result = subprocess.run(command, stdout=out, stderr=err, env=environment,
                                    timeout=timeout, check=False)
            record['exit_code'] = result.returncode
        except Exception as exc:
            record.update(exit_code=None, error=type(exc).__name__ + ': ' + str(exc))
    save(directory / (label + '.command.json'), record)
    if record['exit_code'] != 0:
        raise ProfileBlocked(label + ' unavailable/failed; see ' + str(stderr))
    return stdout.read_text(encoding='utf-8', errors='replace'), record


class PerfProcess:
    """An attached perf collector, enabled only after acknowledged fd control."""
    def __init__(self, prefix, kind, pid, directory, environment):
        self.kind, self.directory = kind, directory
        self.record = {'kind': kind, 'pid': pid, 'status': 'preparing', 'control_events': []}
        self.proc = None
        self.control = self.ack = None
        self.log = None
        self.enabled = False
        ctl_read, ctl_write = os.pipe()
        ack_read, ack_write = os.pipe()
        self.control, self.ack = ctl_write, ack_read
        output = directory / ('perf.data' if kind == 'record' else 'hardware.csv')
        if kind == 'record':
            options = ['record', '--no-buildid-cache', '-e', 'cpu-clock:u', '-F', str(FREQUENCY),
                       '--inherit', '-o', str(output)]
        else:
            # Inheritance is the default for stat; no --no-inherit is supplied.
            options = ['stat', '-e', 'cycles:u,instructions:u', '-x', ';', '--no-big-num', '-o', str(output)]
        command = prefix + options + ['-p', str(pid), '-D', '-1',
                                     '--control=fd:' + str(ctl_read) + ',' + str(ack_write)]
        self.record.update(command=command, output=str(output), control='fd with acknowledgement',
                           event='cpu-clock:u' if kind == 'record' else 'cycles:u,instructions:u')
        try:
            self.log = (directory / (kind + '.log')).open('wb')
            self.proc = subprocess.Popen(command, stdout=self.log, stderr=self.log, env=environment,
                                         pass_fds=(ctl_read, ack_write))
        except BaseException:
            self.close_fds()
            if self.log:
                self.log.close()
            raise
        finally:
            os.close(ctl_read)
            os.close(ack_write)

    def control_command(self, word):
        if self.proc.poll() is not None:
            raise ProfileBlocked(self.kind + ' exited before ' + word)
        os.write(self.control, (word + '\n').encode('ascii'))
        pending = b''
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise ProfileBlocked(self.kind + ' exited while waiting for ' + word)
            ready, _, _ = select.select([self.ack], [], [], .2)
            if ready:
                chunk = os.read(self.ack, 128)
                if not chunk:
                    raise ProfileBlocked(self.kind + ' closed acknowledgement pipe')
                pending += chunk
                if b'\n' in pending:
                    require(pending == b'ack\n', 'unexpected perf acknowledgement: ' + repr(pending))
                    self.record['control_events'].append({'command': word, 'ack_monotonic': time.monotonic()})
                    return
        raise ProfileBlocked(self.kind + ' control acknowledgement timeout for ' + word)

    def enable(self):
        self.control_command('enable')
        self.enabled = True
        self.record['status'] = 'enabled'

    def close_fds(self):
        for name in ('control', 'ack'):
            value = getattr(self, name)
            if value is not None:
                os.close(value)
                setattr(self, name, None)

    def finish(self, abort=False):
        try:
            if not abort and self.enabled:
                if not self.proc or self.proc.poll() is not None:
                    raise ProfileBlocked(self.kind + ' exited before the requested phase boundary')
                # An exit code of zero cannot prove that the complete phase was
                # sampled. Require a live collector and its disable ACK first.
                self.control_command('disable')
            if self.proc and self.proc.poll() is None:
                self.proc.send_signal(signal.SIGINT)
                try:
                    self.proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=5)
                    raise ProfileBlocked(self.kind + ' could not finish bounded collection')
            if self.proc:
                self.record['exit_code'] = self.proc.returncode
            if not abort and self.record.get('exit_code') != 0:
                raise ProfileBlocked(self.kind + ' exited nonzero')
            self.record['status'] = 'aborted' if abort else 'collected'
        except BaseException as exc:
            self.record.update(status='blocked', error=type(exc).__name__ + ': ' + str(exc))
            if self.proc and self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait(timeout=5)
            raise
        finally:
            self.close_fds()
            if self.log:
                self.log.close()
            save(self.directory / (self.kind + '-collection.json'), self.record)


class PhaseCollector:
    def __init__(self, prefix, directory, environment, capability):
        self.prefix, self.directory, self.environment = prefix, directory, environment
        self.capability = capability
        self.active = None
        self.phases = []

    def start(self, phase, pid):
        require(self.active is None, 'overlapping profiling phases')
        directory = self.directory / phase
        directory.mkdir()
        item = {'phase': phase, 'pid': pid, 'directory': str(directory), 'status': 'preparing'}
        self.phases.append(item)
        software = hardware = None
        try:
            software = PerfProcess(self.prefix, 'record', pid, directory, self.environment)
            if self.capability.get('hardware') != 'blocked':
                try:
                    hardware = PerfProcess(self.prefix, 'stat', pid, directory, self.environment)
                    hardware.enable()
                except BaseException as exc:
                    item['hardware_error'] = type(exc).__name__ + ': ' + str(exc)
                    self.capability['hardware'] = 'blocked'
                    if hardware:
                        hardware.finish(abort=True)
                    hardware = None
            software.enable()
            item['status'] = 'recording'
            self.active = (item, software, hardware)
        except BaseException as exc:
            item.update(status='blocked', error=type(exc).__name__ + ': ' + str(exc))
            if software:
                software.finish(abort=True)
            if hardware:
                hardware.finish(abort=True)
            raise ProfileBlocked('software profiling unavailable: ' + str(exc)) from exc

    def stop(self, abort=False):
        if self.active is None:
            return
        item, software, hardware = self.active
        self.active = None
        failure = None
        try:
            software.finish(abort=abort)
            item['software'] = software.record
        except BaseException as exc:
            failure = exc
            item.update(status='blocked', error=type(exc).__name__ + ': ' + str(exc))
        finally:
            if hardware:
                try:
                    hardware.finish(abort=abort)
                    csv = (hardware.directory / 'hardware.csv').read_text(encoding='utf-8', errors='replace')
                    values = {}
                    for line in csv.splitlines():
                        fields = line.split(';')
                        if len(fields) >= 3 and fields[2].strip() in ('cycles:u', 'instructions:u'):
                            try:
                                values[fields[2].strip()] = {'value': float(fields[0]),
                                    'unit': fields[1], 'counter_runtime': fields[3] if len(fields) > 3 else None,
                                    'running_percentage': fields[4] if len(fields) > 4 else None}
                            except ValueError:
                                pass
                    if ('<not supported>' in csv or '<not counted>' in csv or
                        set(values) != {'cycles:u', 'instructions:u'} or
                        any(v['value'] <= 0 for v in values.values())):
                        self.capability['hardware'] = 'blocked'
                        hardware.record['status'] = 'blocked'
                        hardware.record['error'] = 'Missing or unsupported counters; raw output preserved.'
                    else:
                        self.capability['hardware'] = 'available'
                        hardware.record['counters'] = values
                    save(hardware.directory / 'stat-collection.json', hardware.record)
                    item['hardware'] = hardware.record
                except BaseException as exc:
                    self.capability['hardware'] = 'blocked'
                    item['hardware_error'] = type(exc).__name__ + ': ' + str(exc)
            else:
                item['hardware_status'] = 'blocked_or_not_attempted_after_block'
            if failure is None:
                item['status'] = 'aborted' if abort else 'collected'
            save(Path(item['directory']) / 'phase.json', item)
        if failure:
            raise failure


@contextmanager
def profile_boundaries(base, collector):
    original = base.Session

    class ProfiledSession(original):
        profile_state = 'before_ingest'

        def request(self, sql=None, *args, **kwargs):
            if self.profile_state == 'before_ingest' and sql == 'BEGIN;':
                collector.start('ingest', self.proc.pid)
                self.profile_state = 'ingest'
            if self.profile_state == 'ingest' and sql == INDEX_SQL:
                collector.stop()
                self.profile_state = 'index'
                result = super().request(sql, *args, **kwargs)
                collector.start('query', self.proc.pid)
                self.profile_state = 'query'
                return result
            return super().request(sql, *args, **kwargs)

        def shutdown(self):
            if self.profile_state == 'query':
                collector.stop()
                self.profile_state = 'done'
            return super().shutdown()

        def close(self):
            try:
                collector.stop(abort=True)
            finally:
                super().close()

    base.Session = ProfiledSession
    try:
        yield
    finally:
        base.Session = original


def decode_phase(prefix, item, environment):
    directory = Path(item['directory'])
    data = directory / 'perf.data'
    require(data.is_file() and data.stat().st_size > 0, 'missing perf.data')
    # Preserve raw samples before any decoder failure; no sample is discarded.
    compressed = directory / 'perf.data.gz'
    with data.open('rb') as src, gzip.open(compressed, 'wb') as dst:
        shutil.copyfileobj(src, dst)
    item.update(data_sha256=sha(data), gzip_sha256=sha(compressed), gzip_bytes=compressed.stat().st_size)
    report_text, report_command = checked(prefix + ['report', '--stdio', '--header', '--no-children',
        '--percent-limit', '0.5', '--sort', 'dso,symbol', '-n', '-i', str(data)], directory, 'perf-report', environment)
    text, script_command = checked(prefix + ['script', '-i', str(data), '-F', 'event,period,ip,sym,dso'],
                                  directory, 'perf-script', environment)
    counts, periods = Counter(), Counter()
    unparsed = []
    # Demangled C++ symbols can themselves contain " ("; the final parens
    # delimit the DSO. Do not split at the first function-type parenthesis.
    # perf script prints fields in its fixed order, not the -F list order:
    # PERF_OUTPUT_PERIOD precedes PERF_OUTPUT_EVNAME in process_event().
    pattern = re.compile(r'^\s*(\d+)\s+(\S+):\s+([0-9a-fA-Fx]+)\s+(.+)\s+\(([^()]*)\)\s*$')
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        match = pattern.match(line)
        if not match:
            unparsed.append(line)
            continue
        period, event, ip, symbol, dso = match.groups()
        require(event == 'cpu-clock:u', 'unexpected sampled event: ' + event)
        symbol = re.sub(r'\+0x[0-9a-fA-F]+$', '', symbol.strip())
        counts[(dso, symbol)] += 1
        periods[(dso, symbol)] += int(period)
    item['unparsed_sample_lines'] = unparsed
    require(not unparsed, 'unexpected perf script format; raw output retained')
    samples = sum(counts.values())
    unknown = sum(count for (dso, symbol), count in counts.items()
                  if '[unknown]' in symbol or '[unknown]' in dso or symbol.startswith('0x') or
                  re.fullmatch(r'[0-9a-fA-F]+', symbol))
    item.update(samples=samples, unknown_samples=unknown, sampled_period_sum=sum(periods.values()),
                symbols=[{'dso': key[0], 'symbol': key[1], 'samples': counts[key], 'period_sum': periods[key]}
                         for key in sorted(counts, key=lambda k: (-periods[k], k))],
                report_command=report_command, script_command=script_command)
    save(directory / 'phase.json', item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--symbols', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--perf', default='perf')
    args = parser.parse_args()
    output = args.output.resolve()
    require(not output.exists(), 'refuse overwrite')
    output.mkdir(parents=True)
    summary = {'status': 'preparing', 'source_commit': EE, 'expected_binary_sha256': EE_SHA,
               'kind': 'external_actual_EE_perf_profile', 'formal_submission': False,
               'performance_comparison': False, 'source_instrumentation': False,
               'planned_repetitions': REPETITIONS, 'completed_repetitions': 0,
               'software_event': 'cpu-clock:u', 'frequency_hz': FREQUENCY,
               'minimum_samples_per_phase': MIN_PHASE_SAMPLES,
               'fixed_limits': {'db_cpu_count': 1, 'cpu_quota_us': 100000, 'cpu_period_us': 100000,
                                'memory_max': '2147483648', 'swap_max': '0'},
               'capabilities': {'software': 'unverified', 'hardware': 'unverified'},
               'runs': [], 'documentation': DOCS,
               'limitations': [
                   'Fixed twelve complete fresh-DB lifecycles, seed7319; no adaptive repeat or sample replacement.',
                   'Only initial BEGIN/INSERT/COMMIT and initial1000 exact-query phases are sampled; index, startup and restart are excluded.',
                   'Original one_run generator, sampled TopK oracle and complete restart validation are reused unchanged.',
                   'Sampling is CPU-clock user-space only, not off-CPU IO wait time or complete wall-time attribution.',
                   'No source instrumentation, call-graph assumption, sysctl change or hardware selection. Flat sampled IPs only.',
                   'Control acknowledgements bracket phases; observer/control overhead can perturb scheduling. This is not a speed benchmark.',
                   'Sample counts and period-weighted shares are approximate profiles, not exact function CPU times.',
                   'Missing PMU counters are explicitly blocked; software sampling may remain usable independently.']}
    save(output / 'summary.json', summary)
    environment = {k: v for k, v in os.environ.items() if not k.startswith('MVDB_')}
    environment.update(PYTHONDONTWRITEBYTECODE='1', LC_ALL='C')
    parent = None
    failure = None
    try:
        require(sys.platform.startswith('linux'), 'Linux is required')
        summary['tool_provenance'] = verify_vendor()
        binary, symbols, launcher = args.binary.resolve(), args.symbols.resolve(), args.launcher.resolve()
        require(sha(binary) == EE_SHA, 'actual binary is not the verified original EE ELF')
        require(all(os.access(p, os.X_OK) for p in (binary, launcher)), 'binary/launcher not executable')
        summary.update(binary=str(binary), binary_sha256=sha(binary), symbols=str(symbols),
                       symbols_sha256=sha(symbols), launcher_sha256=sha(launcher),
                       wrapper_sha256=sha(__file__), removed_MVDB_names=sorted(k for k in os.environ if k.startswith('MVDB_')))
        stripped = output / 'symbol-strip-verification'
        checked(['strip', '-o', str(stripped), str(symbols)], output, 'strip-symbol-copy', environment)
        require(sha(stripped) == EE_SHA, 'symbol ELF does not strip byte-identically to actual EE')
        summary['symbols_strip_verified'] = True
        perf = str(Path(shutil.which(args.perf) or args.perf).resolve())
        prefix = [perf, '--buildid-dir', str(output / 'buildid-cache')]
        version, _ = checked([perf, '--version'], output, 'perf-version', environment)
        summary['perf_version'] = version.strip()
        checked(prefix + ['buildid-cache', '--add', str(symbols)], output, 'buildid-cache-add', environment)
        summary['host'] = {'platform': platform.platform(), 'affinity': sorted(os.sched_getaffinity(0))}
        for name, path in (('cpuinfo', '/proc/cpuinfo'), ('self-cgroup', '/proc/self/cgroup'),
                           ('perf-event-paranoid', '/proc/sys/kernel/perf_event_paranoid'),
                           ('kptr-restrict', '/proc/sys/kernel/kptr_restrict')):
            try:
                (output / (name + '.txt')).write_bytes(Path(path).read_bytes())
            except OSError as exc:
                summary.setdefault('unavailable_host_files', {})[name] = str(exc)
        sys.path.insert(0, str(TOOLS))
        base, delta, writes, projection, small = [importlib.import_module(name) for name in
            ('benchmark_iteration', 'benchmark_index_delta', 'benchmark_write_query', 'benchmark_projection', 'small_compute')]
        workload = base.make_workload(base.FULL_CASES['standard10k'], 7319, 1000, 100, 10, query_mode='exact')
        summary['workload_identity'] = small.workload_identity('standard10k', workload, base)
        settings = SimpleNamespace(cpu_quota_us=100000, cpu_period_us=100000, memory_max='2147483648',
                                   swap_max='0', case_timeout=600, timeout=120, query_mode='exact', ingest_telemetry=True)
        cpus = small.select_cpus(os.sched_getaffinity(0), 1)
        summary['db_cpus'] = cpus
        taskset = str(Path(shutil.which('taskset') or '/missing-taskset').resolve())
        parent = small.CGROOT / ('ee-profile-' + uuid.uuid4().hex)
        parent.mkdir()
        (parent / 'cgroup.subtree_control').write_text('+cpu +memory +io')
        response_hash = None
        for repetition in range(REPETITIONS):
            directory = output / ('run-' + format(repetition, '02d'))
            directory.mkdir()
            run = {'repetition': repetition, 'cgroup': {}, 'adapter': {}, 'status': 'running'}
            summary['runs'].append(run)
            collector = PhaseCollector(prefix, directory, environment, summary['capabilities'])
            try:
                with no_mvdb_environment(), small.Group(parent, settings, run['cgroup']) as group:
                    settings.command_prefix = [str(launcher), str(group.path), taskset, '--cpu-list', str(cpus[0])]
                    with small.command_adapter(base, projection, settings.command_prefix, binary, group.path,
                                               run['adapter'], cpus, expected_environment={}):
                        with profile_boundaries(base, collector):
                            run.update(base.one_run(binary, workload, settings))
                require([p['phase'] for p in collector.phases] == ['ingest', 'query'], 'phase inventory differs')
                require(not run['cgroup']['timeout'] and not run['cgroup'].get('cleanup_kill_required') and
                        not any(run['cgroup']['oom_events_delta'].values()), 'timeout, leftover process or OOM')
                require(response_hash is None or response_hash == run['response_sha256'], 'responses changed across repetitions')
                response_hash = run['response_sha256']
                require(sha(binary) == EE_SHA, 'actual EE changed during profiling')
                for phase in collector.phases:
                    decode_phase(prefix, phase, environment)
                summary['completed_repetitions'] += 1
                summary['capabilities']['software'] = 'available'
                run['status'] = 'passed'
            finally:
                collector.stop(abort=True)
                run['profiles'] = collector.phases
                save(directory / 'run.json', run)
                save(output / 'summary.json', summary)
        totals = {}
        for phase_name in ('ingest', 'query'):
            samples = unknown = 0
            periods, counts = Counter(), Counter()
            for run in summary['runs']:
                phase = next(p for p in run['profiles'] if p['phase'] == phase_name)
                samples += phase['samples']
                unknown += phase['unknown_samples']
                for item in phase['symbols']:
                    key = (item['dso'], item['symbol'])
                    periods[key] += item['period_sum']
                    counts[key] += item['samples']
            total_period = sum(periods.values())
            usable = samples >= MIN_PHASE_SAMPLES and unknown <= samples * .20
            totals[phase_name] = {'samples': samples, 'unknown_samples': unknown, 'sufficient_samples_and_symbols': usable,
                'status': 'usable_flat_profile' if usable else 'inconclusive_samples_or_symbols',
                'top_symbols': [{'dso': k[0], 'symbol': k[1], 'samples': counts[k], 'period_sum': periods[k],
                                 'sample_fraction': counts[k] / samples if samples else None,
                                 'period_fraction': periods[k] / total_period if total_period else None}
                                for k in sorted(periods, key=lambda k: (-periods[k], k))[:30]]}
        summary['phase_summary'] = totals
        summary['status'] = 'completed' if all(v['sufficient_samples_and_symbols'] for v in totals.values()) else 'inconclusive'
    except BaseException as exc:
        failure = exc
        summary.update(status='blocked' if isinstance(exc, (ProfileBlocked, FileNotFoundError, PermissionError)) else 'failed',
                       error=type(exc).__name__ + ': ' + str(exc), traceback=traceback.format_exc())
        if isinstance(exc, ProfileBlocked):
            summary['capabilities']['software'] = 'blocked'
    finally:
        if parent and parent.exists():
            try:
                parent.rmdir()
            except OSError as exc:
                summary.update(status='failed', cgroup_cleanup_error=str(exc))
        save(output / 'summary.json', summary)
        try:
            archive_raw(output, summary)
        except BaseException as exc:
            summary['raw_archive'] = {'status': 'failed_raw_not_archived_to_logs',
                                      'error': type(exc).__name__ + ': ' + str(exc),
                                      'raw_archive_transport_prepared': False}
        save(output / 'summary.json', summary)
        compact = {'status': summary['status'], 'completed_repetitions': summary['completed_repetitions'],
                   'binary_sha256': summary.get('binary_sha256'), 'capabilities': summary['capabilities'],
                   'phase_summary': summary.get('phase_summary'), 'raw_archive': summary.get('raw_archive'),
                   'error': summary.get('error')}
        (output / 'profile-compact.txt').write_text(json.dumps(compact, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': summary['status'], 'completed_repetitions': summary['completed_repetitions'],
                      'capabilities': summary['capabilities'], 'summary': str(output / 'summary.json')}), flush=True)
    if summary['status'] not in ('completed', 'inconclusive'):
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
