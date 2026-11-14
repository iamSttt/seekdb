#!/usr/bin/env python3
"""Verify prepared confirmation sources/plan; no database or process execution."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def function(text, name):
    node = next(n for n in ast.parse(text).body if isinstance(n, ast.FunctionDef) and n.name == name)
    return '\n'.join(text.splitlines()[node.lineno - 1:node.end_lineno])


def verify():
    provenance = json.loads((HERE / 'source-provenance.json').read_bytes())
    for relative, digest in provenance['files'].items():
        path = (HERE / relative).resolve(strict=True)
        path.relative_to(HERE)
        if sha(path) != digest:
            raise ValueError('prepared source changed: ' + relative)
    freeze_path = HERE / 'freeze.json'
    if freeze_path.exists():
        for relative, digest in json.loads(freeze_path.read_bytes())['files'].items():
            path = (HERE / relative).resolve(strict=True)
            path.relative_to(HERE)
            if sha(path) != digest:
                raise ValueError('frozen integration changed: ' + relative)
    original = (HERE / 'evidence/best_screen-original.py').read_text(encoding='utf8')
    candidate = (HERE / 'seed_screen.py').read_text(encoding='utf8')
    summary = function(original, 'summarize')
    if summary != function(candidate, 'summarize'):
        raise ValueError('original statistics function changed')
    if hashlib.sha256(summary.encode()).hexdigest() != provenance['summarize_function_sha256']:
        raise ValueError('statistics source hash differs')
    screen = module('confirmation_seed_screen', HERE / 'seed_screen.py')
    screen.verify_vendor()
    plan = json.loads((HERE / 'plan.json').read_bytes())
    if screen.plan() != plan['plan'] or plan['expected_pairs'] != 12 or plan['seed'] != 19087:
        raise ValueError('confirmation plan differs')
    helper = module('confirmation_small_compute', HERE / 'vendor/minivec/tools/small_compute.py')
    original_helper = ast.parse((HERE / 'evidence/small_compute-original.py').read_text(encoding='utf8'))
    current_helper = ast.parse((HERE / 'vendor/minivec/tools/small_compute.py').read_text(encoding='utf8'))
    functions = lambda tree: {n.name: ast.dump(n, include_attributes=False) for n in tree.body
                              if isinstance(n, (ast.FunctionDef, ast.ClassDef))}
    old_functions, new_functions = functions(original_helper), functions(current_helper)
    changed = sorted(name for name in old_functions.keys() | new_functions.keys()
                     if old_functions.get(name) != new_functions.get(name))
    if changed != ['check_highest_primary_plan', 'highest_primary_plan', 'highest_primary_seed',
                   'main', 'workload', 'workload_identity']:
        raise ValueError('measurement helper changes exceed the frozen seed-plumbing scope')
    for cpu in (1, 2):
        planned = [{k: x[k] for k in ('scenario', 'seed', 'pair_index', 'order')}
                   for x in plan['plan'] if x['cpu_count'] == cpu]
        if helper.highest_primary_plan('ee', cpu, True) != planned:
            raise ValueError('actual helper does not use frozen confirmation seed/order')
        if any(x['seed'] != 7319 for x in helper.highest_primary_plan('ee', cpu)):
            raise ValueError('ordinary historical seed was changed')
    return {'status': 'confirmation_sources_verified_not_executed', 'seed': 19087,
            'plan_sha256': sha(HERE / 'plan.json'), 'summarize_function_sha256': provenance['summarize_function_sha256'],
            'draft_small_compute_sha256': provenance['draft_small_compute_sha256'],
            'new_database_pairs': 0, 'new_pgo_builds': 0}


if __name__ == '__main__':
    print(json.dumps(verify(), sort_keys=True))
