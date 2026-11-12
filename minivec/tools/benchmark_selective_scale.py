#!/usr/bin/env python3
"""I11 selective-scale paired full-request experiment; never an official score.

Synthetic distributions use dyadic float32 inputs and an independent Python
integer squared-distance oracle for every query. Production norm ordering is
left enabled: row-dispersed outliers may be regrouped; coordinate-dispersed
outliers provide a separate negative control that ordering cannot eliminate.
"""
import argparse
from array import array
from collections import Counter
from datetime import datetime, timezone
import hashlib
import heapq
import json
import math
import os
from pathlib import Path
import platform
import random
from time import perf_counter

import benchmark_iteration as base

CASES = {
    "gaussian128": (10000, 128, "gaussian"),
    "outlier128": (20000, 128, "tail_outlier"),
    "row_dispersed128": (20000, 128, "row_dispersed"),
    "coordinate_dispersed128": (20000, 128, "coordinate_dispersed"),
    "high768_outlier": (4096, 768, "tail_outlier"),
    "high768_gaussian": (4096, 768, "gaussian"),
    "low16_100k": (100000, 16, "gaussian"),
}
DEFAULT_CASES = ("standard10k", "gaussian128", "outlier128", "row_dispersed128",
                 "coordinate_dispersed128", "high768_outlier", "low16_100k")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quantization_step(maximum):
    if maximum == 0:
        return 1.0
    value = math.ldexp(1.0, math.frexp(maximum)[1] - 7)
    return value * 2 if maximum / value > 127 else value


