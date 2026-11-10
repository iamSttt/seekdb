#!/usr/bin/env python3
"""Measure a reproducible synthetic workload through the official JSONL transport."""

import argparse
from array import array
from collections import Counter
import heapq
import json
import math
from operator import sub
from pathlib import Path
import random
from statistics import median
import sys
import tempfile
from time import perf_counter


PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session


DIMENSION = 128
TOP_K = 10
GROUPS = 10
ORACLE_QUERIES = 10
TOLERANCE = 1e-5


def require(condition, message):
    if not condition:
        raise ValueError(message)


def vector_literal(vector):
    return "'[" + ",".join(map(str, vector)) + "]'"


def make_workload(rows, query_count, seed):
    rng = random.Random(seed)

    def random_vector():
        return array("f", (rng.gauss(0.0, 1.0) for _ in range(DIMENSION)))

    require(array("f").itemsize == 4, "this workload requires IEEE float32 storage")
    vectors = [random_vector() for _ in range(rows)]
    groups = [key % GROUPS for key in range(rows)]
    # The first two queries must see IDs 0 and 1 tied at zero distance, even
    # though the table is populated in descending primary-key order.
    vectors[1] = vectors[0]
    groups[1] = groups[0]

    queries = []
    for index in range(query_count):
        group = (index // 2) % GROUPS if index % 2 else None
        vector = vectors[0] if index < 2 else random_vector()
        where = "" if group is None else f" WHERE grp={group}"
        sql = (f"SELECT id,L2_DISTANCE(embedding,{vector_literal(vector)}) AS d FROM docs"
               f"{where} ORDER BY d LIMIT {TOP_K};")
        queries.append((group, vector, sql))
    oracle_indices = {0, 1}
    for parity in (0, 1):
        oracle_indices.update(rng.sample(range(2 + parity, query_count, 2), ORACLE_QUERIES // 2 - 1))
    return vectors, groups, queries, oracle_indices


def execute(session, sql, context, *, affected=None):
    response = session.request(sql)
    require(response["ok"] is True, f"{context}: {response.get('error')!r}")
    if affected is not None:
        actual = response.get("affected_rows")
        require(type(actual) is int and actual == affected,
                f"{context}: affected_rows {actual!r}, expected {affected}")
    return response


def ingest(session, vectors, groups, batch_size):
    execute(session, f"CREATE TABLE docs(id BIGINT PRIMARY KEY, grp BIGINT, "
            f"embedding VECTOR({DIMENSION}));", "create table")
    elapsed = 0.0
    for stop in range(len(vectors), 0, -batch_size):
        start = max(0, stop - batch_size)
        values = ",".join(f"({key},{groups[key]},{vector_literal(vectors[key])})"
                          for key in range(stop - 1, start - 1, -1))
        sql = f"INSERT INTO docs VALUES {values};"
        begin = perf_counter()
        execute(session, "BEGIN;", "begin batch")
        execute(session, sql, f"insert IDs [{start},{stop})",
                affected=stop - start)
        execute(session, "COMMIT;", "commit batch")
        elapsed += perf_counter() - begin
    return elapsed


def squared_distance(a, b):
    return math.fsum(delta * delta for delta in map(sub, a, b))


def close_number(actual, expected):
    return (type(actual) in (int, float) and math.isfinite(actual)
            and math.isclose(actual, expected, rel_tol=TOLERANCE, abs_tol=TOLERANCE))


def check_query(response, vectors, groups, group, query, count, label):
    require(response["ok"] is True, f"{label}: {response.get('error')!r}")
    require(response.get("columns") == ["id", "d"], f"{label}: wrong columns")
    rows = response.get("rows")
    require(type(rows) is list and len(rows) == count, f"{label}: expected {count} rows, got {rows!r}")
    ranked = []
    seen = set()
    for row in rows:
        require(type(row) is list and len(row) == 2, f"{label}: invalid row {row!r}")
        key, distance = row
        require(type(key) is int and 0 <= key < len(vectors) and key not in seen,
                f"{label}: invalid or duplicate primary key {key!r}")
        require(group is None or groups[key] == group, f"{label}: primary key {key} violates filter")
        seen.add(key)
        squared = squared_distance(vectors[key], query)
        require(close_number(distance, math.sqrt(squared)),
                f"{label}: wrong distance {distance!r} for primary key {key}")
        ranked.append((squared, key))
    require(ranked == sorted(ranked), f"{label}: rows violate distance or primary-key order")
    return [key for _, key in ranked]


def query_both_modes(session, vectors, groups, queries, oracle_indices):
    sizes = Counter(groups)
    latency = {"exact": [], "ann": []}
    sampled_ids = {}
    for index, (group, query, sql) in enumerate(queries):
        count = min(TOP_K, len(vectors) if group is None else sizes[group])
        ids = {}
        # Alternate which mode runs first in blocks of two so filter parity
        # does not determine the mode with a warmed query.
        modes = ("exact", "ann") if (index // 2) % 2 == 0 else ("ann", "exact")
        for mode in modes:
            begin = perf_counter()
            response = session.request(sql, mode=mode)
            latency[mode].append(perf_counter() - begin)
            ids[mode] = check_query(response, vectors, groups, group, query, count, f"{mode} query {index}")
        require(ids["exact"] == ids["ann"], f"query {index}: exact {ids['exact']} differs from ann {ids['ann']}")
        if index in oracle_indices:
            sampled_ids[index] = ids["exact"]
    return latency, sampled_ids


def check_oracle(vectors, groups, queries, sampled_ids):
    for index, observed in sorted(sampled_ids.items()):
        group, query, _ = queries[index]
        candidates = ((squared_distance(vector, query), key) for key, vector in enumerate(vectors)
                      if group is None or groups[key] == group)
        expected = [key for _, key in heapq.nsmallest(TOP_K, candidates)]
        require(observed == expected, f"oracle query {index}: got {observed}, expected {expected}")
    for index in (0, 1):
        require(sampled_ids[index][:2] == [0, 1], f"query {index}: primary-key tie check failed")


def check_restart(session, vectors, groups):
    response = execute(session, "SELECT id,grp FROM docs ORDER BY id;", "read restarted rows")
    actual = response.get("rows")
    require(response.get("columns") == ["id", "grp"] and type(actual) is list and len(actual) == len(vectors),
            "restart returned the wrong columns or record count")
    for key, row in enumerate(actual):
        require(type(row) is list and len(row) == 2 and all(type(cell) is int for cell in row)
                and row == [key, groups[key]], f"restart changed record {key}: {row!r}")

    samples = {slot * (len(vectors) - 1) // 9 for slot in range(10)}
    for key in sorted(samples):
        response = execute(session, f"SELECT embedding FROM docs WHERE id={key};", f"read restarted vector {key}")
        rows = response.get("rows")
        require(response.get("columns") == ["embedding"] and type(rows) is list and len(rows) == 1
                and type(rows[0]) is list and len(rows[0]) == 1, f"restart missing vector {key}")
        vector = rows[0][0]
        require(type(vector) is list and len(vector) == DIMENSION
                and all(close_number(actual, expected) for actual, expected in zip(vector, vectors[key])),
                f"restart changed vector {key}")
    return len(samples)


def summarize(latency):
    ordered = sorted(latency)
    elapsed = math.fsum(latency)
    return {
        "count": len(latency),
        "total_seconds": elapsed,
        "p50_ms": 1000 * median(latency),
        "p95_ms": 1000 * ordered[math.ceil(0.95 * len(ordered)) - 1],
        "qps": len(latency) / elapsed,
    }


def benchmark(args):
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    vectors, groups, queries, oracle_indices = make_workload(args.rows, args.queries, args.seed)
    command = None if args.image else [str(args.binary), "--data", "{data_dir}", "--protocol", "jsonl"]
    with tempfile.TemporaryDirectory(prefix="minivec-jsonl-bench-") as scratch:
        log = Path(scratch) / "database.stderr.log"
        session = Session(config, log, image=args.image, command=command)
        try:
            try:
                session.start()
                ingest_seconds = ingest(session, vectors, groups, args.batch_size)
                begin = perf_counter()
                execute(session, "CREATE VECTOR INDEX docs_v ON docs(embedding);", "build vector index")
                index_seconds = perf_counter() - begin
                latency, sampled_ids = query_both_modes(session, vectors, groups, queries, oracle_indices)
                session.shutdown()

                begin = perf_counter()
                session.start()
                restart_seconds = perf_counter() - begin
                vector_samples = check_restart(session, vectors, groups)
                session.shutdown()
            finally:
                session.close()
        except Exception:
            with log.open("rb") as stderr:
                size = stderr.seek(0, 2)
                stderr.seek(max(0, size - 4096))
                print(f"database stderr tail: {stderr.read().decode('utf-8', 'replace')!r}", file=sys.stderr)
            raise

    check_oracle(vectors, groups, queries, sampled_ids)
    return {
        "kind": "local_synthetic_jsonl_benchmark",
        "target": {"image": args.image} if args.image else {"binary": str(args.binary)},
        "seed": args.seed,
        "rows": args.rows,
        "dimension": DIMENSION,
        "queries_per_mode": args.queries,
        "filtered_queries_per_mode": args.queries // 2,
        "k": TOP_K,
        "batch_size": args.batch_size,
        "ingest_seconds": ingest_seconds,
        "index_build_seconds": index_seconds,
        "query": {mode: summarize(samples) for mode, samples in latency.items()},
        "correctness": {
            "ann_matches_exact": True,
            "full_scan_oracle_query_indices": sorted(sampled_ids),
            "primary_key_tie": True,
            "restart_seconds": restart_seconds,
            "restart_records_checked": args.rows,
            "restart_vectors_checked": vector_samples,
        },
        "measurement": "End-to-end JSONL requests with shared index and interleaved modes. "
                       "Query latency excludes workload generation and validation. Local diagnostic only.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--image", help="built MiniVectorDB Docker image")
    target.add_argument("--binary", type=Path, help="local Linux MiniVectorDB binary")
    parser.add_argument("--output", required=True, type=Path, help="new JSON report path")
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--queries", type=int, default=1000, help="queries per mode (at least 10)")
    parser.add_argument("--batch-size", type=int, choices=(100, 500), default=100)
    parser.add_argument("--seed", type=int, default=7319)
    args = parser.parse_args()
    if args.rows < 2 or args.queries < ORACLE_QUERIES:
        parser.error(f"need at least two rows and {ORACLE_QUERIES} queries for the oracle and tie checks")
    if args.output.exists():
        parser.error("--output must be a new file")
    if args.binary:
        args.binary = args.binary.resolve()
        if not args.binary.is_file():
            parser.error(f"binary does not exist: {args.binary}")

    report = json.dumps(benchmark(args), indent=2, allow_nan=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        stream.write(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
