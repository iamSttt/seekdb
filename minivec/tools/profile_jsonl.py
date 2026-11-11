#!/usr/bin/env python3
"""Bounded diagnostic of official-driver round trips and durable batch costs."""
import argparse
from array import array
import json
from pathlib import Path
import random
import signal
import tempfile
from time import perf_counter

from benchmark_jsonl import PROTOCOL_CHECK, Session, summarize, vector_literal


def profile(args):
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    rng = random.Random(args.seed)
    # Identical binary32 payloads and key order for every write variant.
    vectors = [array("f", (rng.gauss(0, 1) for _ in range(128)))
               for _ in range(max(args.write_rows, args.query_rows))]
    values = [f"({i},{i % 10},{vector_literal(v)})" for i, v in enumerate(vectors)]
    command = None if args.image else [str(args.binary.resolve()), "--data", "{data_dir}", "--protocol", "jsonl"]
    deadline = perf_counter() + args.timeout
    with tempfile.TemporaryDirectory(prefix="minivec-path-profile-") as scratch:
        session = Session(config, Path(scratch) / "stderr.log", image=args.image,
                          command=command, case_timeout=args.timeout)

        def request(sql, affected=None):
            if perf_counter() > deadline:
                raise TimeoutError("diagnostic wall deadline exceeded")
            started = perf_counter()
            result = session.request(sql)
            elapsed = perf_counter() - started
            if result.get("ok") is not True:
                raise ValueError(f"request failed: {result.get('error')!r}")
            if affected is not None and result.get("affected_rows") != affected:
                raise ValueError("incorrect affected_rows")
            return elapsed, result

        def create(name):
            request(f"CREATE TABLE {name}(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(128));")

        def ingest(name, statement_rows, commit_rows, count):
            create(name)
            # Preformat outside the measured requests in every variant.
            batches = []
            for start in range(0, count, commit_rows):
                end = min(count, start + commit_rows)
                batches.append([(f"INSERT INTO {name} VALUES " +
                                 ",".join(values[pos:min(pos + statement_rows, end)]) + ";",
                                 min(statement_rows, end - pos))
                                for pos in range(start, end, statement_rows)])
            phases = {"begin": [], "insert": [], "commit": []}
            for batch in batches:
                phases["begin"].append(request("BEGIN;")[0])
                for sql, rows in batch:
                    phases["insert"].append(request(sql, rows)[0])
                phases["commit"].append(request("COMMIT;")[0])
            # Correctness scan deliberately outside the timing.
            _, result = request(f"SELECT id FROM {name} ORDER BY id;")
            if result.get("rows") != [[i] for i in range(count)]:
                raise ValueError("write variant lost or reordered records")
            return {"rows": count, "statement_rows": statement_rows,
                    "commit_rows": commit_rows, "phase": {k: summarize(v) for k, v in phases.items()},
                    "request_seconds": sum(sum(v) for v in phases.values())}

        try:
            session.start()
            variants = [("single_statement_single_commit", 1, 1),
                        ("single_statement_batch_commit", 1, 100),
                        ("batch_statement_batch_commit", 100, 100)]
            # Seed rotation changes position; equal-length table names avoid
            # introducing a SQL/WAL size difference between write variants.
            offset = args.seed % len(variants)
            variants = variants[offset:] + variants[:offset]
            writes = {}
            for position, (name, statement_rows, commit_rows) in enumerate(variants):
                writes[name] = ingest(f"w{position}", statement_rows, commit_rows, args.write_rows)
            ingest("query_docs", 100, 100, args.query_rows)
            index_seconds = request("CREATE VECTOR INDEX query_v ON query_docs(v);")[0]
            query_sql = []
            for i in range(args.requests):
                q = vector_literal(array("f", (rng.gauss(0, 1) for _ in range(128))))
                base = f"SELECT id,L2_DISTANCE(v,{q}) AS d FROM query_docs"
                query_sql.append({
                    "empty_result": "SELECT id FROM query_docs LIMIT 0;",
                    "primary_key": f"SELECT id FROM query_docs WHERE id={i % args.query_rows};",
                    "vector_unfiltered": base + " ORDER BY d LIMIT 10;",
                    "vector_filtered": base + f" WHERE grp={i % 10} ORDER BY d LIMIT 10;"})
            timings = {key: [] for key in query_sql[0]}
            response_bytes = {key: 0 for key in timings}
            keys = list(timings)
            for i, queries in enumerate(query_sql):
                order = keys[i % len(keys):] + keys[:i % len(keys)]
                for key in order:
                    elapsed, result = request(queries[key])
                    timings[key].append(elapsed)
                    # Response serialization here is outside request timing.
                    response_bytes[key] += len(json.dumps(result, separators=(",", ":")))
                    rows = result.get("rows")
                    if key == "empty_result" and rows != []:
                        raise ValueError("LIMIT 0 returned rows")
                    if key == "primary_key" and rows != [[i % args.query_rows]]:
                        raise ValueError("PK lookup mismatch")
                    if key.startswith("vector_") and len(rows) != 10:
                        raise ValueError("vector query row count mismatch")
            session.shutdown()
        finally:
            session.close()
    return {"kind": "local_jsonl_path_diagnostic", "seed": args.seed,
            "write_order": [v[0] for v in variants], "write": writes,
            "query_rows": args.query_rows, "query_index_seconds": index_seconds,
            "query": {key: dict(summarize(samples), response_json_characters=response_bytes[key])
                      for key, samples in timings.items()},
            "scope": "One container and official driver; interleaved request classes. Different SQL and responses are not additive component timings. Single-row commits intentionally change durability frequency; compare single_statement_batch_commit with batch_statement_batch_commit to hold commit count fixed. No formal score or exhaustive query oracle."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--image")
    target.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=7319)
    parser.add_argument("--write-rows", type=int, default=200)
    parser.add_argument("--query-rows", type=int, default=10000)
    parser.add_argument("--requests", type=int, default=200, help="per query class")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not (1 <= args.write_rows <= 10000 and 100 <= args.query_rows <= 10000 and
            1 <= args.requests <= 2000 and 1 <= args.timeout <= 600):
        parser.error("rows/requests/timeout outside diagnostic bounds")
    # Session also enforces the official per-request timeout. POSIX alarm bounds
    # total work; finally still closes the owned session/container on expiry.
    def expired(signum, frame):
        raise TimeoutError("diagnostic wall timeout")
    if hasattr(signal, "SIGALRM"):
        signal.signal(signal.SIGALRM, expired)
        signal.alarm(args.timeout)
    try:
        result = profile(args)
    finally:
        if hasattr(signal, "SIGALRM"):
            signal.alarm(0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2, allow_nan=False)
        output.write("\n")
    print(json.dumps(result, allow_nan=False))


if __name__ == "__main__":
    main()
