#!/usr/bin/env python3
"""I17 controlled AAAA/ABAB/ABCABC and compound workloads, including one query."""
import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import heapq
import json
import os
from pathlib import Path
import platform
import random
from time import perf_counter
import benchmark_iteration as base

CASES=('AAAA','ABAB','ABCABC','compound','compound_reverse')

def make_workload(pattern,seed,queries,batch_size,rows=10000,dimension=128):
    # The existing lifecycle helper expects updated_vector, even without UPDATE.
    # Generate >=2 base queries, then replace every query/oracle/count field with
    # this workload's exact positive length. One-query runs never access index 1.
    work=base.make_workload((rows,dimension,0,0),seed,max(2,queries),batch_size,10)
    vectors=work['vectors']
    labels=[(i%2,(i//2)%1000,(i//3)%10) for i in range(rows)]
    work['create']=f'CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dimension}),a BIGINT,b BIGINT,c BIGINT);'
    literals=[base.vector_literal(v) for v in vectors]
    work['inserts']=[]
    for stop in range(rows,0,-batch_size):
        start=max(0,stop-batch_size)
        sql='INSERT INTO docs VALUES '+','.join(
            f'({i},{literals[i]},{labels[i][0]},{labels[i][1]},{labels[i][2]})'
            for i in range(stop-1,start-1,-1))+';'
        work['inserts'].append((sql,stop-start))
    rng=random.Random(seed ^ 0x174141)
    query_vectors=[array('f',vectors[0])]
    query_vectors.extend(array('f',(rng.gauss(0,1) for _ in range(dimension))) for _ in range(queries-1))
    sqls,counts,oracle,terms=[],[],{},[]
    for q,vector in enumerate(query_vectors):
        if pattern.startswith('compound'):
            a,b=q%2,q%1000
            selected=[('a',a),('b',b)]
            if pattern=='compound_reverse':selected.reverse()
            matches=lambda values,a=a,b=b:values[0]==a and values[1]==b
        else:
            column={'AAAA':0,'ABAB':q%2,'ABCABC':q%3}[pattern]
            value=(q//len(set(pattern)))%[2,1000,10][column]
            selected=[('abc'[column],value)]
            matches=lambda values,column=column,value=value:values[column]==value
        predicate=' AND '.join(f'{name}={value}' for name,value in selected)
        keys=[i for i,values in enumerate(labels) if matches(values)]
        sqls.append(f'SELECT id,L2_DISTANCE(v,{base.vector_literal(vector)}) AS d FROM docs WHERE {predicate} ORDER BY d LIMIT 10;')
        counts.append(min(10,len(keys)))
        # Every pattern request has an independent float32 exhaustive oracle.
        oracle[q]=heapq.nsmallest(10,((base.squared_distance(vectors[i],vector),i) for i in keys))
        terms.append(selected)
    work.update(queries=sqls,query_counts=counts,before_oracle=oracle,after_oracle=oracle,
                oracle_indices=list(range(queries)),pattern=pattern,query_terms=terms)
    digest=hashlib.sha256(work['create'].encode())
    for sql,_ in work['inserts']:digest.update(sql.encode())
    for sql in sqls:digest.update(sql.encode())
    work['sha256']=digest.hexdigest()
    return work

def stderr_with_counters(path,limit=4096):
    raw=path.read_bytes()
    prefix='MVDB_FILTER_DIAG '
    events=[json.loads(line[len(prefix):]) for line in raw.decode('utf-8','replace').splitlines()
            if line.startswith(prefix)]
    return {'total_bytes':len(raw),'tail_source_bytes':min(limit,len(raw)),
            'truncated':len(raw)>limit,'tail_utf8':raw[-limit:].decode('utf-8','replace'),
            'filter_diagnostic_events':events}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--scenario',action='append',choices=CASES)
    parser.add_argument('--queries',type=int,default=128)
    parser.add_argument('--pairs',type=int,default=5)
    parser.add_argument('--seeds',type=int,nargs='+',default=[7319,8911,13337])
    parser.add_argument('--rows',type=int,default=10000)
    parser.add_argument('--dimension',type=int,default=128)
    parser.add_argument('--batch-size',type=int,default=100)
    parser.add_argument('--timeout',type=float,default=120)
    parser.add_argument('--case-timeout',type=float,default=1800)
    parser.add_argument('--diagnostic',action='store_true',help='record counters; timings are not adoption evidence')
    args=parser.parse_args()
    if platform.system()!='Linux':parser.error('requires Linux transport and /proc')
    if min(args.queries,args.pairs,args.rows,args.dimension,args.batch_size)<1 or args.rows<2:
        parser.error('positive parameters and at least two rows required')
    if len(set(args.seeds))!=len(args.seeds):parser.error('duplicate seeds')
    if args.output.exists():parser.error('refusing to overwrite measurements')
    args.baseline,args.candidate=args.baseline.resolve(),args.candidate.resolve()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    if args.diagnostic:
        if os.environ.get('MVDB_QUERY_THREADS')!='1':
            parser.error('diagnostic counters require explicit MVDB_QUERY_THREADS=1')
        base.stderr_tail=stderr_with_counters
    report={'kind':'filter_pattern_paired_measurement','status':'running','official_score':None,
            'diagnostic':args.diagnostic,'created_utc':datetime.now(timezone.utc).isoformat(),
            'parameters':vars(args)|{'baseline':str(args.baseline),'candidate':str(args.candidate),'output':str(args.output)},
            'binaries':{side:hashlib.sha256(getattr(args,side).read_bytes()).hexdigest()
                        for side in ('baseline','candidate')},
            'environment':base.runtime_environment(),'pairs':[],
            'limits':['First-query cost stays in lifecycle and query totals.',
                      'AAAA uses 50%, B 0.1%, C 10% scalar hit rates; these are synthetic.',
                      'Exact/ann transport alternates; default ANN probing remains disabled.',
                      'Diagnostics add counters and must not determine performance adoption.',
                      'Shared host/page cache and one CI run do not prove general speedup.']}
    pair=None
    try:
        for case_index,name in enumerate(args.scenario or CASES):
            for seed_index,seed in enumerate(args.seeds):
                start=perf_counter();work=make_workload(name,seed,args.queries,args.batch_size,args.rows,args.dimension)
                generation=perf_counter()-start
                for index in range(args.pairs):
                    order=['baseline','candidate']
                    if (case_index+seed_index+index)%2:order.reverse()
                    pair={'scenario':name,'seed':seed,'pair_index':index,'order':order,
                          'workload_sha256':work['sha256'],'query_terms':work['query_terms'],
                          'generation_and_oracle_seconds_excluded':generation}
                    for side in order:
                        print(f'{name} seed={seed} pair={index+1}/{args.pairs} {side}',flush=True)
                        pair[side]=base.one_run(getattr(args,side),work,args)
                    base.require(pair['baseline']['response_sha256']==pair['candidate']['response_sha256'],'response mismatch')
                    pair['all_responses_identical']=True
                    report['pairs'].append(pair);report['summary']=base.paired_summary(report['pairs'])
                    base.write_report(args.output,report)
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=f'{type(error).__name__}: {error}',failed_pair=pair)
        base.write_report(args.output,report);raise
    report['completed_utc']=datetime.now(timezone.utc).isoformat()
    base.write_report(args.output,report)
if __name__=='__main__':main()
