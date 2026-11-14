"""Verify current R root inputs, six training checks and extracted ELF identity."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

HERE=Path(__file__).resolve().parent
def require(value,message):
    if not value:raise ValueError(message)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('context','binary','build-log','image-json','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();require(not a.output.exists(),'refuse overwrite')
    manifest=json.loads((HERE/'reference-inputs.json').read_bytes())
    actual={f.relative_to(a.context).as_posix():sha(f) for f in a.context.rglob('*') if f.is_file()}
    require(actual==manifest['source_sha256'] and len(actual)==28,'current git archive is not the complete original R28 inputs')
    program=a.binary.read_bytes()
    require(program[:6]==b'\x7fELF\x02\x01' and int.from_bytes(program[18:20],'little')==62,'not x86-64 ELF')
    require(sha(a.binary)==manifest['expected_binary_sha256'],'current R rebuild did not reproduce acdd; do not substitute imported ELF')
    checks=[]
    for line in a.build_log.read_text(encoding='utf8',errors='replace').splitlines():
        start,end=line.find('{'),line.rfind('}')
        if start<0 or end<start:continue
        try:value=json.loads(line[start:end+1])
        except json.JSONDecodeError:continue
        if value.get('oracle')=='independent exact dyadic distance and complete response comparison':checks.append(value)
    require(len(checks)==6 and all(x.get('status')=='passed' and x.get('responses',0)>0 for x in checks),'six current PGO training oracle successes missing')
    require(Counter(x['expected_file'] for x in checks)==Counter({f'phase-{i}.expected.jsonl':2 for i in range(3)}),'PGO training phase inventory differs')
    images=json.loads(a.image_json.read_bytes())
    require(len(images)==1 and images[0]['Config']['Entrypoint']==['/minivec','--data','/data'],'actual image inventory/entrypoint differs')
    report=dict(status='verified_current_root_build',source_commit=manifest['source_commit'],
        binary_sha256=sha(a.binary),image_id=images[0]['Id'],source_sha256=actual,
        new_reference_root_pgo_builds=1,training_oracles=checks,build_log_sha256=sha(a.build_log),
        image_inspection_sha256=sha(a.image_json),official_binary_identity_known=False,
        scope='Current no-cache original R root build and exact historical ELF reproduction; does not replace candidate correctness gates.')
    a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(json.dumps(dict(status=report['status'],binary_sha256=report['binary_sha256'],training_checks=6)))
if __name__=='__main__':main()
