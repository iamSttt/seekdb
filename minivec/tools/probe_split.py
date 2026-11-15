#!/usr/bin/env python3
"""Split one end-to-end query round trip into protocol, parse and scan parts.

Measures, through the official transport, the same request shape with three
different amounts of work behind it:
  A  trivial predicate, no vector        -> transport + minimal SQL
  B  the identical TopK SQL on 1 row     -> transport + SQL parse of a 128-float literal
  C  the identical TopK SQL on N rows    -> B + scan of N rows
C - B is the scan cost; B - A is the parse cost; A is the transport floor.
"""
import argparse
import json
import sys
import tempfile
import time
from pathlib import Path
from statistics import median

PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session

DIMENSION = 128
ROWS = 10000


def timed(session, sql, repeat):
    lat = []
    for _ in range(repeat):
        begin = time.perf_counter()
        response = session.request(sql)
        lat.append(time.perf_counter() - begin)
        if not response.get("ok"):
            raise SystemExit("request failed: %r" % (response,))
    return lat


def report(name, lat):
    total = sum(lat)
    return {"name": name, "count": len(lat), "total_seconds": total,
            "us_per_request": 1e6 * total / len(lat), "p50_us": 1e6 * median(lat)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image")
    ap.add_argument("--command")
    ap.add_argument("--repeat", type=int, default=1000)
    args = ap.parse_args()
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.NamedTemporaryFile(prefix="probe-split-", suffix=".stderr.log", delete=False)
    command = json.loads(args.command) if args.command else None
    session = Session(config, log, image=args.image, command=command)
    results = []
    try:
        session.start()
        vec = ",".join(["0.013"] * DIMENSION)
        session.request("CREATE TABLE t(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIMENSION)
        session.request("INSERT INTO t VALUES (1,0,'[%s]');" % vec)
        trivial = "SELECT id FROM t WHERE id=-1;"
        topk = ("SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t ORDER BY d LIMIT 10;" % vec)
        # warm up so page faults and first-parse effects do not land in the sample
        timed(session, trivial, 50)
        timed(session, topk, 50)
        results.append(report("A_transport_plus_trivial_sql", timed(session, trivial, args.repeat)))
        results.append(report("B_same_plus_128float_parse", timed(session, topk, args.repeat)))

        vecs = []
        step = 1.0 / ROWS
        for i in range(ROWS):
            base = 0.5 + (i % 97) * step
            vecs.append("(" + str(i + 100) + "," + str(i % 10) + ",'[" +
                        ",".join("%.4f" % (base + (j % 7) * 0.001) for j in range(DIMENSION)) + "]')")
        for start in range(0, ROWS, 100):
            session.request("INSERT INTO t VALUES " + ",".join(vecs[start:start + 100]) + ";")
        session.request("CREATE VECTOR INDEX t_v ON t(v);")
        timed(session, topk, 20)
        results.append(report("C_same_plus_%d_row_scan" % ROWS, timed(session, topk, args.repeat)))
        filtered = ("SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t WHERE grp=3 ORDER BY d LIMIT 10;" % vec)
        timed(session, filtered, 20)
        results.append(report("D_filtered_scan", timed(session, filtered, args.repeat)))
    finally:
        try:
            session.shutdown()
        except Exception:
            pass
    print(json.dumps({"dimension": DIMENSION, "rows": ROWS, "stages": results}, indent=2))


if __name__ == "__main__":
    main()
