#!/usr/bin/env python3
"""Local end-to-end A/B diagnostic. NOT official cgroup scoring.

Uses a zero-result SELECT as a write acknowledgement because the provisional
TSV wire format has no OK framing. This extra query is included in timing.
Reports process VmHWM/io counters, not cgroup memory.peak/io.stat; no score claim.
"""
import argparse
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', default='./minivec')
    ap.add_argument('--rows', type=int, default=10000)
    ap.add_argument('--queries', type=int, default=1000)
    ap.add_argument('--dim', type=int, default=128)
    ap.add_argument('--seed', type=int, default=7319)
    ap.add_argument('--output')
    ap.add_argument('--index', action='store_true')
    args = ap.parse_args()
    rng = random.Random(args.seed)
    f32 = lambda x: struct.unpack('<f', struct.pack('<f',x))[0]
    vectors = [[f32(rng.gauss(0,1)) for _ in range(args.dim)] for _ in range(args.rows)]
    queries = [[f32(rng.gauss(0,1)) for _ in range(args.dim)] for _ in range(args.queries)]
    vector_sql = lambda v: "'["+','.join(map(str,v))+ "]'"
    batches = []
    for start in range(0,args.rows,100):
        batches.append('INSERT INTO t VALUES '+','.join(
            f'({i},{i%10},{vector_sql(vectors[i])})'
            for i in range(start,min(start+100,args.rows)))+';\nSELECT id FROM t WHERE id=-1;\n')
    query_sql = [f'SELECT id,L2_DISTANCE(v,{vector_sql(q)}) AS d FROM t'+
                 (f' WHERE g={j%10}' if j%2 else '')+' ORDER BY d LIMIT 10;\n'
                 for j,q in enumerate(queries)]
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='mvdb-bench-') as data:
        p = subprocess.Popen([binary,'--data',data], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, bufsize=1)
        def send(sql):
            p.stdin.write(sql); p.stdin.flush()
        def line():
            v=p.stdout.readline()
            if not v: raise RuntimeError('database exited')
            if v.startswith('ERROR'): raise RuntimeError(v)
            return v.rstrip('\n')
        send(f'CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({args.dim}));\nSELECT id FROM t;\n')
        assert line() == 'id'
        begin=time.perf_counter()
        for batch in batches:
            send(batch); assert line() == 'id'
        write_s=time.perf_counter()-begin
        prep=time.perf_counter()
        if args.index:
            send('CREATE VECTOR INDEX idx_v ON t(v);\nSELECT id FROM t WHERE id=-1;\n')
            assert line() == 'id'
        index_prepare_s=time.perf_counter()-prep
        outputs=[]
        begin=time.perf_counter()
        for j,sql in enumerate(query_sql):
            send(sql); assert line() == 'id\td'
            size = args.rows if j%2==0 else len(range(j%10,args.rows,10))
            outputs.append([line() for _ in range(min(10,size))])
        query_s=time.perf_counter()-begin
        proc=Path('/proc')/str(p.pid)
        hwm=next(x for x in (proc/'status').read_text().splitlines() if x.startswith('VmHWM:'))
        io=(proc/'io').read_text()
        p.stdin.close(); assert p.wait(timeout=60)==0
        # Accuracy checks are deliberately outside timed region.
        recalls=[[],[]]
        try:
            import numpy as np
            dense = np.asarray(vectors, dtype=np.float64)
        except ImportError:
            np = None
        for j,(q,rows) in enumerate(zip(queries,outputs)):
            ids=list(range(args.rows)) if j%2==0 else list(range(j%10,args.rows,10))
            actual=[int(row.split('\t')[0]) for row in rows]
            assert len(actual)==len(set(actual)) and all(i in ids for i in actual)
            if np is not None:
                delta = dense[ids] - np.asarray(q,dtype=np.float64)
                distances = np.einsum('ij,ij->i',delta,delta)
                truth = [ids[i] for i in np.lexsort((np.asarray(ids),distances))[:10]]
            else:
                truth=sorted(ids,key=lambda i:(sum((a-b)**2 for a,b in zip(vectors[i],q)),i))[:10]
            recalls[j%2].append(len(set(truth)&set(actual))/len(truth) if truth else 1.)
            for row in rows:
                key,dist=row.split('\t'); key=int(key); dist=float(dist)
                ref=math.sqrt(sum((a-b)**2 for a,b in zip(vectors[key],q)))
                assert math.isclose(dist,ref,rel_tol=2e-6,abs_tol=2e-6),(dist,ref)
        # Verify acknowledged write durability after normal restart.
        p2=subprocess.run([binary,'--data',data],input='SELECT id FROM t ORDER BY id;\n',
                          text=True,capture_output=True,timeout=60)
        assert p2.returncode==0 and p2.stdout.splitlines()==['id']+list(map(str,range(args.rows)))
        result={'kind':'local_provisional_protocol_not_official_score','binary':binary,
                'seed':args.seed,'rows':args.rows,'queries':args.queries,'dim':args.dim,
                'index':args.index,'index_prepare_seconds':index_prepare_s,'write_seconds':write_s,'query_seconds':query_s,'qps':args.queries/query_s,
                'process_peak_rss':hwm,'process_io_before_close':io,
                'recall_unfiltered':sum(recalls[0])/len(recalls[0]),
                'recall_filtered':sum(recalls[1])/len(recalls[1]),'restart_contents':'pass'}
        text=json.dumps(result,indent=2)
        print(text)
        if args.output: Path(args.output).write_text(text+'\n')


if __name__=='__main__': main()
