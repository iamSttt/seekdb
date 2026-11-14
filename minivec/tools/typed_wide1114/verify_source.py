#!/usr/bin/env python3
"""Reject source/driver drift before building the isolated typed-wide factor."""
import hashlib
import json
from pathlib import Path
import subprocess
from run_ablation import PLAN, require, sha

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
git=['git','-C',str(ROOT)]
expected=PLAN['roles']['candidate']
paths={p.relative_to(ROOT).as_posix() for directory in ('minivec/src','minivec/third_party')
       for p in (ROOT/directory).rglob('*') if p.is_file()}
paths.update(['Dockerfile','.dockerignore'])
require(paths==set(expected['build_inputs']),'candidate runtime inventory changed')
for path,digest in expected['build_inputs'].items():
    require(sha(ROOT/path)==digest,'working runtime bytes changed: '+path)
    raw=subprocess.check_output(git+['show',expected['source_commit']+':'+path])
    require(hashlib.sha256(raw).hexdigest()==digest,'runtime commit differs: '+path)
    head=subprocess.check_output(git+['show','HEAD:'+path])
    require(head==raw,'driver HEAD changes runtime: '+path)
baseline=PLAN['roles']['baseline']['build_inputs']
changed={p for p in paths|set(baseline) if expected['build_inputs'].get(p)!=baseline.get(p)}
require(changed==set(PLAN['runtime_paths_changed']),'unplanned runtime factor')
require(subprocess.check_output(git+['rev-parse','HEAD:.github/workflows/minivec-score.yml']).decode().strip()
        =='9bbb0c70973e509fb3dbbda02f25c8e18601a3e4','workflow changed')
freeze=json.loads((HERE/'freeze.json').read_bytes())
for item in freeze['files']:
    require(sha(ROOT/item['path'])==item['sha256'],'frozen driver/test changed: '+item['path'])
print(json.dumps({'status':'source_identity_verified','runtime_commit':expected['source_commit'],
                  'build_input_files':len(paths),'runtime_changes':sorted(changed),
                  'release_gate_passed':False,'measurements_executed':0}))
