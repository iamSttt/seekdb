#!/usr/bin/env python3
"""Bind the frozen retained workload/rules to a disk scan no-initialization source factor."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
path = ROOT / 'minivec/tools/retained_guards1114/retained_guards.py'
spec = importlib.util.spec_from_file_location('unchanged_retained_rules', path)
original = importlib.util.module_from_spec(spec)
spec.loader.exec_module(original)
BASE = '760f66220f2c7186b4ba295f9e3065650e549f9e'
BASE_SHA = 'fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456'


def identity(role, binary, commit, evidence, directory):
    binary = binary.resolve()
    evidence = evidence.resolve()
    original.require(binary.is_file() and os.access(binary, os.X_OK), 'missing executable')
    proof = original.read(evidence)
    digest = original.sha(binary)
    original.require(proof['source_commit'] == commit and proof['binary_sha256'] == digest,
                     'actual build/source/ELF proof mismatch')
    original.require(proof['status'] == 'verified_current_root_build', 'invalid build proof')
    if role == 'baseline':
        original.require(commit == BASE and digest == BASE_SHA, 'not the frozen control')
    item = {'status': 'verified', 'source_commit': commit, 'binary_sha256': digest,
            'external_evidence': str(evidence), 'external_evidence_sha256': original.sha(evidence),
            'scope': 'Actual build identity for disk scan no-initialization source factor; not a release assertion.'}
    target = directory / (role + '-identity.json')
    original.save(target, item)
    return {'source_commit': commit, 'binary': str(binary), 'binary_sha256': digest,
            'evidence_file': str(target), 'evidence_sha256': original.sha(target),
            'evidence_binary_pointer': ['binary_sha256'],
            'evidence_commit_pointer': ['source_commit'], 'environment': {}}


# Identity hooks only. Original workload generation, scheduler, counters,
# response oracles, metric extraction, and stable-regression rules stay intact.
original.R = BASE
original.R_SHA = BASE_SHA
original.identity = identity
output = Path(sys.argv[sys.argv.index('--output') + 1])
try:
    original.main()
finally:
    if output.exists():
        data = original.read(output)
        data['measurement_scope'] = 'disk scan no-initialization source factor only; original retained workload/rules'
        data['source_factor_guard_passed'] = data.get('numerical_retained_guards_passed') is True
        data['scope'] = 'No R whole-program, EE primary, release or official-score claim.'
        data['wrapper_source_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        data['release_gate_passed'] = False
        data['automatic_adoption'] = False
        original.save(output, data)
# Preserve every original protection sample before applying the extra benefit gate.
if '--plan-only' not in sys.argv:
    data = original.read(output)
    guard = data.get('source_factor_guard_passed') is True
    group = data.get('guard_metrics', {}).get('retained:1:spill_high_q16', {})
    thresholds = {'query_before_update_wall_seconds': .97,
                  'query_before_update_cpu_usec': .97,
                  'first_query_wall_seconds': .95}
    benefit = {}
    for name, threshold in thresholds.items():
        record = group.get(name, {})
        pairs = record.get('pairs', [])
        benefit[name] = {'threshold_candidate_over_baseline': threshold,
                         'pairs': pairs,
                         'passed': len(pairs) == 2 and
                                   all(row['candidate_over_baseline'] <= threshold for row in pairs)}
    passed = guard and data.get('completed_pairs') == 12 and all(x['passed'] for x in benefit.values())
    data.update(original_31_protection_passed=guard, spill_benefit_conditions=benefit,
                source_factor_passed=passed, source_factor_guard_passed=guard,
                status='source_factor_passed' if passed else 'source_factor_blocked',
                limitation='Source factor against unchanged 760f only; twelve pairs and original 31 guards. No EE/R whole-program, independent confirmation, official score or release claim.')
    original.save(output, data)
    print(json.dumps({'source_factor_passed': passed, 'protection_passed': guard,
                      'benefit_checks': {k: v['passed'] for k, v in benefit.items()},
                      'completed_pairs': data.get('completed_pairs')}))
    if not passed:
        sys.exit(20)
