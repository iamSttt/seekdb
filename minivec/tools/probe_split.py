#!/usr/bin/env python3
"""Split one end-to-end query round trip, and test whether the vector index
actually accelerates the scan (same table, before vs after CREATE VECTOR INDEX).
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


def stage(name, lat):
    total = sum(lat)
    return {"name": name, "count": len(lat), "total_seconds": total,
            "us_per_request": 1e6 * total / len(lat), "p50_us": 1e6 * median(lat)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image")
    ap.add_argument("--repeat", type=int, default=1000)
    args = ap.parse_args()
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.mkstemp(prefix="probe-split-", suffix=".stderr.log")[1]
    session = Session(config, log, image=args.image)
    vec = ",".join(["0.013"] * DIMENSION)
    topk = "SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t ORDER BY d LIMIT 10;" % vec
    filtered = "SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t WHERE grp=3 ORDER BY d LIMIT 10;" % vec
    out = []
    try:
        session.start()
        session.request("CREATE TABLE t(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIMENSION)
        vecs = []
        for i in range(ROWS):
            base = 0.5 + (i % 97) * (1.0 / ROWS)
            vecs.append("(" + str(i + 100) + "," + str(i % 10) + ",'[" +
                        ",".join("%.4f" % (base + (j % 7) * 0.001) for j in range(DIMENSION)) + "]')")
        for start in range(0, ROWS, 100):
            session.request("INSERT INTO t VALUES " + ",".join(vecs[start:start + 100]) + ";")
        timed(session, topk, 30)
        out.append(stage("A_unfiltered_no_index", timed(session, topk, args.repeat)))
        out.append(stage("B_filtered_no_index", timed(session, filtered, args.repeat)))
        begin = time.perf_counter()
        session.request("CREATE VECTOR INDEX t_v ON t(v);")
        print(json.dumps({"index_build_seconds": time.perf_counter() - begin}))
        timed(session, topk, 30)
        out.append(stage("C_unfiltered_with_index", timed(session, topk, args.repeat)))
        out.append(stage("D_filtered_with_index", timed(session, filtered, args.repeat)))
        out.append(stage("E_trivial", timed(session, "SELECT id FROM t WHERE id=-1;", args.repeat)))
        out.append(stage("F_point_lookup", timed(session, "SELECT grp FROM t WHERE id=500;", args.repeat)))
    finally:
        try:
            session.shutdown()
        except Exception:
            pass
    print(json.dumps({"dimension": DIMENSION, "rows": ROWS, "stages": out}, indent=2))


if __name__ == "__main__":
    main()
