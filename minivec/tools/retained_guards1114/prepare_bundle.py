"""Read local history, copy immutable inputs; never build or run a database."""
from pathlib import Path
import hashlib
import json
import subprocess

HERE=Path(__file__).resolve().parent
W=Path('C:/Users/Lenovo/Documents/Codex/2026-11-12/iamsttt-seekdb-https-github-com-iamsttt/work')
REPO=W/'seekdb'
ACTIVE=HERE.parent/'round2-20261114-from-best'
R='33a52a7dd4426af059ec2334b7c93a07c38c93c9'
ELF='acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
INPUTS=['Dockerfile','.dockerignore','minivec/src','minivec/third_party',
        'minivec/tools/pgo/train.cpp','minivec/tools/pgo/run-training.sh']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def git(*args):return subprocess.check_output(['git','-c',f'safe.directory={REPO.as_posix()}','-C',str(REPO),*args])
def copy(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(src.read_bytes());return sha(dst)
def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n',encoding='utf8')

vendor=ACTIVE/'minivec/tools/research1114'
manifest=json.loads((vendor/'source-manifest.json').read_bytes())
for item in manifest['files']:
    assert sha(vendor/item['path'])==item['sha256'],item
    copy(vendor/item['path'],HERE/'vendor'/item['path'])
copy(vendor/'source-manifest.json',HERE/'vendor/source-manifest.json')
proofs=[]
for label,name in [('first','matched-insert-6d2c433'),('repeat','matched-insert-repeat-6d2c433')]:
    src=W/'performance'/name/'r2/proof.json';data=json.loads(src.read_bytes())
    assert data['source_commit']==R and data['binary_sha256']==ELF and data['new_pgo_builds']==1
    dst=HERE/'evidence'/f'R2-{label}-proof.json';digest=copy(src,dst)
    proofs.append(dict(file=str(dst.relative_to(HERE)),sha256=digest,source=str(src)))
paths=git('ls-tree','-r','--name-only',R,'--',*INPUTS).decode().splitlines()
assert len(paths)==28
source={}
for path in paths:
    data=git('show',R+':'+path)
    out=HERE/'reference-context'/path;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
    source[path]=sha(out)
assert source==json.loads((HERE/'evidence/R2-first-proof.json').read_bytes())['source_sha256']
assert source==json.loads((HERE/'evidence/R2-repeat-proof.json').read_bytes())['source_sha256']
save(HERE/'reference-inputs.json',dict(source_commit=R,expected_binary_sha256=ELF,input_paths=INPUTS,source_sha256=source,historical_proofs=proofs))
policy_files=['highest-primary24-preregistration-v2.json','highest-primary24-spill-field-erratum.json','evaluate_highest_primary24_screen.py']
policies=[]
for name in policy_files:
    src=W/name;dst=HERE/'evidence'/name
    policies.append(dict(file=str(dst.relative_to(HERE)),sha256=copy(src,dst),source=str(src)))
local_elf=W/'exact-cost-attempt1-raw/programs/retained-minivec'
assert sha(local_elf)==ELF
save(HERE/'prepared.json',dict(status='prepared_not_executed',vendor_manifest_sha256=sha(HERE/'vendor/source-manifest.json'),
    reference_manifest_sha256=sha(HERE/'reference-inputs.json'),historical_policy=policies,
    local_reference_elf=dict(path=str(local_elf),sha256=ELF,bytes=local_elf.stat().st_size,scope='identity checked only; never used as current rebuild substitute'),
    new_pgo_builds=0,new_database_pairs=0,release_gate_passed=False))
print(json.dumps(dict(status='prepared_not_executed',reference_files=len(paths),vendor_files=len(manifest['files']))))
