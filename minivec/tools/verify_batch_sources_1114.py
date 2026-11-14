#!/usr/bin/env python3
"""Frozen source and harness checks before any native test or measurement."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
def require(ok,message):
    if not ok:raise ValueError(message)
def sha(raw):return hashlib.sha256(raw).hexdigest()
def blob(ref,path):return subprocess.check_output(['git','-C',str(ROOT),'show',ref+':'+path])
freeze=json.loads((ROOT/'validation/batch-factors-1114-freeze.json').read_bytes())
for item in freeze['files']:
    require(sha((ROOT/item['path']).read_bytes())==item['sha256'],'frozen harness changed: '+item['path'])
plans={key:json.loads((ROOT/f'minivec/tools/batch_{key}1114/plan.json').read_bytes()) for key in ('a','b')}
for key,plan in plans.items():
    for role,expected in plan['roles'].items():
        ref=expected['source_commit']
        listed=subprocess.check_output(['git','-C',str(ROOT),'ls-tree','-r','--name-only',ref,'Dockerfile','.dockerignore','minivec/src','minivec/third_party']).decode().splitlines()
        require(set(listed)==set(expected['build_inputs']),'Git source inventory changed')
        for path,digest in expected['build_inputs'].items():
            require(sha(blob(ref,path))==digest,'Git build input differs: '+ref+':'+path)
    b,c=(plan['roles'][role]['build_inputs'] for role in ('baseline','candidate'))
    changed={path for path in set(b)|set(c) if b.get(path)!=c.get(path)}
    require(changed==({'minivec/src/flat_index.cpp','minivec/src/flat_index.h'} if key=='a' else {'minivec/src/db.cpp'}),'nonisolated factor')
    candidate=plan['roles']['candidate']['source_commit']
    prep_path=f'validation/batch-{key}-1114-frozen-plan.json'
    prep_raw=blob(candidate,prep_path)
    require(sha(prep_raw)==plan['source_preparation_plan_sha256'],'native preparation plan changed')
    for path,record in json.loads(prep_raw)['runtime_files'].items():
        require(sha(blob(candidate,path))==record['working_lf_sha256'],'native source/test differs: '+path)
require(plans['a']['roles']['baseline']==plans['b']['roles']['baseline'],'factors do not share baseline identity')
for path,digest in plans['a']['roles']['candidate']['build_inputs'].items():
    require(sha(blob('HEAD',path))==digest and sha((ROOT/path).read_bytes())==digest,'CI HEAD runtime drift: '+path)
workflow=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD:.github/workflows/minivec-score.yml']).decode().strip()
require(workflow==freeze['workflow_blob'],'workflow changed')
print(json.dumps({'status':'verified_before_native','factors':['A','B'],'frozen_files':len(freeze['files']),'measurement_executed':False}))
