#!/usr/bin/env python3
"""Check shared-bound fanout against exact float32 distances in a real binary.

Run with --binary /path/minivec --output results.json. The test runs both the
normal policy and MVDB_QUERY_THREADS=2 in separate processes and databases.
There is no timing claim or assumption that forced fanout proves a particular
shared-cutoff instruction executed; that observability limit is reported.
"""

import argparse
from contextlib import contextmanager
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import tempfile

from json_protocol import Session


F32 = struct.Struct("!f")
MIN_F32 = 2.0 ** -149
FANOUT_TOPK_MAX = 1024  # Matches the bounded fanout gate in exec.cpp.


def vector(values):
    coordinates = tuple(F32.unpack(F32.pack(value))[0] for value in values)
    scaled = tuple((num << 149) // den for num, den in map(float.as_integer_ratio, coordinates))
    sql = "'[" + ",".join(map(repr, coordinates)) + "]'"
    return coordinates, scaled, sql


def distance(left, right):
    # Binary32 values are exact multiples of 2^-149. Sum integer squares and
    # round once to binary64; no engine distance, floating reduction or numpy.
    return float(Fraction(sum((a-b)**2 for a, b in zip(left[1], right[1])), 1 << 298))


def fixtures():
    rng = random.Random(814793)
    result = []

    def add(name, dim, count, make_row, queries, ks):
        keys = rng.sample(range(-4_000_000, 4_000_000), count)
        keys[count // 2 - 1] = -9007199254740993
        keys[-1] = -9007199254740997
        keys[0] = 9007199254740997
        rows = [(key, vector(make_row(i))) for i, key in enumerate(keys)]
        prepared = []
        for label, values in queries:
            query = vector(values)
            ranked = sorted((distance(value, query), key) for key, value in rows)
            prepared.append((label, query, [key for _, key in ranked]))
        result.append({"name": name, "dim": dim, "rows": rows, "queries": prepared, "ks": ks})

    # All distances tie within and across halves. Late smaller BIGINT PKs must
    # replace earlier entries, even if the other worker has already published.
    add("cross_half_equal", 128, 1153, lambda i: [1.0] + [0.0]*127,
        [("zero", [0.0]*128)], [1, 10, 32, 575, 577, 1024, 1154])

    # The two query locations reverse which half owns almost all good rows.
    # This stresses early/late publication and a strong inter-half asymmetry.
    add("half_distribution_skew", 128, 2049,
        lambda i: [((i % 19) / 64.0 if i < 1024 else 64.0 + (i % 19) / 64.0)] * 128,
        [("left_hot", [0.0]*128), ("right_hot", [64.0]*128)], [1, 10, 127, 1024, 1025])

    for dim in (16, 129):
        add(f"non128_dim{dim}", dim, 1153,
            lambda i, dim=dim: [((i * 17 + d * 11) % 97 - 48) / 32.0 + (12 if i >= 576 else 0)
                                for d in range(dim)],
            [("left", [0.0]*dim), ("right", [12.0]*dim)], [1, 10, 577, 1024])

    # Real distances just below/above a rounding midpoint can share a canonical
    # binary64 key. Strict > is necessary: equality still falls back to exact
    # ordering and the smaller PK. Dimension 8 also bypasses int8 quantization.
    h = 2.0 ** 30
    def near_row(i):
        values = [2*h, 0, 0, 0, 0, 0, 0, 0]
        if i in (0, 575):
            values = [h, 16, 0, 0, 0, 0, 0, 0]
        if i in (576, 1152):
            values = [h, 16, 8, 8, 0, 0, 0, 0]
        if i in (64, 640):
            values = [h, 16, 8, 0, 8, 0, 0, 0]
        return values
    near_query = [0, 0, 0, MIN_F32, 0, 0, 0, 0]
    add("rounded_ties_no_int8", 8, 1153, near_row, [("near", near_query)], [1, 2, 4, 10, 577])
    near = result[-1]
    q = near["queries"][0][1]
    assert distance(near["rows"][0][1], q) == distance(near["rows"][-1][1], q) == 2.0**60 + 256
    assert near["queries"][0][2][0] == -9007199254740997

    # k exceeds each half for k=66 but remains <=1024: a half must not publish
    # an uninitialized/partial-heap threshold. This is forced fanout only on
    # ordinary hosts; it intentionally need not meet the adaptive minimum size.
    add("both_halves_smaller_than_k", 128, 129,
        lambda i: [(i % 11) / 8.0] + [0.0]*127,
        [("zero", [0.0]*128)], [64, 65, 66, 128, 129, 130])

    # Dense equal norms force canonical/PK comparisons throughout each half;
    # sparse equal-distance rows above do not exercise the same arithmetic.
    # Append new fixtures so the prior seeded row/PK families remain intact.
    add("lazy128_dense_equal_norm", 128, 1153,
        lambda i: [rng.choice((-1.0, 1.0)) for _ in range(128)],
        [("zero", [0.0]*128)], [1, 10, 32, 33])
    result[-1]["extra_checks"] = ("projection", "descending", "where")
    assert all(distance(value, result[-1]["queries"][0][1]) == 128.0
               for _, value in result[-1]["rows"])

    # Pad the established midpoint fixture to the new 128D admission shape.
    # Unequal exact real distances can still have the same rounded key.
    add("lazy128_rounded_ties", 128, 1153,
        lambda i: near_row(i) + [0.0]*120,
        [("near", near_query + [0.0]*120)], [1, 2, 4, 10, 32, 33])
    result[-1]["extra_checks"] = ("projection",)
    padded = result[-1]
    q = padded["queries"][0][1]
    assert distance(padded["rows"][0][1], q) == distance(padded["rows"][-1][1], q) == 2.0**60 + 256
    assert padded["queries"][0][2][0] == -9007199254740997

    # Every finite nonzero square here overflows binary32. Binary64 canonical
    # distances remain finite, so a float overflow must use exact fallback.
    max_f32 = F32.unpack(bytes.fromhex("7f7fffff"))[0]
    add("lazy128_finite_overflow", 128, 1153,
        lambda i: [(-1.0 if (i+d) & 1 else 1.0) *
                   (max_f32 if (i+3*d) % 7 < 3 else max_f32/2.0)
                   for d in range(128)],
        [("zero", [0.0]*128)], [1, 10, 32, 33])
    result[-1]["extra_checks"] = ("projection",)
    return result


@contextmanager
def running(binary, data, mode, log, timeout):
    env = os.environ.copy()
    for key in ("MVDB_PROTOCOL", "MVDB_OUT", "MVDB_SINGLE_THREAD", "MVDB_QUERY_THREADS",
                "MVDB_QUERY_FANOUT_ROWS", "MVDB_WAKE_US_OVERRIDE", "MVDB_ANN_PROBE_PERCENT"):
        env.pop(key, None)
    env["MVDB_PROTOCOL"] = "jsonl"
    if mode == "forced_two":
        env["MVDB_QUERY_THREADS"] = "2"
    with log.open("wb") as stderr:
        session = Session([binary, "--data", str(data), "--protocol", "jsonl"], env, timeout, stderr)
        try:
            assert session.read_json("ready") == {"type": "ready"}
            yield session
            session.shutdown()
        finally:
            session.close()


def sql(session, statement, label):
    session.send_json({"id": label, "op": "execute", "sql": statement, "search_mode": "exact"})
    response = session.read_json(label)
    assert response.get("id") == label and response.get("ok") is True, (label, response)
    return response


def run_fixture(session, fixture, results):
    name, dim, rows = fixture["name"], fixture["dim"], fixture["rows"]
    sql(session, f"CREATE TABLE {name}(id BIGINT PRIMARY KEY, embedding VECTOR({dim}));", "create-" + name)
    sql(session, "BEGIN;", "begin-" + name)
    for offset in range(0, len(rows), 64):
        chunk = rows[offset:offset+64]
        values = ",".join(f"({key},{value[2]})" for key, value in chunk)
        response = sql(session, f"INSERT INTO {name} VALUES {values};", f"load-{name}-{offset}")
        assert response.get("affected_rows") == len(chunk), response
    sql(session, "COMMIT;", "commit-" + name)
    sql(session, f"CREATE VECTOR INDEX {name}_v ON {name}(embedding);", "index-" + name)
    for query_name, query, ranking in fixture["queries"]:
        for k in fixture["ks"]:
            label = f"{name}-{query_name}-k{k}"
            response = sql(session, f"SELECT id FROM {name} ORDER BY L2_DISTANCE(embedding,{query[2]}) ASC LIMIT {k};", label)
            assert response.get("columns") == ["id"], (label, response)
            actual = response.get("rows")
            expected = [[key] for key in ranking[:k]]
            assert actual == expected, (label, actual, expected)
            results.append({"case": label, "count": len(rows), "dimension": dim, "k": k,
                            "fanout_k_gate_allows": 1 <= k <= FANOUT_TOPK_MAX,
                            "lazy128_shape_eligible": dim == 128 and 1 <= k <= 32,
                            "k_exceeds_first_half": k > len(rows)//2,
                            "k_exceeds_both_halves": k > (len(rows)+1)//2})
        # These retain the same independent integer/Fraction oracle. Eligibility
        # labels describe SQL shape only, never prove an internal path executed.
        for check in fixture.get("extra_checks", ()):
            k = 10
            label = f"{name}-{query_name}-{check}-k{k}"
            if check == "projection":
                response = sql(session, f"SELECT id,L2_DISTANCE(embedding,{query[2]}) AS d FROM {name} ORDER BY d ASC LIMIT {k};", label)
                lookup = dict(rows)
                expected = [[key, math.sqrt(distance(lookup[key], query))] for key in ranking[:k]]
                assert response.get("columns") == ["id", "d"], (label, response)
                eligible = dim == 128
            elif check == "descending":
                response = sql(session, f"SELECT id FROM {name} ORDER BY L2_DISTANCE(embedding,{query[2]}) DESC LIMIT {k};", label)
                descending = sorted((-distance(value, query), key) for key, value in rows)
                expected = [[key] for _, key in descending[:k]]
                assert response.get("columns") == ["id"], (label, response)
                eligible = False
            else:
                assert check == "where"
                response = sql(session, f"SELECT id FROM {name} WHERE id >= 0 ORDER BY L2_DISTANCE(embedding,{query[2]}) ASC LIMIT {k};", label)
                expected = [[key] for key in ranking if key >= 0][:k]
                assert response.get("columns") == ["id"], (label, response)
                eligible = False
            assert response.get("rows") == expected, (label, response.get("rows"), expected)
            results.append({"case": label, "count": len(rows), "dimension": dim, "k": k,
                            "check": check, "lazy128_shape_eligible": eligible})
    # Free old fixtures so dimensions and cumulative index memory cannot force
    # a different residency decision in a later test.
    sql(session, f"DROP TABLE {name};", "drop-" + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    prepared = fixtures()
    fingerprint = hashlib.sha256()
    for fixture in prepared:
        fingerprint.update(fixture["name"].encode())
        for key, value in fixture["rows"]:
            fingerprint.update(f"{key}:{value[2]}\n".encode())
        for label, query, ranking in fixture["queries"]:
            fingerprint.update((label + query[2] + repr(fixture["ks"])).encode())
        fingerprint.update(repr(fixture.get("extra_checks", ())).encode())
    summary = {"result": "pass", "oracle": "exact integer float32 square sum rounded once to binary64; ties use signed BIGINT PK",
               "fixture_sha256": fingerprint.hexdigest(), "modes": {},
               "shared_cutoff_execution_directly_observed": False,
               "lazy_interval_execution_directly_observed": False,
               "observability_limit": "Fanout selection logs are collected, but this binary has no shared-cutoff publication/use or lazy-interval execution counter. Correct answers and eligible SQL shapes cannot prove either optimization executed. Forced mode may fall back when fewer than two online CPUs or worker creation fails."}
    with tempfile.TemporaryDirectory(prefix="mvdb-shared-bound-") as temp:
        root = Path(temp)
        for mode in ("forced_two", "normal"):
            records = []
            log = root / f"{mode}.stderr"
            with running(str(args.binary.resolve()), root / mode, mode, log, args.timeout) as session:
                for fixture in prepared:
                    run_fixture(session, fixture, records)
            diagnostics = log.read_text(encoding="utf-8", errors="replace")
            fanout_lines = [line for line in diagnostics.splitlines() if "fanout" in line]
            summary["modes"][mode] = {"queries": len(records), "cases": records,
                                      "fanout_two_threads_logged": any("2 threads" in line for line in fanout_lines),
                                      "fanout_logs": fanout_lines}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "pass", "fixture_sha256": summary["fixture_sha256"],
                      "queries_per_mode": {mode: value["queries"] for mode, value in summary["modes"].items()},
                      "shared_cutoff_execution_directly_observed": False,
                      "lazy_interval_execution_directly_observed": False}))


if __name__ == "__main__":
    main()
