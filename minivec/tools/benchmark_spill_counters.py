#!/usr/bin/env python3
"""I07 logical scan/read diagnostics, explicitly excluded from timing adoption."""
import argparse,hashlib,json,os,platform
from pathlib import Path
import benchmark_iteration as base
import benchmark_report_matrix as matrix
from benchmark_filter_patterns import stderr_with_counters

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--queries',type=int,default=8)
    parser.add_argument('--seed',type=int,default=7319)
    parser.add_argument('--timeout',type=float,default=120)
    parser.add_argument('--case-timeout',type=float,default=1800)
    args=parser.parse_args()
    if platform.system()!='Linux':parser.error('requires Linux transport and /proc')
    if os.environ.get('MVDB_QUERY_THREADS')!='1':parser.error('MVDB_QUERY_THREADS=1 required')
    if args.queries<2:parser.error('at least two diagnostic queries required')
    if args.output.exists():parser.error('refusing to overwrite results')
    args.baseline,args.candidate=args.baseline.resolve(),args.candidate.resolve()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    base.stderr_tail=stderr_with_counters
    report={'kind':'spill_filter_logical_io_diagnostics','status':'running','official_score':None,
      'not_performance_evidence':True,'pairs':[],
      'binaries':{side:hashlib.sha256(getattr(args,side).read_bytes()).hexdigest()
                   for side in ('baseline','candidate')},
      'limits':['pread returned bytes are userspace transfer, not actual storage-device IO.',
                'The default 32MiB spill threshold is unchanged; 5000 rows x8KiB plus vectors spills.',
                'CREATE_INDEX is counted; index reconstruction during process open is outside statement scope.',
                'Process /proc read_bytes snapshots remain separate proxies for actual device IO.',
                'Counters add overhead; use uninstrumented paired runs for performance decisions.']}
    checks=[]
    for name in ('spill_compound','spill_high'):
        work=matrix.make_workload(matrix.CASES[name],args.seed,args.queries,100)
        pair={'scenario':name,'seed':args.seed,'pair_index':0,'workload_sha256':work['sha256']}
        for side in ('baseline','candidate'):
            pair[side]=base.one_run(getattr(args,side),work,args)
            events=pair[side]['stderr']['filter_diagnostic_events']
            creates=[e for e in events if e['statement_kind']==10]
            scans=[e for e in events if e['scan_calls']]
            assert len(creates)==1 and creates[0]['row_fetches']>=5000,'CREATE INDEX not captured'
            assert len(scans)==args.queries*2,'missing query scan counters'
            expected=5000*(128*4+16)
            for event in scans:
                assert event['vector_records']==5000,'vector scan unexpectedly omitted rows'
                assert event['vector_read_requested_bytes']==expected,'wrong logical index scan bytes'
                assert event['vector_pread_returned_bytes']==expected,'wrong actual returned index bytes'
                assert event['payload_bytes']==event['payload_pread_returned_bytes'],'payload read accounting mismatch'
            checks.append({'scenario':name,'side':side,'create_index':creates[0],
                           'per_query_counters':scans,'expected_vector_index_bytes_per_query':expected})
        assert pair['baseline']['response_sha256']==pair['candidate']['response_sha256']
        pair['all_responses_identical']=True;report['pairs'].append(pair)
        report['checks']=checks;base.write_report(args.output,report)
    report['status']='passed';base.write_report(args.output,report)
if __name__=='__main__':main()
