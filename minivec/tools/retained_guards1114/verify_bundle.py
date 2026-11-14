#!/usr/bin/env python3
"""Check the integration's copied bytes and unchanged fixed plan; no DB runs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def verify():
    manifest = json.loads((HERE / 'copied-sources.json').read_bytes())
    entries = manifest['copied_files']
    paths = [item['path'] for item in entries]
    if len(paths) != len(set(paths)) or len(paths) != 54:
        raise ValueError('copied bundle inventory differs')
    changed = []
    for item in entries:
        path = (HERE / item['path']).resolve(strict=True)
        path.relative_to(HERE)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != item['integrated_sha256']:
            raise ValueError('integrated bytes changed: ' + item['path'])
        if item['byte_identical']:
            if digest != item['source_sha256']:
                raise ValueError('source copy differs: ' + item['path'])
        else:
            changed.append(item['path'])
    if changed != ['retained_guards.py']:
        raise ValueError('unexpected integration adaptation')
    original_plan = (HERE / 'plan-only.json').read_bytes()
    if hashlib.sha256(original_plan).hexdigest() != manifest['plan_sha256']:
        raise ValueError('frozen plan file changed')
    spec = importlib.util.spec_from_file_location('integrated_retained_guards', HERE / 'retained_guards.py')
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)  # Independently verifies vendor/rule manifests.
    expected = [dict(cpu_count=cpu, **row) for cpu in (1, 2) for row in guard.plan(cpu)]
    prepared = json.loads(original_plan)
    if prepared['plan'] != expected or prepared['expected_pairs'] != 12:
        raise ValueError('executable plan differs from original prepared plan')
    return {'status': 'copied_bytes_and_plan_verified', 'copied_files': len(entries),
            'unchanged_files': len(entries) - len(changed), 'adapted_files': changed,
            'plan_sha256': manifest['plan_sha256'], 'new_database_pairs': 0, 'new_pgo_builds': 0}


if __name__ == '__main__':
    print(json.dumps(verify(), sort_keys=True))
