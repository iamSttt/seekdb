#!/usr/bin/env python3
"""Static adapter regressions using synthetic files, never a database run."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def invoke(script, *args, expected=0):
    run = subprocess.run([sys.executable, '-B', str(HERE / script), *map(str, args)],
                         capture_output=True, text=True)
    assert run.returncode == expected, (run.returncode, run.stdout, run.stderr)
    return run


def write(path, value):
    path.write_text(json.dumps(value) + '\n', encoding='utf8')


with tempfile.TemporaryDirectory(prefix='pgo-adapter-static-') as td:
    root = Path(td)
    write(root / 'source-manifest.json', {'synthetic_test': True})
    for label in ('A', 'B'):
        d = root / label
        d.mkdir()
        (d / 'binary').write_bytes(b'SYNTHETIC-IDENTITY-TEST-NOT-AN-ELF')
        digest = hashlib.sha256((d / 'binary').read_bytes()).hexdigest()
        (d / 'builder-ELF-sha256.txt').write_text(digest + '\n')
        write(d / 'image.json', [{'Id': 'synthetic-release-' + label}])
        write(d / 'builder-image.json', [{'Id': 'synthetic-builder-' + label}])
        records = [dict(status='passed', responses=n) for n in [951, 41, 14] * 2]
        (d / 'training-verification.txt').write_text(''.join(json.dumps(x) + '\n' for x in records))
        (d / 'profile-sha256.txt').write_text(''.join(f'{n:064x} /pgo/file{n}.gcda\n' for n in range(6)))
        (d / 'corpus-sha256.txt').write_text('synthetic-corresponding-corpus\n')
        invoke('build_audit.py', 'record', '--root', root, '--label', label,
               '--source', 'a' * 40, '--binary', d / 'binary')
    invoke('build_audit.py', 'compare', '--root', root)
    assert json.loads((root / 'build-comparison.json').read_bytes())['ELF_equal']
    proof = json.loads((root / 'B/build-proof.json').read_bytes())
    proof['binary_sha256'] = 'f' * 64
    write(root / 'B/build-proof.json', proof)
    invoke('build_audit.py', 'compare', '--root', root, expected=42)
    blocked = json.loads((root / 'build-comparison.json').read_bytes())
    assert blocked['status'] == 'blocked_nonreproducible_ELF'
    assert not blocked['performance_allowed'] and not blocked['faster_build_selection']
    (root / 'A/profile-sha256.txt').write_text('missing feedback\n')
    bad = subprocess.run([sys.executable, '-B', str(HERE / 'build_audit.py'), 'record',
                          '--root', str(root), '--label', 'A', '--source', 'a' * 40,
                          '--binary', str(root / 'A/binary')], capture_output=True, text=True)
    assert bad.returncode != 0 and 'six translation-unit profiles' in bad.stderr
    out = root / 'plan.json'
    invoke('source_factor.py', '--plan-only', '--baseline', root / 'control',
           '--baseline-build-proof', root / 'control-proof.json', '--candidate', root / 'candidate',
           '--candidate-source', 'a' * 40, '--candidate-evidence', root / 'candidate-proof.json',
           '--launcher', root / 'launcher', '--output', out)
    plan = json.loads(out.read_bytes())
    assert plan['expected_pairs'] == 12 and plan['completed_pairs'] == 0
    assert plan['reference_source'] == '760f66220f2c7186b4ba295f9e3065650e549f9e'
    assert plan['reference_binary_sha256'] == 'fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456'
    assert [(p['cpu_count'], p['scenario']) for p in plan['plan']].count((1, 'spill_high_q16')) == 2
    assert [(p['cpu_count'], p['scenario']) for p in plan['plan']].count((2, 'wide768')) == 2
    assert not plan['release_gate_passed'] and not plan['same_source_guard_passed']
print(json.dumps({'status': 'static_adapter_tests_passed', 'database_runs': 0,
                  'tested': ['equal-build identity', 'different-build blocking',
                             'missing feedback rejection', 'source-factor frozen plan']}))
