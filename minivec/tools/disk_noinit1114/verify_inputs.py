#!/usr/bin/env python3
"""Verify frozen real Git-byte identities before any native test or benchmark."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
plan_path = ROOT / 'validation/disk-noinit-plan-20261114.json'
plan = json.loads(plan_path.read_bytes())
for family in ('runtime_inputs', 'driver_inputs'):
    for name, expected in plan[family].items():
        actual = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f'{family} mismatch: {name}: {actual} != {expected}')
print(json.dumps({'status': 'verified', 'base': plan['base_source'],
                  'runtime_inputs': plan['runtime_inputs'],
                  'driver_inputs': plan['driver_inputs'],
                  'plan_sha256': hashlib.sha256(plan_path.read_bytes()).hexdigest(),
                  'scope': 'Frozen source and driver identity only; no correctness/performance inference.'}, indent=2))
