#!/usr/bin/env python3
"""Small paired JSONL lifecycle screen for opt-in mixed index deltas.

Run identical-build baseline vs MVDB_ENABLE_INDEX_DELTA, leaving ANN knobs
unset. Default: three workloads x three seeds x one alternating pair = 9 pairs.
One-pair defaults are a screen, never sufficient evidence for default adoption.
"""
import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import heapq
import json
import os
from pathlib import Path
import platform
import random
import tempfile
from time import perf_counter

import benchmark_iteration as base

CASES = ("mixed_small", "same_pk_churn", "over_64_limit")


def make_workload(name, seed, rows=10000, dimension=128, queries=8, batch_size=100):
    base.require(name in CASES and rows >= 6400 and queries >= 4, "invalid workload dimensions")
    source = base.make_workload((rows, dimension, 0, 0), seed, 2, batch_size, 10)
    initial = {key: (vector, key % 7) for key, vector in enumerate(source["vectors"])}
    initial[1] = (initial[0][0], initial[0][1])
    model = dict(initial)
    rng = random.Random(seed ^ 0xD317A)
    zero = array("f", [0] * dimension)
    qvectors = [initial[0][0], zero]
    create = f"CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dimension}),g BIGINT);"
    inserts = []
    for stop in range(rows, 0, -batch_size):
        keys = range(stop - 1, max(0, stop - batch_size) - 1, -1)
        sql = "INSERT INTO docs VALUES " + ",".join(
            f"({key},{base.vector_literal(initial[key][0])},{initial[key][1]})" for key in keys) + ";"
        inserts.append((sql, min(batch_size, stop)))

    def query_set():
        result = []
        cached = {}
        for index in range(queries):
            vector = qvectors[index % 2]
            group = 3 if index % 4 == 3 else None
            key = (index % 2, group)
            if key not in cached:
                cached[key] = heapq.nsmallest(10, (
                    (base.squared_distance(values[0], vector), pk) for pk, values in model.items()
                    if group is None or values[1] == group))
            sql = f"SELECT id,L2_DISTANCE(v,{base.vector_literal(vector)}) AS d FROM docs"
            if group is not None:
                sql += f" WHERE g={group}"
            sql += " ORDER BY d LIMIT 10;"
            result.append({"sql": sql, "mode": "exact" if index % 2 == 0 else "ann",
                           "oracle": cached[key]})
        return result

    before = query_set()
    cycles, changed = [], set()
    for cycle in range({"mixed_small": 4, "same_pk_churn": 8, "over_64_limit": 1}[name]):
        operations = []
        if name == "mixed_small":
            plan = [("update", 100 + cycle), ("delete", 200 + cycle), ("insert", rows + cycle)]
        elif name == "same_pk_churn":
            plan = [("delete", 9), ("insert", 9), ("update", 0)]
        else:
            plan = [("update", 100 + i) if i % 3 == 0 else
                    ("delete", 200 + i) if i % 3 == 1 else
                    ("insert", rows + i) for i in range(65)]
        for operation, pk in plan:
            changed.add(pk)
            if operation == "delete":
                base.require(pk in model, "generator deleted missing PK")
                sql = f"DELETE FROM docs WHERE id={pk};"
                del model[pk]
            else:
                # Repeated zeros plant ties among old, changed and inserted rows.
                vector = zero if name == "same_pk_churn" or len(operations) % 2 == 0 else array(
                    "f", (rng.gauss(0, 1) for _ in range(dimension)))
                if operation == "insert":
                    base.require(pk not in model, "generator inserted duplicate PK")
                    sql = f"INSERT INTO docs VALUES ({pk},{base.vector_literal(vector)},3);"
                else:
                    base.require(pk in model, "generator updated missing PK")
                    sql = f"UPDATE docs SET v={base.vector_literal(vector)},g=3 WHERE id={pk};"
                model[pk] = (vector, 3)
            operations.append({"kind": operation, "pk": pk, "sql": sql})
        cycles.append({"operations": operations, "queries": query_set()})
    after = query_set()
    digest = hashlib.sha256(create.encode())
    for sql, _ in inserts:
        digest.update(sql.encode())
    for query in before:
        digest.update(base.encoded({key: query[key] for key in ("sql", "mode")}))
    for cycle in cycles:
        digest.update(b"BEGIN;COMMIT;")
        for operation in cycle["operations"]:
            digest.update(operation["sql"].encode())
        for query in cycle["queries"]:
            digest.update(base.encoded({key: query[key] for key in ("sql", "mode")}))
    return {"create": create, "inserts": inserts, "before": before, "cycles": cycles,
            "after": after, "final_model": model, "changed_ids": sorted(changed),
            "sha256": digest.hexdigest(), "rows": rows, "dimension": dimension,
            "mutation_count": sum(len(cycle["operations"]) for cycle in cycles)}


