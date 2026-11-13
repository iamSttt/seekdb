#!/usr/bin/env python3
"""Actual-image distance projection and TopK against exact float32 oracle."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import struct

CASE_KINDS=("unindexed_all","indexed_top1","indexed_top10","indexed_rescue_filter","indexed_near_tie","indexed_extreme")
EXPECTED_CASES=tuple(f"{path}_d{dimension}_{kind}" for dimension in (128,512)
                     for path in ("serial","fanout") for kind in CASE_KINDS)
ENV_KEYS=("MVDB_PROTOCOL","MVDB_OUT","MVDB_SINGLE_THREAD","MVDB_QUERY_THREADS","MVDB_QUERY_FANOUT_ROWS",
          "MVDB_WAKE_US_OVERRIDE","MVDB_PARSE_THREADS","MVDB_ANN_PROBE_PERCENT","MVDB_ANN_OFF","MVDB_BATCH8_TRACE")
SCHEMA="actual-image-tighter-certificate-v1"

def require(ok,message):
    if not ok:raise AssertionError(message)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def digest(value):return hashlib.sha256(json.dumps(value,separators=(",",":"),sort_keys=True).encode()).hexdigest()
def double_bits(value):return struct.pack('!d',float(value)).hex()

def load_oracle(repo):
    spec=importlib.util.spec_from_file_location("tighter_actual_topk_oracle",repo/'minivec/tests/topk_bounds.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

@contextmanager
def environment(threads):
    saved={key:os.environ.get(key) for key in ENV_KEYS}
    selected={"MVDB_QUERY_THREADS":str(threads),"MVDB_PARSE_THREADS":"1","MVDB_ANN_OFF":"1"}
    try:
        for key in ENV_KEYS:os.environ.pop(key,None)
        os.environ.update(selected);yield selected
    finally:
        for key,value in saved.items():
            if value is None:os.environ.pop(key,None)
            else:os.environ[key]=value

def fixture(oracle,dimension):
    oracle.DIMENSION=dimension;rows=[];key=100
    for exponent in (-100,0,100):
        h=2.0**exponent;t=2.0**(exponent-27)
        midpoint=[h,t,t]
        below=midpoint[:];below[1]=oracle.F32.unpack(oracle.U32.pack(oracle.U32.unpack(oracle.F32.pack(t))[0]-1))[0]
        above=midpoint+[2.0**(exponent-40)]
        rescue=midpoint+[2.0**(exponent-29)]*(2 if dimension==128 else 8)
        for group,values in ((2,midpoint),(2,below),(2,above),(1,rescue),(1,rescue)):
            rows.append((key,group,oracle.stored_vector(values)));key+=1
    rows.extend([(-900,0,oracle.stored_vector()),(-901,0,oracle.stored_vector([-0.0])),
                 (800,3,oracle.stored_vector([oracle.MAX_F32,-oracle.MAX_F32,oracle.MIN_F32])),
                 (801,3,oracle.stored_vector([-oracle.MAX_F32,oracle.MAX_F32,-oracle.MIN_F32])),
                 (oracle.NEAR_EARLY,4,oracle.stored_vector([2.0**30,16])),
                 (oracle.NEAR_WINNER,4,oracle.stored_vector([2.0**30,16,8,8]))])
    rng=random.Random(0x71BC44)
    for i in range(12):rows.append((500+i,0,oracle.stored_vector(rng.randint(-1024,1024)/512 for _ in range(dimension))))
    records={key:(group,vector) for key,group,vector in rows}
    require(len(records)==len(rows)==33,"fixture inventory")
    zero=oracle.stored_vector();near=oracle.stored_vector([0,0,0,oracle.MIN_F32])
    extreme=oracle.stored_vector([oracle.MAX_F32,-oracle.MAX_F32,oracle.MIN_F32,-oracle.MIN_F32])
    specs=[("unindexed_all",zero,None,33),("indexed_top1",zero,None,1),("indexed_top10",zero,None,10),
           ("indexed_rescue_filter",zero,1,10),("indexed_near_tie",near,4,1),("indexed_extreme",extreme,None,10)]
    return rows,records,specs

def expected_rows(oracle,records,query,group,k):
    ranked=sorted((oracle.canonical_squared(vector,query),key) for key,(category,vector) in records.items()
                  if group is None or category==group)
    return [[key,double_bits(distance)] for distance,key in ranked[:k]]

def expected_checks(repo):
    oracle=load_oracle(repo);out=[]
    for dimension in (128,512):
        _,records,specs=fixture(oracle,dimension)
        for path in ("serial","fanout"):
            for kind,query,group,k in specs:
                expected=expected_rows(oracle,records,query,group,k)
                out.append({"name":f"{path}_d{dimension}_{kind}","path":path,"dimension":dimension,"kind":kind,
                            "group":group,"k":k,"mode":"exact","expected_rows":expected,"expected_sha256":digest(expected)})
    return out

def process_identity(session,binary_sha,selected,threads):
    pid=session.proc.pid;proc=Path('/proc')/str(pid)
    require(sha(proc/'exe')==binary_sha,"actual process ELF mismatch")
    observed={}
    for item in (proc/'environ').read_bytes().split(b'\0'):
        key,separator,value=item.partition(b'=')
        if separator and key.decode(errors='strict') in ENV_KEYS:observed[key.decode()]=value.decode()
    require(observed==selected,"actual environment differs")
    tasks=sorted(int(path.name) for path in (proc/'task').iterdir())
    require(len(tasks)==threads and pid in tasks,"native task count differs")
    return {"pid":pid,"native_tasks":tasks,"binary_sha256":binary_sha,"runtime_environment":observed}

def run_group(oracle,binary,binary_sha,logs,checks,sessions,dimension,path):
    rows,records,specs=fixture(oracle,dimension);threads=1 if path=='serial' else 2
    key=f'{path}_d{dimension}';log=logs/(key+'.stderr.log')
    config=json.loads((oracle.PROTOCOL_CHECK/'config.json').read_bytes())
    with environment(threads) as selected:
        session=oracle.Session(config,log,command=[str(binary),'--data','{data_dir}','--protocol','jsonl'],case_timeout=180)
        try:
            session.start()
            oracle.execute(session,f'CREATE TABLE docs(id BIGINT PRIMARY KEY, grp BIGINT, embedding VECTOR({dimension}));','create')
            oracle.execute(session,'BEGIN;','begin')
            values=','.join(f'({key},{group},{vector.sql})' for key,group,vector in rows)
            oracle.execute(session,'INSERT INTO docs VALUES '+values+';','insert',affected=len(rows))
            oracle.execute(session,'COMMIT;','commit')
            identity=process_identity(session,binary_sha,selected,threads)
            sessions.append({'name':key,'dimension':dimension,'path':path,**identity})
            for kind,query,group,k in specs:
                if kind=='indexed_top1':oracle.execute(session,'CREATE VECTOR INDEX docs_v ON docs(embedding);','index')
                name=f'{path}_d{dimension}_{kind}';where='' if group is None else f' WHERE grp={group}'
                sql=f'SELECT id,L2_DISTANCE(embedding,{query.sql}) AS d FROM docs{where} ORDER BY d ASC LIMIT {k};'
                response=oracle.execute(session,sql,name,mode='exact')
                actual=response.get('rows')
                require(response.get('columns')==['id','d'] and type(actual) is list and
                        all(type(row) is list and len(row)==2 and type(row[0]) is int and
                            type(row[1]) in (int,float) and math.isfinite(row[1]) for row in actual),'invalid typed response')
                normalized=[[row[0],double_bits(row[1])] for row in actual]
                expected=expected_rows(oracle,records,query,group,k)
                require(normalized==expected,f'{name}: exact projected distance or TopK order mismatch')
                if kind=='indexed_near_tie':require(normalized[0][0]==oracle.NEAR_WINNER,'late exact tie fixture not exercised')
                require(len(list((Path('/proc')/str(session.proc.pid)/'task').iterdir()))==threads,'worker lifetime changed')
                checks.append({'name':name,'status':'passed','path':path,'dimension':dimension,'kind':kind,'group':group,'k':k,
                               'mode':'exact','session':key,'actual_rows':normalized,'expected_rows':expected,
                               'actual_sha256':digest(normalized),'expected_sha256':digest(expected)})
            session.shutdown()
            require(b'MVDB_BATCH8_TRACE ' not in log.read_bytes(),'timing/algorithm trace unexpectedly enabled')
        finally:session.close()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);args=parser.parse_args()
    repo=args.repo.resolve();binary=args.binary.resolve();output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    logs=output.parent/(output.stem+'-logs');logs.mkdir(exist_ok=False)
    report={'schema':SCHEMA,'status':'running','binary_sha256':sha(binary),'expected_cases':list(EXPECTED_CASES),'checks':[],'sessions':[],
            'mode':'exact','trace_enabled':False,'scope':'actual extracted image ELF; exact JSONL projection and TopK, no performance claim',
            'fp_scope':'normal process ABI environment; no altered FP controls or per-query CW telemetry',
            'worker_scope':'forced query settings plus observed native task counts; no per-function branch counter',
            'oracle_source_sha256':sha(repo/'minivec/tests/topk_bounds.py'),'transport_source_sha256':sha(repo/'protocol_check/_transport.py')}
    try:
        oracle=load_oracle(repo)
        for dimension in (128,512):
            for path in ('serial','fanout'):run_group(oracle,binary,report['binary_sha256'],logs,report['checks'],report['sessions'],dimension,path)
        require(tuple(row['name'] for row in report['checks'])==EXPECTED_CASES,'case inventory/order differs')
        require(sha(binary)==report['binary_sha256'],'ELF changed during gate')
        report.update(status='passed',checks_count=len(report['checks']))
        print('PASS actual image tighter certificate oracle: 24 cases, exact distance bits and TopK')
    except Exception as exc:
        report.update(status='failed',error=f'{type(exc).__name__}: {exc}');raise
    finally:
        report['stderr_logs']=[{'path':str(path.relative_to(output.parent)),'bytes':path.stat().st_size,'sha256':sha(path)} for path in sorted(logs.glob('*.log'))]
        output.write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
