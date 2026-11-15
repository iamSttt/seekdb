#!/usr/bin/env python3
"""Isolate the ingest cost: same number of commits, different rows per commit."""
import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session

DIM = 128


def row(i, vec):
    return "(" + str(i) + "," + str(i % 10) + ",'[" + vec + "]')"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image")
    ap.add_argument("--commits", type=int, default=100)
    args = ap.parse_args()
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    log = tempfile.mkstemp(prefix="probe-ingest-", suffix=".log")[1]
    session = Session(config, log, image=args.image)
    vec = ",".join(["0.013"] * DIM)
    out = {}
    try:
        session.start()
        # A: 1 row per commit -> pure per-commit cost (fsync-dominated if flat)
        session.request("CREATE TABLE a(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIM)
        begin = time.perf_counter()
        for i in range(args.commits):
            session.request("BEGIN;")
            session.request("INSERT INTO a VALUES " + row(i, vec) + ";")
            session.request("COMMIT;")
        out["A_one_row_per_commit_seconds"] = time.perf_counter() - begin
        # B: 100 rows per commit, same commit count -> same fsyncs, 100x the serialize work
        session.request("CREATE TABLE b(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIM)
        begin = time.perf_counter()
        for c in range(args.commits):
            session.request("BEGIN;")
            session.request("INSERT INTO b VALUES " + ",".join(row(c * 100 + k, vec) for k in range(100)) + ";")
            session.request("COMMIT;")
        out["B_hundred_rows_per_commit_seconds"] = time.perf_counter() - begin
        # C: one single statement, 10000 rows, one commit -> serialize without fsync per row
        session.request("CREATE TABLE c(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d));" % DIM)
        begin = time.perf_counter()
        session.request("BEGIN;")
        session.request("INSERT INTO c VALUES " + ",".join(row(k, vec) for k in range(100 * args.commits)) + ";")
        session.request("COMMIT;")
        out["C_single_statement_10000_rows_seconds"] = time.perf_counter() - begin
        out["commits"] = args.commits
        out["per_commit_A_ms"] = 1000 * out["A_one_row_per_commit_seconds"] / args.commits
        out["per_commit_B_ms"] = 1000 * out["B_hundred_rows_per_commit_seconds"] / args.commits
    finally:
        try:
            session.shutdown()
        except Exception:
            pass
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
