#!/usr/bin/env python3
"""Bind stripped root identity and preserve actual compiler assembly; no timings."""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import textwrap

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--contexts',type=Path,required=True)
p.add_argument('--plan',type=Path,required=True)
a=p.parse_args();plan=json.loads(a.plan.read_bytes())
report={'status':'assembly_collected_not_performance','plan_sha256':sha(a.plan),'roles':{},
        'performance_pairs':0,'release_gate_passed':False,'formal_submission':False}
for role in ('base','typed','noinline'):
    directory=a.output/role
    stripped=sha(directory/'root-equivalent.elf')
    if role in plan['expected_stripped_sha256']:
        assert stripped==plan['expected_stripped_sha256'][role],role+' is not the actual prior root ELF'
    symbols=[]
    for line in (directory/'symbols.nm').read_text().splitlines():
        fields=line.split()
        if len(fields)!=4:continue
        address,size,kind,name=fields
        if 'execute_impl' not in name and 'JsonRowWriter' not in name:continue
        symbols.append({'address':int(address,16),'bytes':int(size,16),'kind':kind,'mangled':name})
    decoded=subprocess.check_output(['c++filt',*[s['mangled'] for s in symbols]],text=True).splitlines()
    assert len(decoded)==len(symbols)
    lines=(directory/'full-disassembly.asm').read_text().splitlines()
    starts={int(match.group(1),16):i for i,line in enumerate(lines)
            if (match:=re.match(r'^([0-9a-f]+) <.*>:$',line))}
    chunks=[]
    for symbol,name in zip(symbols,decoded):
        symbol['demangled']=name
        begin=starts.get(symbol['address'])
        if begin is None:continue
        end=begin+1
        while end<len(lines) and not re.match(r'^[0-9a-f]+ <.*>:$',lines[end]):end+=1
        body=lines[begin:end];chunks.extend(body)
        symbol['first_instructions']=body[:16]
        symbol['calls']=[line.strip() for line in body if re.search(r'\bcall\b',line)]
    (directory/'interesting-disassembly.asm').write_text('\n'.join(chunks)+'\n')
    inputs={q.relative_to(a.contexts/role).as_posix():sha(q) for q in (a.contexts/role).rglob('*') if q.is_file()}
    report['roles'][role]={'stripped_sha256':stripped,'unstripped_sha256':sha(directory/'unstripped.elf'),
        'root_input_sha256':inputs,'symbols':symbols,'sections':(directory/'sections.size').read_text(),
        'full_disassembly_sha256':sha(directory/'full-disassembly.asm')}
report['noinline_identical_ELF_to_typed']=report['roles']['noinline']['stripped_sha256']==report['roles']['typed']['stripped_sha256']
report['mechanism_conclusion']='Requires human review of actual core/lambda code and default-path stack work; ELF inequality alone is not evidence of a speedup.'
save(a.output/'summary.json',report)
# Preserve binary+full disassembly evidence even though the workflow has no
# artifact-upload permission. Wrapped base64 survives line-oriented log return.
members=[];buffer=io.BytesIO()
with tarfile.open(fileobj=buffer,mode='w:gz',format=tarfile.PAX_FORMAT) as archive:
    for item in sorted(a.output.rglob('*')):
        if not item.is_file():continue
        relative=item.relative_to(a.output).as_posix()
        archive.add(item,arcname=relative,recursive=False)
        members.append({'path':relative,'bytes':item.stat().st_size,'sha256':sha(item)})
raw=buffer.getvalue();assert len(raw)<=12*1024*1024,'archive exceeds explicit cap; never truncate'
(a.output/'assembly-raw.tar.gz.b64.txt').write_text('\n'.join(textwrap.wrap(base64.b64encode(raw).decode(),96))+'\n')
save(a.output/'archive-manifest.json',{'compressed_bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'members':members})
print(json.dumps({'status':report['status'],'same_noinline_ELF':report['noinline_identical_ELF_to_typed'],
                  'archive_bytes':len(raw),'roles':{k:v['stripped_sha256'] for k,v in report['roles'].items()}}))
