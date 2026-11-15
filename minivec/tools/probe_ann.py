#!/usr/bin/env python3
"""Compare exact vs ann search mode on the scored workload shape."""
import argparse, json, sys, tempfile, time
from pathlib import Path
from statistics import median
PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session

DIM, ROWS = 128, 10000


def timed(session, sql, mode, repeat):
    lat = []
    for _ in range(repeat):
        b = time.perf_counter()
        r = session.request(sql, mode=mode)
        lat.append(time.perf_counter() - b)
        if not r.get("ok"):
            raise SystemExit("fail %r" % (r,))
    return 1e6 * sum(lat) / len(lat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image")
    ap.add_argument("--repeat", type=int, default=500)
    a = ap.parse_args()
    cfg = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.mkstemp(prefix="p-", suffix=".log")[1]
    s = Session(cfg, log, image=a.image)
    vec = ",".join(["0.013"] * DIM)
    out = {}
    try:
        s.start()
        s.request("CREATE TABLE t(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIM)
        rows = []
        for i in range(ROWS):
            base = 0.5 + (i % 97) * (1.0 / ROWS)
            rows.append("(%d,%d,'[%s]')" % (i + 100, i % 10, ",".join("%.4f" % (base + (j % 7) * 0.001) for j in range(DIM))))
        for st in range(0, ROWS, 100):
            s.request("INSERT INTO t VALUES " + ",".join(rows[st:st + 100]) + ";")
        s.request("CREATE VECTOR INDEX t_v ON t(v);")
        unf = "SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t ORDER BY d LIMIT 10;" % vec
        filt = "SELECT id,L2_DISTANCE(v,'[%s]') AS d FROM t WHERE grp=3 ORDER BY d LIMIT 10;" % vec
        for mode in ("exact", "ann"):
            timed(s, unf, mode, 20)
            out[mode + "_unfiltered_us"] = timed(s, unf, mode, a.repeat)
            out[mode + "_filtered_us"] = timed(s, filt, mode, a.repeat)
    finally:
        try: s.shutdown()
        except Exception: pass
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
