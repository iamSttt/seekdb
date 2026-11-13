"""Fail-closed actual-header and actual-image tighter-certificate evidence."""
import hashlib
import importlib.util
import json
import math
import struct
from pathlib import Path
import re

REFERENCE_SHA='7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9'
PRODUCTION_SHA='bdcc9e587677b91c0e57a0da8dd63eda7160c62fb9121e22cf5db19d243bf7c7'
ORACLE_SHA='74e1b99088943e073b5f4821844aabc7001a500ae2af9ef0c7dc309d8ed6262e'
EXPECTED_CHECKS=11452601

def require(value,message):
    if not value:raise AssertionError(message)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def read_json(path):
    def unique(items):
        result={}
        for key,value in items:
            require(key not in result,'duplicate JSON key');result[key]=value
        return result
    return json.loads(Path(path).read_bytes(),object_pairs_hook=unique)

def verify_tighter_evidence(reports,binary_sha):
    reports=Path(reports);repo=reports.parent;hashes={}
    production=repo/'minivec/src/distance.h';reference=repo/'minivec/tests/tighter_certificate_reference_distance.h'
    require(sha(production)==PRODUCTION_SHA,'actual production header differs from reviewed/micro-tested candidate')
    require(sha(reference)==REFERENCE_SHA,'old reference header differs from canonical Git bytes')
    counts=[]
    for mode in ('normal','sanitized'):
        path=reports/f'tighter-certificate-{mode}.log';text=path.read_text()
        numbers=re.findall(r'^tighter_bound_checks=(\d+)$',text,re.M)
        require(numbers==[str(EXPECTED_CHECKS)] and text.splitlines().count('tighter_bound=PASS')==1 and
                text.splitlines().count('tighter_bound_header=actual_minivec_src_distance_h')==1 and
                text.splitlines().count('tighter_bound_cases=6748 midpoint_fixtures=180 rescue_fixtures=10')==1,
                'actual-header normal/sanitized inventory or source marker differs')
        counts.append(int(numbers[0]));hashes[path.name]=sha(path)
    require(counts[0]==counts[1],'actual-header normal/sanitized count mismatch')
    path=reports/'tighter-oracle.json';oracle=read_json(path)
    require(oracle.get('status')=='generated_independent_oracle' and oracle.get('cases')==6748 and
            oracle.get('kinds')=={'0':6558,'1':180,'2':10} and oracle.get('maximum_dimension')==262144 and
            oracle.get('bytes')==11425564 and oracle.get('sha256')==ORACLE_SHA and
            oracle.get('seed')=='0x6c42b9e173' and oracle.get('native_executions')==0,'independent fixture identity differs')
    hashes[path.name]=sha(path)
    generation=reports/'tighter-fixture-generation.log'
    require(read_json(generation)==oracle,'generation log/fixture metadata differ');hashes[generation.name]=sha(generation)
    spec=importlib.util.spec_from_file_location('bound_actual_image_gate',repo/'minivec/tests/test_tighter_certificate_db.py')
    gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
    require(len(gate.EXPECTED_CASES)==24,'unexpected actual-image case plan')
    path=reports/'image-binary-tighter-certificate.json';report=read_json(path);checks=report.get('checks',[])
    require(report.get('schema')==gate.SCHEMA=='actual-image-tighter-certificate-v2' and report.get('status')=='passed' and
            report.get('projection_semantics')==gate.PROJECTION_SEMANTICS and
            report.get('binary_sha256')==binary_sha and report.get('mode')=='exact' and report.get('trace_enabled') is False and
            report.get('expected_cases')==list(gate.EXPECTED_CASES) and report.get('checks_count')==24 and
            [row.get('name') for row in checks]==list(gate.EXPECTED_CASES),'actual image identity/case inventory differs')
    require(report.get('oracle_source_sha256')==sha(repo/'minivec/tests/topk_bounds.py') and
            report.get('transport_source_sha256')==sha(repo/'protocol_check/_transport.py'),'actual image oracle/transport source differs')
    expected=gate.expected_checks(repo)
    for actual,want in zip(checks,expected):
        require(actual.get('status')=='passed' and all(actual.get(key)==value for key,value in want.items()),'actual image oracle expectation differs')
        response=actual.get('raw_response',{})
        rows=response.get('rows')
        require(response.get('columns')==['id','d'] and type(rows) is list and
                all(type(row) is list and len(row)==2 and type(row[0]) is int and type(row[1]) in (int,float) and
                    math.isfinite(row[1]) for row in rows),'preserved actual response shape differs')
        require([[row[0],struct.pack('!d',float(row[1])).hex()] for row in rows]==actual.get('actual_rows'),
                'preserved parsed response differs from normalized actual rows')
        require(actual.get('actual_rows')==want['expected_rows'] and actual.get('actual_sha256')==want['expected_sha256'] and
                actual.get('session')==f"{want['path']}_d{want['dimension']}",'actual image response bits/TopK differ')
    session_names=[f'{path}_d{dimension}' for dimension in (128,512) for path in ('serial','fanout')]
    sessions=report.get('sessions',[])
    require([row.get('name') for row in sessions]==session_names,'actual process session inventory differs')
    for row in sessions:
        threads=1 if row['path']=='serial' else 2;tasks=row.get('native_tasks',[]);pid=row.get('pid')
        require(row.get('binary_sha256')==binary_sha and type(pid) is int and pid>0 and
                len(tasks)==threads and len(set(tasks))==threads and pid in tasks and
                all(type(tid) is int and tid>0 for tid in tasks) and
                row.get('runtime_environment')=={'MVDB_QUERY_THREADS':str(threads),'MVDB_PARSE_THREADS':'1','MVDB_ANN_OFF':'1'},
                'actual process ELF/environment/worker evidence differs')
        require(row['name']==f"{row['path']}_d{row['dimension']}" and row['dimension'] in (128,512),'actual process shape differs')
    logs=report.get('stderr_logs',[])
    expected_logs={f'image-binary-tighter-certificate-logs/{name}.stderr.log' for name in session_names}
    require(len(logs)==4 and {row.get('path') for row in logs}==expected_logs,'actual process stderr inventory differs')
    for row in logs:
        file=reports/row['path'];require(file.stat().st_size==row['bytes'] and sha(file)==row['sha256'],'actual process stderr digest differs')
        require(b'MVDB_BATCH8_TRACE ' not in file.read_bytes(),'unexpected algorithm trace enabled');hashes[row['path']]=sha(file)
    hashes[path.name]=sha(path)
    return {'schema_version':1,'checks_per_mode':counts[0],'fixture_cases':6748,'fixture_sha256':ORACLE_SHA,
            'actual_image_checks':24,'mode':'exact','projection_semantics':gate.PROJECTION_SEMANTICS,'binary_sha256':binary_sha,'production_header_sha256':PRODUCTION_SHA,
            'reference_header_sha256':REFERENCE_SHA,'file_sha256':hashes}
