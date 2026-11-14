#!/usr/bin/env python3
"""Audit extracted build products; never infer performance from source identity."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

BASE = '760f66220f2c7186b4ba295f9e3065650e549f9e'
BASE_SHA = 'fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf8')


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', choices=['record', 'compare', 'baseline', 'finish'])
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--label', choices=['A', 'B'])
parser.add_argument('--source')
parser.add_argument('--binary', type=Path)
args = parser.parse_args()
root = args.root
if args.mode == 'record':
    d = root / args.label
    records = [json.loads(line) for line in (d / 'training-verification.txt').read_text().splitlines()]
    require(len(records) == 6, 'six training verifications required')
    require([r['responses'] for r in records] == [951, 41, 14] * 2, 'training corpus/count changed')
    require(all(r['status'] == 'passed' for r in records), 'training oracle failed')
    profiles = (d / 'profile-sha256.txt').read_text().splitlines()
    require(len(profiles) == 6, 'six translation-unit profiles required')
    builder = (d / 'builder-ELF-sha256.txt').read_text().strip()
    require(sha(args.binary) == builder, 'release binary differs from the trained builder product')
    proof = {'status': 'verified_build_and_training', 'label': args.label,
             'source_commit': args.source, 'binary_sha256': sha(args.binary),
             'image_id': json.loads((d / 'image.json').read_bytes())[0]['Id'],
             'builder_image_id': json.loads((d / 'builder-image.json').read_bytes())[0]['Id'],
             'training': records, 'profiles': profiles,
             'corpus_hashes': (d / 'corpus-sha256.txt').read_text().splitlines(),
             'source_manifest_sha256': sha(root / 'source-manifest.json'),
             'scope': 'Actual extracted ELF, original six training oracles and context identity; not a full runtime correctness pass.'}
    save(d / 'build-proof.json', proof)
elif args.mode == 'baseline':
    require(sha(args.binary) == BASE_SHA, 'fresh non-PGO control differs from760f')
    save(root / 'baseline-proof.json', {'status': 'verified_760f_non_PGO_build',
         'source_commit': BASE, 'binary_sha256': BASE_SHA,
         'image_id': json.loads((root / 'baseline-image.json').read_bytes())[0]['Id']})
elif args.mode == 'compare':
    proofs = {label: json.loads((root / label / 'build-proof.json').read_bytes()) for label in ('A', 'B')}
    equal = proofs['A']['binary_sha256'] == proofs['B']['binary_sha256']
    report = {'status': 'two_ELFs_equal' if equal else 'blocked_nonreproducible_ELF',
              'two_fresh_builds': 2, 'ELF_equal': equal, 'builds': proofs,
              'profiles_equal': proofs['A']['profiles'] == proofs['B']['profiles'],
              'corpus_hashes_equal': proofs['A']['corpus_hashes'] == proofs['B']['corpus_hashes'],
              'same_ELF_confirmation_rule_unchanged': True,
              'performance_allowed': equal, 'faster_build_selection': False,
              'release_gate_passed': False, 'automatic_adoption': False}
    require(report['corpus_hashes_equal'], 'training corpus changed between builds')
    save(root / 'build-comparison.json', report)
    print(json.dumps(report))
    if not equal:
        sys.exit(42)
else:
    results = {label: json.loads((root / label / 'source-factor.json').read_bytes()) for label in ('A', 'B')}
    complete = all(r['completed_pairs'] == 12 for r in results.values())
    passed = complete and all(r.get('same_source_guard_passed') for r in results.values())
    report = {'status': 'source_factor_guard_passed' if passed else 'source_factor_blocked',
              'complete': complete, 'pairs': sum(r['completed_pairs'] for r in results.values()),
              'source_factor_guard_passed': passed,
              'per_build': {k: {x: v.get(x) for x in ('status', 'completed_pairs', 'guard_triggers', 'guard_metrics')}
                            for k, v in results.items()},
              'EE_R_whole_program_gates': 'deferred, unchanged; final combination only',
              'independent_confirmation': 'not executed', 'release_gate_passed': False,
              'official_score': None, 'automatic_adoption': False}
    save(root / 'summary.json', report)
    print(json.dumps({k: v for k, v in report.items() if k != 'per_build'}))
    if not passed:
        sys.exit(20)
