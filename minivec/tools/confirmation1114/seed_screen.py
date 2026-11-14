#!/usr/bin/env python3
"""12-pair source comparison; EE screening by default, explicit ablation otherwise.

Run with cgroup-v2 write permission (normally sudo on the Linux CI host).
The imported measurement/oracle tools retain original bytes except the explicit seed19087 plumbing.
Binary/source declarations are recorded, not asserted to recover official ELFs.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
from statistics import median
import subprocess
import sys
import traceback

ROOT = Path(__file__).resolve().parent / 'vendor'
TOOLS = ROOT / 'minivec' / 'tools'
EE = 'ee299adb296a2922d01982aa6c5707d37e767b41'
FAMILY = 'ee_confirmation19087_12pair_20261114'
ABLATION_FAMILY = 'unused_confirmation19087_ablation'


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                                   allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def plan():
    return [{'cpu_count': cpu, 'scenario': 'standard10k', 'seed': 19087,
             'pair_index': index,
             'order': ['baseline', 'candidate'] if index % 2 == 0
                      else ['candidate', 'baseline']}
            for cpu in (1, 2) for index in range(6)]


def verify_vendor():
    provenance = json.loads((ROOT / 'source-manifest.json').read_text(encoding='utf-8'))
    for item in provenance['files']:
        require(sha(ROOT / item['path']) == item['sha256'],
                'vendored tool changed: ' + item['path'])
    return provenance


def identity(role, binary, commit, directory, evidence=None):
    require(re.fullmatch('[0-9a-f]{40}', commit) is not None,
            role + ' needs a full declared source commit')
    binary = binary.resolve()
    require(binary.is_file() and os.access(binary, os.X_OK),
            role + ' is not an executable file')
    digest = sha(binary)
    proof = {'status': 'verified', 'source_commit': commit, 'binary_sha256': digest,
             'verification_scope': 'Binary file SHA256 measured now; source_commit is caller-declared, not independently source-to-ELF verified.',
             'official_binary_identity_known': False}
    if evidence:
        evidence = evidence.resolve()
        proof['external_build_evidence'] = {'path': str(evidence), 'sha256': sha(evidence)}
    proof_path = directory / (role + '-file-identity.json')
    save(proof_path, proof)
    return {'source_commit': commit, 'binary': str(binary), 'binary_sha256': digest,
            'evidence_file': str(proof_path), 'evidence_sha256': sha(proof_path),
            'evidence_binary_pointer': ['binary_sha256'],
            'evidence_commit_pointer': ['source_commit'], 'environment': {}}


def summarize(data, baseline_source=EE):
    # This function uses the original standard_metrics extraction, with its
    # original CPU phase boundaries. No new oracle or timing instrumentation.
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(TOOLS))
    from metrics import standard_metrics
    raw = {}
    for pair in data['pairs']:
        values = {role: standard_metrics(pair[role]) for role in ('baseline', 'candidate')}
        for name, baseline in values['baseline'].items():
            candidate = values['candidate'][name]
            require(all(math.isfinite(x) and x >= 0 for x in (baseline, candidate)),
                    'invalid metric: ' + name)
            raw.setdefault(name, []).append({
                'pair_index': pair['pair_index'], 'order': pair['order'],
                'baseline': baseline, 'candidate': candidate,
                'candidate_over_baseline': candidate / baseline if baseline else None,
                'baseline_over_candidate': baseline / candidate if candidate else None})
    summary = {}
    for name, rows in raw.items():
        ratios = [r['candidate_over_baseline'] for r in rows]
        valid = [r for r in ratios if r is not None]
        by_order = {}
        for first in ('baseline', 'candidate'):
            items = [r['candidate_over_baseline'] for r in rows
                     if r['order'][0] == first and r['candidate_over_baseline'] is not None]
            by_order['AB' if first == 'baseline' else 'BA'] = median(items) if items else None
        summary[name] = {'raw_pairs': rows, 'median_candidate_over_baseline': median(valid) if valid else None,
                         'candidate_strictly_faster_pairs': sum(r['candidate'] < r['baseline'] for r in rows),
                         'candidate_slower_pairs': sum(r['candidate'] > r['baseline'] for r in rows),
                         'order_median_candidate_over_baseline': by_order}
    result = {'metrics': summary, 'baseline_source_commit': baseline_source,
              'release_gate_passed': False, 'automatic_adoption': False}
    if baseline_source != EE:
        result['decision_scope'] = 'Paired source ablation metrics only; EE improvement thresholds do not apply to this baseline.'
        return result
    thresholds = {'first_lifecycle_wall_seconds': .95,
                  'first_lifecycle_cpu_to_before_shutdown_usec': .95,
                  'first_1000_queries_wall_seconds': .90,
                  'first_1000_queries_cpu_usec': .90}
    checks = {}
    for name, threshold in thresholds.items():
        metric = summary[name]
        checks[name] = (len(metric['raw_pairs']) == 6 and
                        metric['median_candidate_over_baseline'] is not None and
                        metric['median_candidate_over_baseline'] <= threshold and
                        metric['candidate_strictly_faster_pairs'] >= 5 and
                        all(r is not None and r < 1 for r in
                            metric['order_median_candidate_over_baseline'].values()))
    result.update(historical_EE_primary_thresholds_only=checks,
                  all_EE_primary_thresholds_passed=all(checks.values()))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--baseline-source', default=EE,
                        help='full source SHA; EE by default, another explicit source selects metric-only ablation')
    parser.add_argument('--candidate-source', required=True)
    parser.add_argument('--baseline-evidence', type=Path)
    parser.add_argument('--candidate-evidence', type=Path)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plan-only', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    directory = args.output.with_suffix(args.output.suffix + '.d')
    require(not args.output.exists() and not directory.exists(), 'refuse to overwrite prior measurement')
    require(re.fullmatch('[0-9a-f]{40}', args.baseline_source) is not None, 'full baseline source SHA required')
    require(re.fullmatch('[0-9a-f]{40}', args.candidate_source) is not None, 'full candidate source SHA required')
    ee_comparison = args.baseline_source == EE
    manifest = verify_vendor()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'status': 'plan_only_not_executed' if args.plan_only else 'running',
              'measurement_family': FAMILY if ee_comparison else ABLATION_FAMILY,
              'comparison_baseline': {'declared_source_commit': args.baseline_source,
                                      'is_EE_source': ee_comparison,
                                      'identity_scope': 'Caller-declared source; actual binary SHA256 is recorded separately.'},
              'comparison_candidate': {'declared_source_commit': args.candidate_source},
              'underlying_plan_selector': {'highest_primary_reference': 'ee',
                  'scope': 'Selects the existing six-default-pairs-per-CPU schedule only. It does not establish EE binary/source identity for a non-EE comparison.'},
              'plan': plan(), 'expected_pairs': 12,
              'completed_pairs': 0, 'official_score': None, 'formal_submission': False,
              'release_gate_passed': False, 'automatic_adoption': False,
              'tool_provenance': manifest, 'wrapper_sha256': sha(__file__),
              'limits': {'cpu_counts': [1, 2], 'period_usec': 100000,
                         'memory_max_bytes': 2147483648, 'memory_swap_max_bytes': 0},
              'workload': {'scenario': 'standard10k', 'rows': 10000, 'dimension': 128,
                           'seed': 19087, 'query_mode': 'exact', 'queries_before_shutdown': 1000,
                           'queries_after_restart': 1000, 'batch_size': 100, 'topk': 10},
              'notes': [
                  ('EE-only 12-pair seed19087 confirmation; never merge with seed7319.' if ee_comparison else
                   'Explicit source ablation, 12 paired comparisons; no EE thresholds or official-baseline inference.') +
                  ' Omits retained-R default, spill and wide protection; never the historical 24-pair release gate.',
                  'Original deterministic generator, sampled independent TopK oracle, all restarted IDs and sampled vectors are unchanged.',
                  'Full responses compared by SHA256 across each pair; per-request timings, phase counters, memory and IO remain in raw reports.',
                  'First-lifecycle wall is the sum of original measured request/startup/shutdown intervals. CPU ends before shutdown, as in the historical metric.',
                  'CPU counters include engine activity during client checks. Driver/oracle remain outside DB cgroup, sharing the same host.',
                  'Warm OS cache; no cache dropping or discarded warmup samples. Failure stops this screen and preserves completed/partial data.',
                  'No numeric official-score forecast. Source-to-ELF build provenance must be established separately.'],
              'arms': {}, 'comparisons': {}}
    save(args.output, report)
    if args.plan_only:
        return
    failure = None
    try:
        require(sys.platform.startswith('linux'), 'Linux cgroup-v2 runner required')
        directory.mkdir()
        identities = {role: identity(role, getattr(args, role), getattr(args, role + '_source'), directory,
                                     getattr(args, role + '_evidence')) for role in ('baseline', 'candidate')}
        identities_path = directory / 'identities.json'
        save(identities_path, identities)
        report['identities'] = identities
        launcher = args.launcher.resolve()
        require(launcher.is_file() and os.access(launcher, os.X_OK), 'executable cgroup launcher required')
        report['launcher_sha256'] = sha(launcher)
        # Remove every inherited MVDB variable, including future/unlisted knobs.
        # Names only are recorded because unrelated environment values may be secrets.
        child_env = {k: v for k, v in os.environ.items() if not k.startswith('MVDB_')}
        child_env['PYTHONDONTWRITEBYTECODE'] = '1'
        report['removed_inherited_MVDB_variable_names'] = sorted(k for k in os.environ if k.startswith('MVDB_'))
        for count in (1, 2):
            path = directory / (('ee-' if ee_comparison else 'comparison-') + str(count) + 'cpu.json')
            command = [sys.executable, '-B', str(TOOLS / 'small_compute.py'), '--tools', str(TOOLS),
                       '--identities', str(identities_path), '--launcher', str(launcher), '--output', str(path),
                       '--allow-version-contrast', '--db-cpu-count', str(count), '--highest-primary-reference', 'ee',
                       '--highest-primary-confirmation',
                       '--query-mode', 'exact', '--memory-max', '2147483648', '--swap-max', '0',
                       '--cpu-quota-us', str(count * 100000), '--cpu-period-us', '100000', '--case-timeout', '600']
            arm = {'command': command, 'raw_file': str(path), 'status': 'running',
                   'baseline_source_commit': args.baseline_source,
                   'raw_plan_scope': 'Historical helper with only seed19087 plumbing uses an ee selector/family label for this six-pair schedule; source and binary identity are the explicit identities records, not that plan label.'}
            report['arms'][str(count)] = arm
            save(args.output, report)
            code = subprocess.run(command, env=child_env, check=False).returncode
            arm['exit_code'] = code
            if path.exists():
                raw = json.loads(path.read_text(encoding='utf-8'))
                arm.update(status=raw.get('status'), raw_sha256=sha(path), data=raw)
                report['completed_pairs'] += len(raw.get('pairs', []))
                if raw.get('pairs'):
                    report['comparisons'][str(count)] = summarize(raw, args.baseline_source)
            save(args.output, report)
            require(code == 0 and arm.get('status') == 'passed', 'CPU arm failed: ' + str(count))
            require(len(arm['data']['pairs']) == 6, 'missing pairs')
        first, second = (report['arms'][str(count)]['data'] for count in (1, 2))
        require(first['workloads'] == second['workloads'], 'workload differs between CPU arms')
        require(first['host'] == second['host'] and first['taskset'] == second['taskset'], 'host identity changed')
        require(first['db_cpus'] == second['db_cpus'][:1], 'CPU assignments differ')
        actual_plan = [{'cpu_count': count, **{k: pair[k] for k in ('scenario', 'seed', 'pair_index', 'order')}}
                       for count in (1, 2) for pair in report['arms'][str(count)]['data']['pairs']]
        require(actual_plan == plan() and report['completed_pairs'] == 12, 'measured plan differs')
        require(verify_vendor() == manifest, 'vendor provenance changed')
        report['status'] = 'screen_completed'
        if ee_comparison:
            report['all_EE_primary_thresholds_passed'] = all(
                item['all_EE_primary_thresholds_passed'] for item in report['comparisons'].values())
        else:
            report['decision_scope'] = 'Source ablation completed; no EE threshold pass/fail or automatic adoption decision.'
    except BaseException as exc:
        failure = exc
        report.update(status='failed_or_incomplete', error=type(exc).__name__ + ': ' + str(exc),
                      traceback=traceback.format_exc())
    finally:
        save(args.output, report)
    if failure is not None:
        raise failure


if __name__ == '__main__':
    main()