def predicted_gate(vectors):
    """Source-based prediction only; it is not a runtime branch observation."""
    count, dimension = len(vectors), len(vectors[0])
    keys = list(range(count - 1, -1, -1))
    # Single-table cases fit the metadata reservation if this fixed 1MiB norm
    # scratch limit permits ordering. Larger 16D cases retain insertion order.
    ordered = count <= (1 << 20) // 16
    if ordered:
        def norm(key):
            total = 0.0
            for value in vectors[key]:
                total += float(value) * value
            return total, count - 1 - key
        keys.sort(key=norm)
    maxima = [max(abs(value) for value in vectors[key]) for key in keys]
    global_step = quantization_step(max(maxima))
    histogram = Counter()
    for start in range(0, count, 256):
        histogram[quantization_step(max(maxima[start:start + 256]))] += min(256, count - start)
    strong = sum(rows for step, rows in histogram.items() if step <= global_step / 4)
    return {"kind": "reconstructed_from_inputs_not_live_branch_evidence", "norm_order_expected": ordered,
        "code_bytes": count * dimension, "global_step": global_step,
        "rows_by_local_step": {str(step): rows for step, rows in sorted(histogram.items())},
        "substantially_finer_rows": strong, "required_rows": count - count // 5,
        "selective_eligible_expected": (dimension >= 128 and count * dimension <= 4 * 1024**2 and
                                        2 <= len(histogram) <= 8 and strong >= count - count // 5)}


def make_workload(name, seed, queries, batch_size):
    if name in ("standard10k", "low16_100k"):
        spec = base.FULL_CASES[name] if name == "standard10k" else (100000, 16, 0, 0)
        work = base.make_workload(spec, seed, queries, batch_size, 10)
        work["gate_prediction"] = predicted_gate(work["vectors"])
        work["distribution"] = ("original standard10k mixed-filter control" if name == "standard10k" else
                                "original large100k low16 regression control") + "; original sampled fsum oracle"
        return work
    count, dimension, family = CASES[name]
    rng = random.Random(seed)
    def normal_integer():
        return max(-32768, min(32768, round(rng.gauss(0, 1) * 4096)))
    integers = []
    for key in range(count):
        values = [normal_integer() for _ in range(dimension)]
        is_outlier = ((family == "tail_outlier" and key >= count - max(1, count // 100)) or
                      (family == "row_dispersed" and key % 100 == 0))
        if is_outlier:
            values = [value * 128 for value in values]
        if family == "coordinate_dispersed":
            values[key % dimension] = (-1 if key % 2 else 1) * 128 * 4096
        integers.append(values)
    integers[1] = integers[0][:]  # Exact distance ties must keep PK ascending.
    vectors = [array("f", (value / 4096 for value in row)) for row in integers]
    query_integers = [integers[0][:]] + [[normal_integer() for _ in range(dimension)] for _ in range(queries - 1)]
    query_vectors = [array("f", (value / 4096 for value in row)) for row in query_integers]
    create = f"CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dimension}));"
    inserts = []
    source_hash = hashlib.sha256(create.encode())
    for stop in range(count, 0, -batch_size):
        start = max(0, stop - batch_size)
        sql = "INSERT INTO docs VALUES " + ",".join(
            f"({key},{base.vector_literal(vectors[key])})" for key in range(stop - 1, start - 1, -1)) + ";"
        inserts.append((sql, stop - start)); source_hash.update(sql.encode())
    sql_queries = [f"SELECT id,L2_DISTANCE(v,{base.vector_literal(query)}) AS d FROM docs ORDER BY d LIMIT 10;"
                   for query in query_vectors]
    oracle = {}
    for index, query in enumerate(query_integers):
        # Integer accumulation cannot lose any dyadic term. Conversion rounds
        # once to binary64, matching the canonical exact key including its ties.
        def distances():
            for key, row in enumerate(integers):
                total = sum((left - right) ** 2 for left, right in zip(row, query))
                yield math.ldexp(float(total), -24), key
        oracle[index] = heapq.nsmallest(10, distances())
    for sql in sql_queries:
        source_hash.update(sql.encode())
    return {"rows": count, "dimension": dimension, "updated_rows": 0, "padding_bytes": 0,
        "create": create, "inserts": inserts, "queries": sql_queries, "update": None,
        "before_oracle": oracle, "after_oracle": oracle, "oracle_indices": list(range(queries)),
        "vectors": vectors, "updated_vector": query_vectors[1], "sha256": source_hash.hexdigest(),
        "k": 10, "query_counts": [10] * queries, "group_modulus": 0, "mixed_filters": False,
        "distribution": family, "gate_prediction": predicted_gate(vectors)}


def one_run(binary, workload, args):
    # Instrument only this standalone experiment. The shared harness file and
    # its total phase timers remain unchanged. These nested request timers
    # include transport/JSON decode, and help distinguish COMMIT waiting.
    original = base.Session
    records = {name: [] for name in ("BEGIN", "INSERT", "COMMIT")}
    class IngestSession(original):
        def request(self, sql=None, **options):
            kind = next((name for name in records if sql and sql.startswith(name)), None)
            if kind is None:
                return super().request(sql, **options)
            start = perf_counter()
            response = super().request(sql, **options)
            records[kind].append(perf_counter() - start)
            return response
    base.Session = IngestSession
    try:
        result = base.one_run(binary, workload, args)
    finally:
        base.Session = original
    result["ingest_request_breakdown"] = {name: {"raw_seconds": values, "seconds": math.fsum(values),
                                                "requests": len(values)} for name, values in records.items()}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", action="append", choices=["standard10k", *CASES])
    parser.add_argument("--seeds", type=int, nargs="+", default=[7319, 8911, 13337])
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--queries", type=int, default=64)
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--case-timeout", type=float, default=1800)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Linux transport and /proc are required")
    if min(args.pairs, args.batch_size, args.timeout, args.case_timeout) <= 0 or args.queries < 2 or len(set(args.seeds)) != len(args.seeds):
        parser.error("invalid repetitions/seeds/timeouts")
    if args.output.exists() or args.output.with_name(args.output.name + ".tmp").exists():
        parser.error("refusing to overwrite evidence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.baseline, args.candidate = args.baseline.resolve(), args.candidate.resolve()
    cpus = sorted(os.sched_getaffinity(0))[:2]
    if len(cpus) != 2:
        parser.error("two allowed CPUs are required")
    os.sched_setaffinity(0, cpus)
    report = {"status": "running", "kind": "selective_scale_whole_request_comparison", "official_score": None,
        "pairs": [], "workloads": [], "created_utc": datetime.now(timezone.utc).isoformat(),
        "parameters": {"scenarios": args.scenario or list(DEFAULT_CASES), "seeds": args.seeds,
                       "pairs_per_seed": args.pairs, "queries_per_phase": args.queries},
        "host": {"platform": platform.platform(), "affinity": cpus}, "environment": base.runtime_environment(),
        "binaries": {name: {"path": str(getattr(args, name)), "sha256": digest(getattr(args, name))}
                     for name in ("baseline", "candidate")},
        "harness_sha256": {path.name: digest(path) for path in (Path(__file__), Path(base.__file__))},
        "limitations": ["Not official scoring. Shared cache; all positive/negative pairs retained.",
            "Process RSS/IO are proxies, not cgroup M/B; CPU samples have clock-tick granularity.",
            "Integer oracle covers every new synthetic query. Original standard10k/large100k controls retain their sampled oracles.",
            "Gate predictions are reconstructed input summaries, not runtime branch measurements.",
            "Row-dispersed outliers can be regrouped by norm ordering; coordinate-dispersed is a separate control.",
            "BEGIN/INSERT/COMMIT nested wall times include protocol decode; no fsync duration is directly observed."]}
    base.write_report(args.output, report)
    pair = None
    try:
        for case_index, name in enumerate(args.scenario or DEFAULT_CASES):
            for seed_index, seed in enumerate(args.seeds):
                start = perf_counter()
                workload = make_workload(name, seed, args.queries, args.batch_size)
                report["workloads"].append({"scenario": name, "seed": seed,
                    "input_sha256": workload["sha256"], "distribution": workload["distribution"],
                    "generation_and_oracle_seconds_excluded": perf_counter() - start,
                    "gate_prediction": workload["gate_prediction"]})
                base.write_report(args.output, report)
                for index in range(args.pairs):
                    order = ["baseline", "candidate"]
                    if (case_index + seed_index + index) % 2:
                        order.reverse()
                    pair = {"scenario": name, "seed": seed, "pair_index": index, "order": order,
                            "workload_sha256": workload["sha256"]}
                    for side in order:
                        print(f"{name} seed={seed} pair={index + 1}/{args.pairs} {side}", flush=True)
                        pair[side] = one_run(getattr(args, side), workload, args)
                    base.require(pair["baseline"]["response_sha256"] == pair["candidate"]["response_sha256"], "response mismatch")
                    pair["all_responses_identical"] = True
                    report["pairs"].append(pair)
                    report["summary"] = base.paired_summary(report["pairs"])
                    base.write_report(args.output, report)
        base.require(all(digest(getattr(args, name)) == report["binaries"][name]["sha256"] for name in ("baseline", "candidate")),
                     "binary changed during measurement")
        report["status"] = "passed"
    except Exception as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}", failed_pair=pair)
        base.write_report(args.output, report)
        raise
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    base.write_report(args.output, report)
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
