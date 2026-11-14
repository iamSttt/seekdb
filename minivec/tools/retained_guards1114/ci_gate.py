#!/usr/bin/env python3
"""Convert complete retained-guard evidence to a CI result, never a release pass."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import retained_guards as original_guards

R = '33a52a7dd4426af059ec2334b7c93a07c38c93c9'
R_SHA = 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'


def preflight(candidate, commit, proof):
    if re.fullmatch('[0-9a-f]{40}', commit) is None:
        raise ValueError('full candidate source commit required')
    binary = candidate.read_bytes()
    if binary[:6] != b'\x7fELF\x02\x01' or int.from_bytes(binary[18:20], 'little') != 62:
        raise ValueError('candidate is not an extracted x86-64 ELF')
    digest = hashlib.sha256(binary).hexdigest()
    declared = proof.get('source_commit', proof.get('runtime_commit'))
    recorded = proof.get('binary_sha256', proof.get('image_binary_sha256'))
    if declared != commit or recorded != digest:
        raise ValueError('candidate proof does not bind this source and actual ELF')
    return {'source_commit': commit, 'binary_sha256': digest,
            'scope': 'Identity only; caller retains the ordinary root correctness gate evidence.'}


def evaluate(execution, guards, reference):
    try:
        recomputed = original_guards.numerical_guards(guards.get('arms', {}))
        complete_metrics = (
            recomputed['numerical_retained_guards_passed'] is True
            and guards.get('guard_metrics') == recomputed['guard_metrics']
            and guards.get('guard_triggers') == recomputed['guard_triggers']
        )
    except (ValueError, TypeError, KeyError, IndexError):
        complete_metrics = False
    checks = {
        'execution_completed': execution.get('status') == 'completed' and execution.get('exit_code') == 0,
        'execution_numerical_pass': execution.get('numerical_retained_guards_passed') is True,
        'guard_completed': guards.get('status') == 'retained_guards_complete_no_trigger',
        'guard_numerical_pass': guards.get('numerical_retained_guards_passed') is True,
        'all_twelve_pairs': guards.get('expected_pairs') == guards.get('completed_pairs') == 12,
        'both_cpu_arms': set(guards.get('arms', {})) == {'1', '2'},
        'no_guard_trigger': guards.get('guard_triggers') == [],
        'all_original_metrics_recomputed': complete_metrics,
        'fresh_R_build': reference.get('status') == 'verified_current_root_build'
                         and reference.get('new_reference_root_pgo_builds') == 1,
        'R_source_and_ELF': reference.get('source_commit') == R and reference.get('binary_sha256') == R_SHA,
    }
    passed = all(checks.values())
    return {'status': 'retained_guard_stage_passed' if passed else 'retained_guard_stage_blocked',
            'retained_guard_stage_passed': passed, 'checks': checks,
            'release_gate_passed': False, 'full_24_pair_gate_passed': False,
            'automatic_adoption': False, 'official_score': None,
            'scope': 'Original R protection portion only; no EE or final release claim.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    first = sub.add_parser('preflight')
    first.add_argument('--candidate', type=Path, required=True)
    first.add_argument('--source', required=True)
    first.add_argument('--proof', type=Path, required=True)
    last = sub.add_parser('finish')
    last.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'preflight':
        print(json.dumps(preflight(args.candidate, args.source, json.loads(args.proof.read_bytes()))))
        return
    report_path = args.output / 'ci-gate.json'
    if report_path.exists():
        raise ValueError('refuse to overwrite CI result')
    try:
        values = [json.loads((args.output / name).read_bytes()) for name in
                  ('execution.json', 'guards.json', 'reference-build-proof.json')]
        report = evaluate(*values)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        report = {'status': 'retained_guard_stage_blocked', 'retained_guard_stage_passed': False,
                  'error': type(exc).__name__ + ': ' + str(exc), 'release_gate_passed': False,
                  'full_24_pair_gate_passed': False, 'automatic_adoption': False}
    report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf8')
    print(json.dumps(report, sort_keys=True))
    if not report['retained_guard_stage_passed']:
        sys.exit(20)


if __name__ == '__main__':
    main()
