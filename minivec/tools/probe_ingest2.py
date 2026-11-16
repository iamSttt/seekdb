#!/usr/bin/env python3
"""Where does ingest time go? Same row count and commit count, varying payload."""
import argparse, json, sys, tempfile, time
from pathlib import Path
PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session
ROWS, BATCH = 10000, 100

def lit(n):
    return None if n == 0 else "[" + ",".join(["0.013"] * n) + "]"

def ingest(s, table, cols, dim):
    s.request("CREATE TABLE %s(%s);" % (table, cols))
    rows = []
    for i in range(ROWS):
        parts = [str(i + 1), str(i % 10)]
        if dim: parts.append("'" + lit(dim) + "'")
        rows.append("(" + ",".join(parts) + ")")
    begin = time.perf_counter()
    for b in range(0, ROWS, BATCH):
        s.request("BEGIN;")
        s.request("INSERT INTO %s VALUES %s;" % (table, ",".join(rows[b:b + BATCH])))
        s.request("COMMIT;")
    return time.perf_counter() - begin

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--image"); a = ap.parse_args()
    cfg = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.mkstemp(prefix="p-", suffix=".log")[1]
    s = Session(cfg, log, image=a.image)
    out = {}
    try:
        s.start()
        out["scalar_only_s"] = ingest(s, "a", "id BIGINT PRIMARY KEY, grp BIGINT", 0)
        out["vector8_s"] = ingest(s, "b", "id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(8)", 8)
        out["vector128_s"] = ingest(s, "c", "id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(128)", 128)
    finally:
        try: s.shutdown()
        except Exception: pass
    for k in list(out):
        if k.endswith("_s"): out[k.replace("_s", "_us_per_row")] = 1e6 * out[k] / ROWS
    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()
