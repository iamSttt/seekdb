#!/usr/bin/env python3
"""Does a primary-key lookup cost grow with row count? If yes, the 'protocol
floor' measured on a populated table is partly our own scan."""
import argparse, json, sys, tempfile, time
from pathlib import Path
from statistics import median
PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session
DIM = 128

def timed(s, sql, n):
    lat = []
    for _ in range(n):
        b = time.perf_counter(); r = s.request(sql); lat.append(time.perf_counter() - b)
        if not r.get("ok"): raise SystemExit("fail %r" % (r,))
    return 1e6 * median(lat)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--image"); ap.add_argument("--repeat", type=int, default=400)
    a = ap.parse_args()
    cfg = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.mkstemp(prefix="p-", suffix=".log")[1]
    s = Session(cfg, log, image=a.image)
    v = ",".join(["0.013"] * DIM)
    out = {}
    try:
        s.start()
        s.request("CREATE TABLE t(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIM)
        out["empty_missing_pk_us"] = timed(s, "SELECT id FROM t WHERE id=-1;", a.repeat)
        rows = []
        for i in range(10000):
            base = 0.5 + (i % 97) * (1.0 / 10000)
            rows.append("(%d,%d,'[%s]')" % (i + 100, i % 10, ",".join("%.4f" % (base + (j % 7) * 0.001) for j in range(DIM))))
        marks = [100, 1000, 10000]
        done = 0
        for mk in marks:
            while done < mk:
                s.request("INSERT INTO t VALUES " + ",".join(rows[done:done + 100]) + ";")
                done += 100
            out["rows_%d_missing_pk_us" % mk] = timed(s, "SELECT id FROM t WHERE id=-1;", a.repeat)
            out["rows_%d_hit_pk_us" % mk] = timed(s, "SELECT id FROM t WHERE id=500;", a.repeat)
    finally:
        try: s.shutdown()
        except Exception: pass
    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()
