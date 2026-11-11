#!/usr/bin/env python3
"""Large resident TopK versus an independent exact float32 Fraction oracle."""
import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import signal
import struct
import sys
import tempfile
from typing import NamedTuple

PROTOCOL=Path(__file__).resolve().parents[2]/"protocol_check"
sys.path.insert(0,str(PROTOCOL))
from _transport import Session

DIMENSION=16
ROWS=8193
LATE_KEY=-(1<<53)-7

class Vector(NamedTuple):
    sql:str
    scaled:tuple

def require(ok,message):
    if not ok:
        raise AssertionError(message)

def vector(values=()):
    values=tuple(struct.unpack("!f",struct.pack("!f",v))[0] for v in values)
    require(len(values)<=DIMENSION and all(map(math.isfinite,values)),"finite float32 fixture")
    values+=(0.0,)*(DIMENSION-len(values))
    scaled=tuple((n<<149)//d for n,d in map(float.as_integer_ratio,values))
    return Vector("'["+",".join(map(repr,values))+"]'",scaled)

def squared(a,b):
    # No production floating accumulation, bound or quantizer is reused.
    return float(Fraction(sum((x-y)**2 for x,y in zip(a.scaled,b.scaled)),1<<298))

def execute(session,sql,*,affected=None,mode="exact"):
    response=session.request(sql,mode=mode)
    require(response.get("ok") is True,f"{sql[:160]}: {response!r}")
    if affected is not None:
        require(type(response.get("affected_rows")) is int and response["affected_rows"]==affected,
                f"wrong affected_rows: {response!r}")
    return response

def topk(session,records,q,k,*,group=None,descending=False,mode="exact"):
    where="" if group is None else f" WHERE g={group}"
    direction="DESC" if descending else "ASC"
    response=execute(session,f"SELECT id FROM parallel_rows{where} ORDER BY L2_DISTANCE(v,{q.sql}) {direction} LIMIT {k};",mode=mode)
    require(response.get("columns")==["id"],"typed PK result column")
    actual=response.get("rows")
    require(type(actual) is list and all(type(r) is list and len(r)==1 and type(r[0]) is int for r in actual),
            f"typed PK result rows: {response!r}")
    sign=-1 if descending else 1
    ranked=sorted((sign*squared(v,q),key) for key,(g,v) in records.items() if group is None or g==group)
    expected=[[key] for _,key in ranked[:k]]
    require(actual==expected,f"k={k} group={group} {direction}: got {actual}, expected {expected}")
    return actual

def checks(session):
    same=vector((3,4));zero=vector();records={}
    for i in range(ROWS-4):
        # Most rows tie, including late smaller PKs beyond both shard warmups.
        v=same if i<7160 else vector((16+i%17,(i%13)-6,(i%7)*0.25))
        records[100000+i]=(i%2,v)
    for key,group in [(LATE_KEY+2,1),(-99,0),(-11,1),(LATE_KEY,0)]:
        records[key]=(group,same)
    require(len(records)==ROWS,"unique fixture keys")
    execute(session,f"CREATE TABLE parallel_rows(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({DIMENSION}));")
    execute(session,"BEGIN;")
    entries=list(records.items())
    for start in range(0,len(entries),128):
        batch=entries[start:start+128]
        execute(session,"INSERT INTO parallel_rows VALUES "+",".join(f"({key},{g},{v.sql})" for key,(g,v) in batch)+";",affected=len(batch))
    execute(session,"COMMIT;")
    topk(session,records,zero,1) # Unindexed serial fallback at the same row count.
    execute(session,"CREATE VECTOR INDEX parallel_v ON parallel_rows(v);")
    for k in (0,1,10,64,65):
        topk(session,records,zero,k)
    for i in range(4):
        topk(session,records,vector((1,-2,0.5)),10,mode="ann" if i%2 else "exact")
    topk(session,records,vector((30,7,-1,0.25)),10)
    topk(session,records,zero,10,group=0)
    topk(session,records,zero,10,descending=True)

    # A transaction update spills payloads; its rebuilt resident index owns
    # vectors. Distances straddle a binary64 midpoint by one float32 square.
    h=2.0**30;tiny=2.0**-149
    near=vector((0,0,0,tiny));early=vector((h,16));late=vector((h,16,8,8))
    farther=vector((h,16,8,0,8))
    require(squared(early,near)==squared(late,near)==2.0**60+256,"canonical tie fixture")
    require(squared(farther,near)==math.nextafter(2.0**60+256,math.inf),"adjacent canonical distance")
    execute(session,"BEGIN;")
    execute(session,f"UPDATE parallel_rows SET v={farther.sql};",affected=ROWS)
    execute(session,f"UPDATE parallel_rows SET v={early.sql} WHERE id=100000;",affected=1)
    execute(session,f"UPDATE parallel_rows SET v={late.sql} WHERE id={LATE_KEY};",affected=1)
    changed={key:(g,farther) for key,(g,_) in records.items()}
    changed[100000]=(records[100000][0],early);changed[LATE_KEY]=(records[LATE_KEY][0],late)
    require(topk(session,changed,near,1)==[[LATE_KEY]],"late smaller PK survives canonical tie")
    topk(session,changed,near,10)
    # Extreme finite endpoints overflow float subtraction/squares; both shards
    # must use the complete double/exact fallback rather than drop candidates.
    maximum=struct.unpack("!f",struct.pack("!I",0x7f7fffff))[0]
    extreme=vector((maximum,-maximum,tiny,-tiny))
    opposite=vector((-maximum,maximum,-tiny,tiny))
    execute(session,f"UPDATE parallel_rows SET v={extreme.sql} WHERE id=100001;",affected=1)
    execute(session,f"UPDATE parallel_rows SET v={opposite.sql} WHERE id={LATE_KEY+2};",affected=1)
    changed[100001]=(records[100001][0],extreme);changed[LATE_KEY+2]=(records[LATE_KEY+2][0],opposite)
    topk(session,changed,extreme,1,mode="ann")
    topk(session,changed,opposite,10)
    execute(session,"ROLLBACK;")
    topk(session,records,zero,10) # Fresh view after rollback, payloads remain spilled.
    topk(session,records,zero,10,group=1)
    session.shutdown();session.start()
    topk(session,records,zero,1)
    # The idle helper must not consume TERM while main waits in read(2).
    # Exit must still close cleanly, and the next process must recover commits.
    session.proc.send_signal(signal.SIGTERM)
    require(session.proc.wait(timeout=10)==0,"idle SIGTERM must exit promptly and cleanly")
    require(not session.buffer and not session.proc.stdout.read(session.config["max_response_bytes"]),
            "termination must not emit unsolicited JSON")
    session.stop();session.start()
    topk(session,records,zero,64)
    session.shutdown()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    args=parser.parse_args()
    config=json.loads((PROTOCOL/"config.json").read_text(encoding="utf-8"))
    command=[str(args.binary.resolve()),"--data","{data_dir}","--protocol","jsonl"]
    with tempfile.TemporaryDirectory(prefix="mvdb-parallel-topk-") as scratch:
        session=Session(config,Path(scratch)/"stderr.log",command=command)
        try:
            session.start();checks(session)
        finally:
            session.close()
    print("PASS 8193-row JSONL TopK exact Fraction oracle, late PK ties, midpoint/extremes, serial fallbacks, spill/rollback/restart")

if __name__=="__main__":
    main()
