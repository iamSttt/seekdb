#!/usr/bin/env python3
"""Isolated cgroup-v2 diagnostic resources and 1/2 CPU lifecycle comparison.

Run as root only in an ephemeral Linux CI runner with delegated controllers.
This is not the official dedicated-device/cache measurement environment.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import time
import uuid

import benchmark_iteration as base
import benchmark_report_matrix as matrix


def snapshot(group):
    result = {}
    for name in ('cpu.max', 'cpu.stat', 'memory.max', 'memory.swap.max', 'memory.peak', 'memory.events', 'io.stat'):
        try:
            result[name] = (group/name).read_text().strip()
        except OSError as error:
            result[name] = {'unavailable': str(error)}
    return result


@contextmanager
def group_context(parent, cpus):
    group = parent / ('case-'+uuid.uuid4().hex)
    group.mkdir()
    try:
        (group/'cpu.max').write_text(f'{cpus*100000} 100000')
        (group/'memory.max').write_text(str(2*1024**3))
        (group/'memory.swap.max').write_text('0')
        yield group
    finally:
        # Only the group this invocation created, after the Session has reaped
        # the database. Never recursively remove arbitrary cgroup paths.
        if (group/'cgroup.procs').read_text().strip():
            raise RuntimeError('database still running in measurement cgroup')
        group.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=3)
    parser.add_argument('--queries', type=int, default=256)
    parser.add_argument('--seeds', type=int, nargs='+', default=[7319,8911,13337])
    parser.add_argument('--cpus', type=int, nargs='+', choices=[1,2], default=[1,2])
    parser.add_argument('--scenario', action='append', choices=['standard10k','spill','high768_k100','compound'], default=None)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--case-timeout', type=float, default=1800)
    args=parser.parse_args()
    if args.output.exists():parser.error('refusing to overwrite report')
    if args.pairs<1 or args.queries<2 or len(set(args.seeds))!=len(args.seeds):parser.error('invalid repetitions')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    report={'status':'running','pairs':[], 'official_score':None,
        'scope':'isolated cgroup-v2 CPU/memory/io diagnostics; not official M/B or score',
        'limitations':['No dedicated data device, cold-cache or IO bandwidth parity with official evaluation.',
            'Memory includes page cache and a tiny static exec launcher, but excludes workload driver.',
            '2GiB memory/swap-disabled setting is an explicit diagnostic limit, not an assertion about frozen contest limits.',
            'Lifecycle includes restart verification and its resources; phase times exclude verification wall time.',
            'IO is collected after exit and a bounded stability wait, not proof of official writeback attribution.'],
        'binaries':{name:hashlib.sha256(getattr(args,name).read_bytes()).hexdigest() for name in ['baseline','candidate']}}
    base.write_report(args.output,report)
    parent=Path('/sys/fs/cgroup')/('minivec-report-'+uuid.uuid4().hex)
    try:
        parent.mkdir()
        (parent/'cgroup.subtree_control').write_text('+cpu +memory +io')
    except OSError as error:
        report.update(status='resource_collection_unavailable',error=str(error))
        base.write_report(args.output,report)
        if parent.exists():parent.rmdir()
        raise SystemExit(2)
    pair=None
    try:
        for name in args.scenario or ['standard10k','spill','high768_k100','compound']:
            for seed_index,seed in enumerate(args.seeds):
                work=(base.make_workload(base.FULL_CASES[name],seed,args.queries,100,10)
                      if name in base.FULL_CASES else matrix.make_workload(matrix.CASES[name],seed,args.queries,100))
                for cpus in args.cpus:
                    for index in range(args.pairs):
                        order=['baseline','candidate']
                        if (index+seed_index+cpus)%2:order.reverse()
                        pair={'scenario':name+f'-cpu{cpus}','seed':seed,'pair_index':index,'order':order,'cpu_quota':cpus}
                        for side in order:
                            print(f'{name} cpu={cpus} seed={seed} pair={index} {side}',flush=True)
                            with group_context(parent,cpus) as group:
                                args.command_prefix=[str(args.launcher.resolve()),str(group)]
                                before=snapshot(group)
                                run=base.one_run(getattr(args,side).resolve(),work,args)
                                prior=None;stable=0
                                for _ in range(20):
                                    current=snapshot(group)
                                    if current['io.stat']==prior:stable+=1
                                    else:stable=0
                                    if stable>=3:break
                                    prior=current['io.stat'];time.sleep(0.1)
                                run['cgroup_before']=before;run['cgroup_after_shutdown']=current
                                run['io_stable_samples']=stable
                                pair[side]=run
                        base.require(pair['baseline']['response_sha256']==pair['candidate']['response_sha256'],'response mismatch')
                        pair['all_responses_identical']=True
                        report['pairs'].append(pair);report['summary']=base.paired_summary(report['pairs'])
                        base.write_report(args.output,report)
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=f'{type(error).__name__}: {error}',failed_pair=pair)
        base.write_report(args.output,report)
        raise
    finally:
        parent.rmdir()
    base.write_report(args.output,report)


if __name__=='__main__':main()