def one_run(binary, work, args, result):
    full_started = perf_counter()
    config = json.loads((base.PROTOCOL_CHECK / "config.json").read_text())
    config.update(request_timeout=args.timeout, startup_timeout=args.timeout)
    phases = result.setdefault("phase_seconds", {})
    result.update(query={}, resource_snapshots=[], write_cycles=[], completed_requests=0)
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="mvdb-delta-log-") as temporary:
        log = Path(temporary) / "stderr.log"
        session = base.Session(config, log, command=[str(binary), "--data", "{data_dir}",
                                                     "--protocol", "jsonl"],
                               case_timeout=args.case_timeout)

        def snapshot(label):
            result["resource_snapshots"].append(base.process_snapshot(session, label))

        def request(sql, affected=None, mode="exact"):
            started = perf_counter()
            response = session.request(sql, mode=mode)
            elapsed = perf_counter() - started
            result["completed_requests"] += 1
            base.require(response.get("ok") is True, f"SQL failed: {response.get('error')} ({sql[:120]})")
            if affected is not None:
                base.require(type(response.get("affected_rows")) is int and
                             response["affected_rows"] == affected, "wrong affected rows")
            digest.update(base.encoded({key: value for key, value in response.items() if key != "id"}))
            return elapsed, response

        def query_phase(label, queries):
            timings = []
            # Store partial progress too, so a failed later request cannot erase
            # the successful timings or response evidence that preceded it.
            result["query"][label] = {"per_request_seconds": timings, "modes": []}
            for index, query in enumerate(queries):
                elapsed, response = request(query["sql"], mode=query["mode"])
                timings.append(elapsed)
                result["query"][label]["modes"].append(query["mode"])
                base.verify_query(response, query["oracle"], f"{label} query {index}")
            phases[label] = sum(timings)
            result["query"][label].update(base.summarize(timings), first_request_seconds=timings[0],
                                         all_queries_independent_oracle=True)
            snapshot(label)

        def shutdown(label):
            snapshot("before_" + label)
            started = perf_counter(); session.shutdown(); phases[label] = perf_counter() - started
            result[label + "_data_file_bytes"] = sum(
                path.stat().st_size for path in Path(session.temp.name).rglob("*") if path.is_file())

        try:
            started = perf_counter(); session.start(); phases["startup"] = perf_counter() - started
            phases["create_table"] = request(work["create"])[0]
            phases["ingest"] = 0.0
            for sql, count in work["inserts"]:
                phases["ingest"] += request("BEGIN;")[0]
                phases["ingest"] += request(sql, affected=count)[0]
                phases["ingest"] += request("COMMIT;")[0]
            snapshot("after_ingest")
            phases["index_build"] = request("CREATE VECTOR INDEX docs_v ON docs(v);")[0]
            snapshot("after_index_build")
            if work.get("require_real_spill"):
                files = result["resource_snapshots"][-1].get("open_rebuildable_files", [])
                kinds = {item["kind"] for item in files if item["logical_bytes"] > 0}
                base.require({"payload", "flat_index"} <= kinds, "real payload/index spill was not observed")
                result["real_spill_observed"] = True
            if work.get("require_resident"):
                observed = result["resource_snapshots"][-1]
                base.require("open_rebuildable_files" in observed and not observed["open_rebuildable_files"] and
                             observed.get("process_memory_proxy", {}).get("VmRSS_bytes", 0) > 0,
                             "resident storage could not be established from open files and process memory")
                result["resident_storage_observed"] = True
            query_phase("query_before_mutations", work["before"])
            for index, cycle in enumerate(work["cycles"]):
                label = f"writes_cycle_{index}"
                details = {"cycle": index, "mutations": []}
                result["write_cycles"].append(details)
                details["begin_seconds"] = request("BEGIN;")[0]
                phases[label] = details["begin_seconds"]
                for operation in cycle["operations"]:
                    elapsed = request(operation["sql"], affected=1)[0]
                    details["mutations"].append({"kind": operation["kind"], "pk": operation["pk"], "seconds": elapsed})
                    phases[label] += elapsed
                details["commit_seconds"] = request("COMMIT;")[0]
                phases[label] += details["commit_seconds"]
                snapshot(label)
                query_phase(f"query_cycle_{index}", cycle["queries"])
                details.update(write_and_commit_seconds=phases[label],
                               query_seconds=phases[f"query_cycle_{index}"],
                               first_query_seconds=result["query"][f"query_cycle_{index}"]["first_request_seconds"],
                               cycle_seconds=phases[label] + phases[f"query_cycle_{index}"])
            shutdown("shutdown")
            started = perf_counter(); session.start(); phases["restart"] = perf_counter() - started
            snapshot("after_restart")
            started = perf_counter()
            _, response = request("SELECT id FROM docs ORDER BY id;")
            base.require(response.get("columns") == ["id"] and
                         response.get("rows") == [[pk] for pk in sorted(work["final_model"])],
                         "restart lost, duplicated or reordered rows")
            for pk in work["changed_ids"]:
                _, response = request(f"SELECT v,g FROM docs WHERE id={pk};")
                value = work["final_model"].get(pk)
                expected = [[list(value[0]), value[1]]] if value is not None else []
                base.require(response.get("columns") == ["v", "g"] and response.get("rows") == expected,
                             f"restart changed payload or tombstone {pk}")
                if work.get("verify_second_scalar"):
                    _, response = request(f"SELECT h FROM docs WHERE id={pk};")
                    base.require(response.get("columns") == ["h"] and
                                 response.get("rows") == ([[value[2]]] if value is not None else []),
                                 f"restart changed second scalar {pk}")
            if "padding_value" in work:
                samples = sorted({0, max(work["final_model"]), *work["changed_ids"]})
                for pk in samples:
                    _, response = request(f"SELECT pad FROM docs WHERE id={pk};")
                    base.require(response.get("columns") == ["pad"] and
                                 response.get("rows") == [[work["padding_value"]]],
                                 f"restart changed spilled string payload {pk}")
                result["restarted_padding_ids_checked"] = samples
            phases["restart_verification_wall"] = perf_counter() - started
            query_phase("query_after_restart", work["after"])
            shutdown("final_shutdown")
            phases["measured_lifecycle"] = sum(value for key, value in phases.items()
                                                if key != "restart_verification_wall")
            result["correctness"] = {"all_query_responses_independent_oracle": True,
                                     "all_restarted_ids_checked": len(work["final_model"]),
                                     "all_mutated_payloads_and_tombstones_checked": work["changed_ids"]}
        finally:
            try:
                session.close()
            finally:
                result["stderr"] = base.stderr_tail(log)
                result["response_sha256"] = digest.hexdigest()
                result["full_run_wall_seconds_including_verification"] = perf_counter() - full_started
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("baseline", "candidate", "output"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--scenario", choices=CASES, action="append")
    parser.add_argument("--seeds", type=int, nargs="+", default=[7319, 8911, 13337])
    parser.add_argument("--pairs", type=int, default=1)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dimension", type=int, default=128)
    parser.add_argument("--queries", type=int, default=8)
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--case-timeout", type=float, default=1800)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("requires Linux JSONL transport and /proc")
    if args.rows < 6400 or args.queries < 4 or min(args.pairs, args.dimension, args.batch_size,
                                               args.timeout, args.case_timeout) <= 0:
        parser.error("rows >=6400, queries >=4 and positive remaining sizes/timeouts required")
    if len(set(args.seeds)) != len(args.seeds) or (args.scenario and len(set(args.scenario)) != len(args.scenario)):
        parser.error("duplicate seed/scenario")
    if any(key in os.environ for key in ("MVDB_ANN_PROBE_PERCENT", "MVDB_ANN_OFF")):
        parser.error("unset ANN policy overrides to test default ANN protocol")
    if args.output.exists() or args.output.with_name(args.output.name + ".tmp").exists():
        parser.error("refusing to overwrite measurements")
    for side in ("baseline", "candidate"):
        binary = getattr(args, side).resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"missing/nonexecutable {side}: {binary}")
        setattr(args, side, binary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    cases = args.scenario or CASES
    report = {"kind": "paired_mixed_index_delta_screen", "status": "running", "official_score": None,
              "created_utc": datetime.now(timezone.utc).isoformat(),
              "expected_total_pairs": len(cases) * len(args.seeds) * args.pairs,
              "parameters": vars(args) | {key: str(getattr(args, key)) for key in ("baseline", "candidate", "output")},
              "environment": base.runtime_environment(),
              "host": {"platform": platform.platform(), "cpu_affinity": sorted(os.sched_getaffinity(0))},
              "binaries": {side: {"path": str(getattr(args, side)),
                                  "sha256": hashlib.sha256(getattr(args, side).read_bytes()).hexdigest()}
                           for side in ("baseline", "candidate")},
              "limits": ["Not official scoring; no score inferred.",
                         "Default 9 pairs are screening only; promotion needs repeated confirmation.",
                         "Match compiler/PGO/other flags; compare only the delta switch.",
                         "Exact/default-ANN plus filtered queries use independent exhaustive float32-input oracles.",
                         "Lifecycle sums include ingest, index build, commits, queries, shutdown, restart and final shutdown.",
                         "Oracle/generation/snapshots/restart verification are excluded from measured lifecycle.",
                         "Process RSS/HWM/CPU/IO snapshots are proxies, not cgroup totals; final shutdown resources omitted.",
                         "Shared page cache, no eviction or cgroup limits imposed by this tool."],
              "pairs": []}
    base.write_report(args.output, report)
    pair = None
    try:
        for case_index, name in enumerate(cases):
            for seed_index, seed in enumerate(args.seeds):
                started = perf_counter()
                work = make_workload(name, seed, args.rows, args.dimension, args.queries, args.batch_size)
                generation = perf_counter() - started
                for index in range(args.pairs):
                    order = ["baseline", "candidate"]
                    if (case_index + seed_index + index) % 2:
                        order.reverse()
                    pair = {"scenario": name, "seed": seed, "pair_index": index, "order": order,
                            "workload_sha256": work["sha256"], "mutation_count": work["mutation_count"],
                            "generation_and_oracle_seconds_excluded": generation}
                    for side in order:
                        print(f"{name} seed={seed} pair={index + 1}/{args.pairs} {side}", flush=True)
                        pair[side] = {}
                        one_run(getattr(args, side), work, args, pair[side])
                    base.require(pair["baseline"]["response_sha256"] == pair["candidate"]["response_sha256"],
                                 "paired response mismatch")
                    pair["all_responses_identical"] = True
                    report["pairs"].append(pair)
                    report["summary"] = base.paired_summary(report["pairs"])
                    base.write_report(args.output, report)
        base.require(len(report["pairs"]) == report["expected_total_pairs"], "incomplete pair matrix")
        for side in ("baseline", "candidate"):
            base.require(hashlib.sha256(getattr(args, side).read_bytes()).hexdigest() ==
                         report["binaries"][side]["sha256"], "measured binary changed")
        report.update(status="passed", total_pairs=len(report["pairs"]), binary_unchanged=True)
    except Exception as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}", failed_pair=pair)
        base.write_report(args.output, report)
        raise
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    base.write_report(args.output, report)


if __name__ == "__main__":
    main()
