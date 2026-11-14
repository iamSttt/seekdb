#!/usr/bin/env python3
"""I17/I18 same-PGO filter-cache screen: one row written then 1 or 16 reads.

Defaults: 5000 rows x 128 dimensions x 8192-byte VARCHAR; low/high filter hit
rate x 1/16 reads x 3 seeds x 1 alternating pair = 12 pairs. Four write cycles.
For spill scenarios the caller supplies two binaries built with identical
PGO/compiler settings; only MVDB_ENABLE_SPILL_FILTER may differ. This tool
never builds a binary.
Explicit resident_compound_q1/q16 scenarios instead use 10000 resident rows,
50% AND 0.1% scalar conditions, and baseline MVDB_DISABLE_MULTI_POSTING versus
the default candidate. Run these two scenarios in a separate 6-pair report.
"""
import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
from time import perf_counter

import benchmark_iteration as base
import benchmark_index_delta as lifecycle

SPILL_CASES = ("spill_low_q1", "spill_high_q1", "spill_low_q16", "spill_high_q16")
RESIDENT_CASES = ("resident_compound_q1", "resident_compound_q16")
CASES = SPILL_CASES + RESIDENT_CASES


def make_workload(name, seed, cycles=4, batch_size=100, query_mode="mixed"):
    base.query_mode_for(0, query_mode)
    base.require(name in CASES and cycles > 0, "invalid write/query workload")
    resident = name in RESIDENT_CASES
    rows, dimension, padding = (10000, 128, 0) if resident else (5000, 128, 8192)
    reads = 16 if name.endswith("q16") else 1
    low = "_low_" in name
    source = base.make_workload((rows, dimension, 0, 0), seed, 2, batch_size, 10)
    model = {pk: (vector, pk % 2, (pk // 2) % 1000) if resident else (vector, pk % 1000)
             for pk, vector in enumerate(source["vectors"])}
    vectors = [array("f", [0] * dimension), source["vectors"][0]]
    pad = "x" * padding
    extra = ",h BIGINT" if resident else f",pad VARCHAR({padding})"
    create = f"CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dimension}),g BIGINT{extra});"
    inserts = []
    for stop in range(rows, 0, -batch_size):
        keys = range(stop - 1, max(0, stop - batch_size) - 1, -1)
        sql = "INSERT INTO docs VALUES " + ",".join(
            f"({pk},{base.vector_literal(model[pk][0])},{model[pk][1]}," +
            (str(model[pk][2]) if resident else f"'{pad}'") + ")" for pk in keys) + ";"
        inserts.append((sql, min(batch_size, stop)))

    def query_set(stage):
        result, expected = [], {}
        for index in range(reads):
            vector_index = (index + stage) % 2
            vector = vectors[vector_index]
            if vector_index not in expected:
                # Independent exhaustive full sort, including deterministic PK
                # ties. The production implementation uses neither this model
                # nor Python's arithmetic or ordering implementation.
                expected[vector_index] = sorted(
                    (base.squared_distance(value[0], vector), pk)
                    for pk, value in model.items() if
                    ((value[1] == 1 and value[2] == 7) if resident else (not low or value[1] == 7)))[:10]
            predicate = "g=1 AND h=7" if resident else "g=7" if low else "g>=0"
            sql = f"SELECT id,L2_DISTANCE(v,{base.vector_literal(vector)}) AS d FROM docs WHERE {predicate} ORDER BY d LIMIT 10;"
            result.append({"sql": sql, "mode": base.query_mode_for(index + stage, query_mode),
                           "oracle": expected[vector_index]})
        return result

    before = query_set(0)
    steps, changed = [], set()
    for cycle in range(cycles):
        # Alternate moving one row into and out of the selective group. Spill
        # changes scalar+vector; resident compound changes its two scalar keys.
        if resident:
            pk = cycle // 2 % rows if cycle % 2 == 0 else (15 + 2000 * (cycle // 2)) % rows
            group, second = (1, 7) if cycle % 2 == 0 else (0, 8)
            vector = model[pk][0]
            sql = f"UPDATE docs SET g={group},h={second} WHERE id={pk};"
            model[pk] = (vector, group, second)
        else:
            pk = cycle // 2 % rows if cycle % 2 == 0 else (7 + 1000 * (cycle // 2)) % rows
            group = 7 if cycle % 2 == 0 else 8
            vector = array("f", [float(cycle % 3) / 8] * dimension)
            sql = f"UPDATE docs SET g={group},v={base.vector_literal(vector)} WHERE id={pk};"
            model[pk] = (vector, group)
        changed.add(pk)
        steps.append({"operations": [{"kind": "update", "pk": pk, "sql": sql}],
                      "queries": query_set(cycle + 1)})
    after = query_set(cycles + 1)
    digest = hashlib.sha256(create.encode())
    for sql, _ in inserts:
        digest.update(sql.encode())
    for phase in [before, *[step["queries"] for step in steps], after]:
        for query in phase:
            digest.update(base.encoded({key: query[key] for key in ("sql", "mode")}))
    for step in steps:
        digest.update(b"BEGIN;COMMIT;")
        digest.update(step["operations"][0]["sql"].encode())
    work = {"query_mode_policy": query_mode, "create": create, "inserts": inserts, "before": before, "cycles": steps,
            "after": after, "final_model": model, "changed_ids": sorted(changed),
            "reads_per_write": reads,
            "padding_bytes": padding, "rows": rows, "dimension": dimension,
            "initial_matching_rows": 5 if low or resident else rows, "mutation_count": cycles,
            "sha256": digest.hexdigest()}
    if resident:
        work.update(require_resident=True, verify_second_scalar=True)
    else:
        work.update(require_real_spill=True, padding_value=pad)
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("baseline", "candidate", "output"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--scenario", choices=CASES, action="append")
    parser.add_argument("--seeds", type=int, nargs="+", default=[7319, 8911, 13337])
    parser.add_argument("--pairs", type=int, default=1)
    parser.add_argument("--cycles", type=int, default=4)
    parser.add_argument("--query-mode", choices=("mixed", "exact"), default="mixed")
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--case-timeout", type=float, default=1800)
    parser.add_argument("--build-context", type=Path, required=True,
                        help="PGO build manifest with compiler/flags/training source and both binary SHA256 values")
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("requires Linux JSONL transport and /proc")
    if min(args.pairs, args.cycles, args.batch_size, args.timeout, args.case_timeout) <= 0:
        parser.error("positive sizes/timeouts required")
    if len(set(args.seeds)) != len(args.seeds) or (args.scenario and len(set(args.scenario)) != len(args.scenario)):
        parser.error("duplicate seed/scenario")
    cases = args.scenario or SPILL_CASES
    families = {"resident_compound" if name in RESIDENT_CASES else "spill" for name in cases}
    if len(families) != 1:
        parser.error("resident compound and spill contrasts require separate binaries, contexts and reports")
    family = families.pop()
    if any(key in os.environ for key in ("MVDB_ANN_PROBE_PERCENT", "MVDB_ANN_OFF")):
        parser.error("unset ANN policy overrides for default ANN transport")
    if args.output.exists() or args.output.with_name(args.output.name + ".tmp").exists():
        parser.error("refusing to overwrite measurements")
    for side in ("baseline", "candidate"):
        binary = getattr(args, side).resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"missing/nonexecutable {side}: {binary}")
        setattr(args, side, binary)
    # A filename/CLI assertion cannot prove that two executables used PGO.
    # Require the caller's build manifest and bind it to the exact measured bytes.
    context_raw = args.build_context.read_bytes()
    context = json.loads(context_raw)
    binaries = {side: {"path": str(getattr(args, side)),
                       "sha256": hashlib.sha256(getattr(args, side).read_bytes()).hexdigest()}
                for side in ("baseline", "candidate")}
    base.require(context.get("pgo_enabled") is True and context.get("same_compiler_and_training") is True,
                 "build context must establish both PGO-enabled builds with matching compiler/training")
    for side in binaries:
        base.require(context.get("binaries", {}).get(side, {}).get("sha256") == binaries[side]["sha256"],
                     f"build context does not bind {side} binary")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"kind": "paired_filter_write_query_lifecycle_screen", "family": family,
              "status": "running", "official_score": None,
              "created_utc": datetime.now(timezone.utc).isoformat(),
              "expected_total_pairs": len(cases) * len(args.seeds) * args.pairs,
              "parameters": vars(args) | {key: str(getattr(args, key)) for key in
                                           ("baseline", "candidate", "output", "build_context")},
              "environment": base.runtime_environment(), "binaries": binaries,
              "build_context": {"path": str(args.build_context), "sha256": hashlib.sha256(context_raw).hexdigest(),
                                "content": context},
              "host": {"platform": platform.platform(), "cpu_affinity": sorted(os.sched_getaffinity(0))},
              "limits": ["Not official scoring; no score inferred.",
                         "Default spill 12 pairs or explicit resident compound 6 pairs are screening, not default adoption.",
                         "Both executables must use the same original PGO training and compiler; only the chosen filter feature differs.",
                         "Spill runs require positive-size payload/index spill files; resident runs require no spill files and observed process RSS before first query.",
                         "Every query has an independent exhaustive sorted float32-input oracle; all IDs and mutated vectors/scalars plus sampled VARCHARs are checked after restart.",
                         "Measured lifecycle includes table/ingest/index/commits/all queries/shutdown/restart; verification and snapshots excluded.",
                         "Separate full-run wall includes verification and snapshots, and must not be compared to request-only totals.",
                         "Process RSS/HWM/CPU/IO are proxies, not cgroup totals; final shutdown resources omitted.",
                         "Shared page cache, no eviction/cgroup limits imposed; no timing is discarded."],
              "pairs": []}
    base.write_report(args.output, report)
    pair = None
    try:
        for case_index, name in enumerate(cases):
            for seed_index, seed in enumerate(args.seeds):
                started = perf_counter();work = make_workload(name, seed, args.cycles, args.batch_size, args.query_mode)
                generation = perf_counter() - started
                for index in range(args.pairs):
                    order = ["baseline", "candidate"]
                    if (case_index + seed_index + index) % 2:
                        order.reverse()
                    pair = {"scenario": name, "seed": seed, "pair_index": index, "order": order,
                            "workload_sha256": work["sha256"], "mutation_count": work["mutation_count"],
                            "reads_per_write": work["reads_per_write"], "initial_matching_rows": work["initial_matching_rows"],
                            "rows": work["rows"], "dimension": work["dimension"], "padding_bytes": work["padding_bytes"],
                            "generation_and_oracle_seconds_excluded": generation}
                    for side in order:
                        print(f"{name} seed={seed} pair={index + 1}/{args.pairs} {side}", flush=True)
                        pair[side] = {};lifecycle.one_run(getattr(args, side), work, args, pair[side])
                    base.require(pair["baseline"]["response_sha256"] == pair["candidate"]["response_sha256"],
                                 "paired response mismatch")
                    pair["all_responses_identical"] = True
                    report["pairs"].append(pair);report["summary"] = base.paired_summary(report["pairs"])
                    base.write_report(args.output, report)
        base.require(len(report["pairs"]) == report["expected_total_pairs"], "incomplete pair matrix")
        for side in binaries:
            base.require(hashlib.sha256(getattr(args, side).read_bytes()).hexdigest() == binaries[side]["sha256"],
                         "measured binary changed")
        report.update(status="passed", total_pairs=len(report["pairs"]), binary_unchanged=True)
    except Exception as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}", failed_pair=pair)
        base.write_report(args.output, report);raise
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    base.write_report(args.output, report)


if __name__ == "__main__":
    main()
