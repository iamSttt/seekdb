#!/usr/bin/env python3
"""Verify frozen source identity and read an unchanged 12-pair source ablation."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BASE = '9ed5baa1b1363ebd388850e74271b353954902da'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n', encoding='utf8')


def git(*args):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + str(REPO), '-C', str(REPO), *args])


def plan():
    data = json.loads((HERE / 'plan.json').read_text(encoding='utf8'))
    require(data['baseline_source'] == BASE and data['expected_pairs'] == 12, 'wrong frozen source/plan')
    return data


def verify(output):
    data = plan()
    for path, expected in data['candidate_inputs'].items():
        require(sha(REPO / path) == expected, 'candidate input changed: ' + path)
    for path, expected in data['baseline_build_inputs'].items():
        require(hashlib.sha256(git('show', BASE + ':' + path)).hexdigest() == expected,
                'baseline input changed: ' + path)
    actual = set(git('diff', '--name-only', BASE, '--', 'minivec/src').decode().splitlines())
    require(actual == {'minivec/src/quantized_bound.h'}, 'runtime scope differs')
    require(not git('diff', BASE, '--', 'Dockerfile', '.dockerignore', 'minivec/third_party',
                    '.github/workflows'), 'build/workflow inputs changed')
    save(output, dict(status='verified_frozen_source_inputs', source_commit=git('rev-parse', 'HEAD').decode().strip(),
         plan_sha256=sha(HERE/'plan.json'), candidate_inputs=data['candidate_inputs'],
         baseline_build_inputs=data['baseline_build_inputs'], runtime_change_paths=sorted(actual)))


def proof(binary, role, image_inspect, output):
    data = plan()
    binary = binary.resolve()
    digest = sha(binary)
    source = BASE if role == 'baseline' else git('rev-parse', 'HEAD').decode().strip()
    inputs = data['baseline_build_inputs'] if role == 'baseline' else data['candidate_build_inputs']
    for path, expected in inputs.items():
        require(hashlib.sha256(git('show', source + ':' + path)).hexdigest() == expected,
                role + ' source/blob mismatch: ' + path)
    if role == 'baseline':
        require(digest == data['baseline_expected_elf_sha256'], 'parent root ELF changed')
    save(output, dict(status='verified_original_root_build_inputs', source_commit=source,
         binary_sha256=digest, binary=str(binary), build_inputs=inputs,
         image_inspect_sha256=sha(image_inspect), frozen_plan_sha256=sha(HERE/'plan.json'),
         scope='Actual ELF extracted from the root image built by this driver; not an official platform ELF claim.'))


def evaluate(screen_path, output):
    data = plan()
    screen = json.loads(screen_path.read_text(encoding='utf8'))
    require(screen['status'] == 'screen_completed' and screen['completed_pairs'] == 12, 'incomplete screen')
    require(screen['comparison_baseline']['declared_source_commit'] == BASE and
            screen['comparison_candidate']['declared_source_commit'] == git('rev-parse','HEAD').decode().strip(),
            'source declaration changed')
    require(screen['plan'] == data['schedule'], 'executed schedule changed')
    checks = {}
    guards = []
    groups = {}
    for cpu in ('1', '2'):
        raw = screen['arms'][cpu]['data']
        require(raw['status'] == 'passed' and len(raw['pairs']) == 6 and
                all(x['all_responses_identical'] is True for x in raw['pairs']), 'oracle/response gate absent')
        require(all(x[role]['declared_environment'] == {} for x in raw['pairs']
                    for role in ('baseline','candidate')), 'declared environment changed')
        metrics = screen['comparisons'][cpu]['metrics']
        groups[cpu] = metrics
        for name in data['query_metrics']:
            metric = metrics[name]
            require(len(metric['raw_pairs']) == 6 and all(
                math.isfinite(r['baseline']) and r['baseline'] > 0 and
                math.isfinite(r['candidate']) and r['candidate'] >= 0 for r in metric['raw_pairs']),
                'invalid query ratio')
            checks[cpu + ':' + name] = (
                metric['median_candidate_over_baseline'] <= data['query_max_ratio'] and
                metric['candidate_strictly_faster_pairs'] >= data['query_min_faster_pairs'] and
                all(v < 1 for v in metric['order_median_candidate_over_baseline'].values()))
        for name, limit in data['stable_regression_metrics'].items():
            metric = metrics[name]
            require(len(metric['raw_pairs']) == 6 and all(
                math.isfinite(r['baseline']) and r['baseline'] > 0 and
                math.isfinite(r['candidate']) and r['candidate'] >= 0 for r in metric['raw_pairs']),
                'invalid protection ratio')
            if (metric['median_candidate_over_baseline'] > limit and
                    all(v > limit for v in metric['order_median_candidate_over_baseline'].values())):
                guards.append(dict(cpu=int(cpu), metric=name, threshold=limit))
    eligible = all(checks.values()) and not guards
    save(output, dict(status='single_factor_screen_eligible' if eligible else 'single_factor_screen_not_eligible',
         fixed_parent_ablation_thresholds_passed=eligible, query_checks=checks,
         stable_regression_triggers=guards, all_metrics_with_raw_pairs=groups,
         completed_pairs=12, frozen_plan_sha256=sha(HERE/'plan.json'), screen_sha256=sha(screen_path),
         baseline_source=BASE, candidate_source=screen['comparison_candidate']['declared_source_commit'],
         release_gate_passed=False, automatic_adoption=False, formal_submission=False, official_score=None,
         scope='Parent9ed single-factor screen only. Does not replace EE .90/.95, retained protection or independent confirmation.'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('verify');p.add_argument('--output',type=Path,required=True)
    p = sub.add_parser('proof')
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--role',choices=('baseline','candidate'),required=True)
    p.add_argument('--image-inspect',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p = sub.add_parser('evaluate');p.add_argument('--screen',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.action=='verify':verify(args.output)
    elif args.action=='proof':proof(args.binary,args.role,args.image_inspect,args.output)
    else:evaluate(args.screen,args.output)


if __name__ == '__main__':
    main()
