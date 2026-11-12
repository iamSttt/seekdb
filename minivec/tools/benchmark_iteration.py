#!/usr/bin/env python3
"""Paired Linux JSONL lifecycle measurements for I03/I04/I08 (not official scores).

Example, representative boundary screen:
  python3 minivec/tools/benchmark_iteration.py --baseline /tmp/baseline \
    --candidate /tmp/candidate --suite full --seeds 7319 --pairs 1 \
    --queries 16 --output /tmp/iteration.json

Full defaults use three seeds and five alternating pairs. Workload generation,
oracle construction, resource snapshots and result validation are outside request
timers. Processes share a host and OS page cache; this is not a cold-cache test.
"""

import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import heapq
import json
import math
import os
from pathlib import Path
import platform
import random
from statistics import median
import sys
import tempfile
from time import perf_counter


PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session


FULL_CASES = {
    "resident30k": (30000, 128, 0, 0),
    "resident35k": (35000, 128, 0, 0),
    "resident50k": (50000, 128, 0, 0),
    "update1": (10000, 128, 1, 0),
    "update10": (10000, 128, 10, 0),
    "update100": (10000, 128, 100, 0),
    "low16": (10000, 16, 0, 0),
    "filter60k": (60000, 16, 0, 0, 100),
    "spill": (50000, 128, 0, 512),
    "standard10k": (10000, 128, 0, 0, 10, True),
}
SMOKE_CASES = {
    "resident": (256, 16, 0, 0),
    "update": (512, 16, 1, 0),
    "spill": (2048, 16, 0, 16384),
}
RUNTIME_ENVIRONMENT_KEYS = (
    "MVDB_PROTOCOL", "MVDB_OUT", "MVDB_SINGLE_THREAD", "MVDB_QUERY_THREADS",
    "MVDB_QUERY_FANOUT_ROWS", "MVDB_WAKE_US_OVERRIDE", "MVDB_PARSE_THREADS",
    "MVDB_ANN_PROBE_PERCENT", "MVDB_ANN_OFF",
)


def runtime_environment():
    # Deliberately enumerate behavior knobs; unrelated MVDB_* values may hold
    # secrets or unrelated task data and do not belong in a published report.
    return {key: os.environ[key] for key in RUNTIME_ENVIRONMENT_KEYS if key in os.environ}


def stderr_tail(path, limit=4096):
    with path.open("rb") as stream:
        size = stream.seek(0, 2)
        stream.seek(max(0, size - limit))
        tail = stream.read(limit)
    return {"total_bytes": size, "tail_source_bytes": len(tail), "truncated": size > len(tail),
            "tail_utf8": tail.decode("utf-8", "replace")}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False,
                      sort_keys=True, separators=(",", ":")).encode("utf-8")


def vector_literal(values):
    return "'[" + ",".join(map(str, values)) + "]'"


def squared_distance(a, b):
    return math.fsum((float(x) - float(y)) ** 2 for x, y in zip(a, b))


def make_workload(spec, seed, queries, batch_size, k):
    rows, dimension, updated_rows, padding = spec[:4]
    group_modulus = spec[4] if len(spec) > 4 else 0
    mixed_filters = bool(spec[5]) if len(spec) > 5 else False
    rng = random.Random(seed)
    require(array("f").itemsize == 4, "IEEE binary32 array storage required")
    vectors = [array("f", (rng.gauss(0, 1) for _ in range(dimension)))
               for _ in range(rows)]
    vectors[1] = array("f", vectors[0])
    query_vectors = [array("f", vectors[0])]
    query_vectors.extend(array("f", (rng.gauss(0, 1) for _ in range(dimension)))
                         for _ in range(queries - 1))
    if mixed_filters:
        query_vectors[1] = array("f", vectors[0])
    literals = [vector_literal(v) for v in vectors]
    extra = ",pad VARCHAR(%d)" % padding if padding else ""
    if group_modulus:
        extra += ",grp BIGINT"
    groups = [key % group_modulus for key in range(rows)] if group_modulus else None
    if groups:
        groups[1] = groups[0]  # Preserve the planted PK tie in filtered query 0.
    create = f"CREATE TABLE docs(id BIGINT PRIMARY KEY,v VECTOR({dimension}){extra});"
    pad = ", '" + "x" * padding + "'" if padding else ""
    inserts = []
    digest = hashlib.sha256()
    digest.update(create.encode())
    # Descending IDs make PK tie order independent of insertion order.
    for stop in range(rows, 0, -batch_size):
        start = max(0, stop - batch_size)
        sql = "INSERT INTO docs VALUES " + ",".join(
            f"({key},{literals[key]}{pad}" + (f",{groups[key]})" if groups else ")")
            for key in range(stop - 1, start - 1, -1)) + ";"
        inserts.append((sql, stop - start))
        digest.update(sql.encode())
    query_groups = [((index // 2) % group_modulus if index % 2 else None)
                    if mixed_filters else (index % group_modulus if group_modulus else None)
                    for index in range(queries)]
    sql_queries = [f"SELECT id,L2_DISTANCE(v,{vector_literal(q)}) AS d FROM docs" +
                   (f" WHERE grp={group}" if group is not None else "") +
                   f" ORDER BY d LIMIT {k};" for q, group in zip(query_vectors, query_groups)]
    query_counts = [min(k, groups.count(group) if groups and group is not None else rows)
                    for group in query_groups]
    update = (f"UPDATE docs SET v={vector_literal(query_vectors[1])} "
              f"WHERE id<{updated_rows};") if updated_rows else None
    oracle_indices = sorted({0, 1, queries - 1})

    def oracles(after_update):
        result = {}
        for index in oracle_indices:
            query = query_vectors[index]
            distances = ((squared_distance(query_vectors[1] if after_update and key < updated_rows
                                          else vector, query), key)
                         for key, vector in enumerate(vectors)
                         if groups is None or query_groups[index] is None or groups[key] == query_groups[index])
            result[index] = heapq.nsmallest(k, distances)
        return result

    before = oracles(False)
    after = oracles(True) if updated_rows else before
    for sql in sql_queries + ([update] if update else []):
        digest.update(sql.encode())
    return {"rows": rows, "dimension": dimension, "updated_rows": updated_rows,
            "padding_bytes": padding, "create": create, "inserts": inserts,
            "queries": sql_queries, "update": update, "before_oracle": before,
            "after_oracle": after, "oracle_indices": oracle_indices,
            "vectors": vectors, "updated_vector": query_vectors[1],
            "sha256": digest.hexdigest(), "k": min(k, rows), "query_counts": query_counts,
            "group_modulus": group_modulus, "mixed_filters": mixed_filters}


def process_snapshot(session, phase):
    """Linux process counters, explicitly not cgroup or formal M/B metrics."""
    result = {"phase": phase}
    if session.proc is None:
        result["unavailable"] = "process exited; no final process counters"
        return result
    root = Path("/proc") / str(session.proc.pid)
    try:
        status = {}
        for line in (root / "status").read_text().splitlines():
            key, _, value = line.partition(":")
            if key in ("VmRSS", "VmHWM", "VmSize"):
                status[key + "_bytes"] = int(value.split()[0]) * 1024
        result["process_memory_proxy"] = status
    except (OSError, ValueError) as exc:
        result["memory_unavailable"] = str(exc)
    try:
        result["process_io_counters_proxy"] = {
            key: int(value) for key, value in
            (line.split(":", 1) for line in (root / "io").read_text().splitlines())}
    except (OSError, ValueError) as exc:
        result["io_unavailable"] = str(exc)
    try:
        # Fields after the final ')' start at stat field 3; CPU uses fields 14/15.
        fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
        result["process_cpu_seconds"] = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError) as exc:
        result["cpu_unavailable"] = str(exc)
    try:
        temporary_files = []
        for fd in (root / "fd").iterdir():
            target = os.readlink(fd)
            kind = ("payload" if "/.payload-" in target else
                    "flat_index" if "/.flat-" in target else None)
            if kind:
                temporary_files.append({"kind": kind, "logical_bytes": fd.stat().st_size})
        result["open_rebuildable_files"] = temporary_files
    except OSError as exc:
        result["temporary_files_unavailable"] = str(exc)
    return result


def summarize(values):
    return {"count": len(values), "seconds": math.fsum(values),
            "p50_ms": 1000 * median(values),
            "p95_ms": 1000 * sorted(values)[math.ceil(len(values) * .95) - 1]}


def verify_query(response, expected, label):
    require(response.get("columns") == ["id", "d"], f"{label}: wrong columns")
    rows = response.get("rows")
    require(type(rows) is list and len(rows) == len(expected), f"{label}: wrong row count")
    for row, (distance, key) in zip(rows, expected):
        require(type(row) is list and len(row) == 2 and type(row[0]) is int and row[0] == key,
                f"{label}: TopK oracle mismatch, got {rows}, expected IDs {[x[1] for x in expected]}")
        require(type(row[1]) in (int, float) and math.isfinite(row[1]) and
                math.isclose(row[1], math.sqrt(distance), rel_tol=1e-12, abs_tol=1e-12),
                f"{label}: distance mismatch for {key}")


def one_run(binary, workload, args):
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text())
    config["request_timeout"] = args.timeout
    config["startup_timeout"] = args.timeout
    result = {"phase_seconds": {}, "query": {}, "resource_snapshots": []}
    digest = hashlib.sha256()
    phases = result["phase_seconds"]
    with tempfile.TemporaryDirectory(prefix="mvdb-iteration-log-") as temporary:
        log = Path(temporary) / "stderr.log"
        session = Session(config, log, command=[str(binary), "--data", "{data_dir}",
                                               "--protocol", "jsonl"],
                          case_timeout=args.case_timeout)

        def snapshot(phase):
            result["resource_snapshots"].append(process_snapshot(session, phase))

        def request(sql, affected=None, mode="exact"):
            started = perf_counter()
            response = session.request(sql, mode=mode)
            elapsed = perf_counter() - started
            require(response.get("ok") is True, f"SQL failed: {response.get('error')} ({sql[:120]})")
            if affected is not None:
                require(type(response.get("affected_rows")) is int and
                        response["affected_rows"] == affected, "incorrect affected_rows")
            digest.update(encoded({key: value for key, value in response.items() if key != "id"}))
            return elapsed, response

        def query_phase(label, oracle):
            timings = []
            modes = []
            for index, sql in enumerate(workload["queries"]):
                # Both default modes promise exact results in this iteration.
                mode = "exact" if index % 2 == 0 else "ann"
                elapsed, response = request(sql, mode=mode)
                timings.append(elapsed)
                modes.append(mode)
                require(len(response.get("rows", [])) == workload["query_counts"][index], "wrong TopK size")
                if index in oracle:
                    verify_query(response, oracle[index], f"{label} query {index}")
            phases[label] = math.fsum(timings)
            result["query"][label] = {**summarize(timings), "per_request_seconds": timings,
                                        "modes": modes, "first_request_seconds": timings[0]}
            snapshot(label)

        def shutdown(label):
            snapshot("before_" + label)
            started = perf_counter()
            session.shutdown()
            phases[label] = perf_counter() - started
            result[label + "_data_file_bytes"] = sum(
                path.stat().st_size for path in Path(session.temp.name).rglob("*") if path.is_file())

        try:
            started = perf_counter()
            session.start()
            phases["startup"] = perf_counter() - started
            phases["create_table"] = request(workload["create"])[0]
            ingest = 0.0
            for sql, count in workload["inserts"]:
                ingest += request("BEGIN;")[0]
                ingest += request(sql, affected=count)[0]
                ingest += request("COMMIT;")[0]
            phases["ingest"] = ingest
            snapshot("after_ingest")
            phases["index_build"] = request("CREATE VECTOR INDEX docs_v ON docs(v);")[0]
            snapshot("after_index_build")
            query_phase("query_before_update", workload["before_oracle"])
            if workload["update"]:
                phases["update"] = request(workload["update"], affected=workload["updated_rows"])[0]
                snapshot("after_update")
                query_phase("query_after_update", workload["after_oracle"])
            shutdown("shutdown")
            started = perf_counter()
            session.start()
            phases["restart"] = perf_counter() - started
            snapshot("after_restart")
            started = perf_counter()
            _, response = request("SELECT id FROM docs ORDER BY id;")
            require(response.get("columns") == ["id"] and
                    response.get("rows") == [[i] for i in range(workload["rows"])],
                    "restart lost or reordered rows")
            sample_ids = sorted({0, 1, workload["rows"] // 2, workload["rows"] - 1,
                                 max(0, workload["updated_rows"] - 1)})
            for key in sample_ids:
                _, response = request(f"SELECT v FROM docs WHERE id={key};")
                expected = (workload["updated_vector"] if key < workload["updated_rows"]
                            else workload["vectors"][key])
                require(response.get("columns") == ["v"] and response.get("rows") == [[list(expected)]],
                        f"restart changed vector {key}")
            phases["restart_verification_wall"] = perf_counter() - started
            # Includes lazy index reconstruction in the first measured request.
            query_phase("query_after_restart", workload["after_oracle"])
            shutdown("final_shutdown")
        except Exception:
            print("database stderr tail: " + stderr_tail(log)["tail_utf8"], file=sys.stderr)
            raise
        finally:
            try:
                session.close()
            finally:
                result["stderr"] = stderr_tail(log)
    phases["measured_lifecycle"] = math.fsum(
        value for key, value in phases.items() if key != "restart_verification_wall")
    result["response_sha256"] = digest.hexdigest()
    result["correctness"] = {"sampled_topk_oracle_queries": workload["oracle_indices"],
                             "all_restarted_ids_checked": workload["rows"],
                             "restarted_vector_ids_checked": sample_ids}
    return result


def paired_summary(pairs):
    grouped = {}
    for pair in pairs:
        target = grouped.setdefault(pair["scenario"], {})
        before = pair["baseline"]["phase_seconds"]
        after = pair["candidate"]["phase_seconds"]
        for phase in before:
            ratio = before[phase] / after[phase] if after[phase] > 0 else None
            if ratio is not None:
                target.setdefault(phase, []).append(ratio)
    return {scenario: {phase: {"pairs": len(ratios), "median_baseline_over_candidate": median(ratios),
                              "minimum": min(ratios), "maximum": max(ratios)}
                       for phase, ratios in phases.items()}
            for scenario, phases in grouped.items()}


def write_report(path, report):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("smoke", "full"), default="smoke")
    parser.add_argument("--scenario", action="append", help="run only named scenarios; repeatable")
    parser.add_argument("--seeds", type=int, nargs="+", help="default: smoke=7319; full=7319,8911,13337")
    parser.add_argument("--pairs", type=int, help="default: smoke=1; full=5")
    parser.add_argument("--queries", type=int, help="queries per phase; default: smoke=4; full=32")
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=120, help="per-request/startup timeout seconds")
    parser.add_argument("--case-timeout", type=float, default=1800, help="one binary lifecycle deadline seconds")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this process benchmark requires Linux (official JSONL transport and /proc)")
    args.pairs = args.pairs if args.pairs is not None else (5 if args.suite == "full" else 1)
    args.seeds = args.seeds or ([7319, 8911, 13337] if args.suite == "full" else [7319])
    args.queries = args.queries if args.queries is not None else (32 if args.suite == "full" else 4)
    if min(args.pairs, args.batch_size, args.k) < 1 or args.queries < 2 or min(args.timeout, args.case_timeout) <= 0:
        parser.error("pairs/batch-size/k/timeouts must be positive; queries must be >=2")
    if len(set(args.seeds)) != len(args.seeds):
        parser.error("seeds must be distinct")
    cases = FULL_CASES if args.suite == "full" else SMOKE_CASES
    if args.scenario:
        if any(name not in cases for name in args.scenario):
            parser.error("unknown scenario; choices: " + ", ".join(cases))
        cases = {name: cases[name] for name in args.scenario}
    args.baseline, args.candidate = args.baseline.resolve(), args.candidate.resolve()
    for binary in (args.baseline, args.candidate):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"binary missing or not executable: {binary}")
    if args.output.exists() or args.output.with_name(args.output.name + ".tmp").exists():
        parser.error("output and its .tmp file must not exist")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "kind": "paired_local_iteration_benchmark", "status": "running", "official_score": None,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "suite": args.suite, "seeds": args.seeds, "pairs_per_seed": args.pairs,
        "queries_per_phase": args.queries, "batch_size": args.batch_size, "k": args.k,
        "host": {"platform": platform.platform(), "python": platform.python_version(),
                 "cpu_affinity": sorted(os.sched_getaffinity(0)), "logical_cpus": os.cpu_count()},
        "environment": runtime_environment(),
        "binaries": {label: {"path": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
                     for label, binary in (("baseline", args.baseline), ("candidate", args.candidate))},
        "limits": ["Not formal scoring; no conversion to E or leaderboard points.",
                   "Process VmHWM/RSS and /proc I/O are proxies, not cgroup M/B; no score inferred.",
                   "Process counters sampled before shutdown omit final shutdown I/O and allocations.",
                   "No cgroup resource limits imposed by this tool; record external limits separately.",
                   "Shared host/page cache, no cache eviction; alternating order mitigates but does not remove noise.",
                   "Oracle uses binary32 inputs and Python math.fsum for synthetic Gaussian ranks; numeric edge cases need separate tests.",
                   "Bounded stderr records observed fanout policy diagnostics. CPU affinity or environment settings alone do not establish actual parallel execution.",
                   "Full defaults: 3 seeds x 5 pairs. Smoke/smaller overrides are screening only."],
        "pairs": [],
    }
    write_report(args.output, report)
    pair = None
    try:
        for case_index, (name, spec) in enumerate(cases.items()):
            for seed_index, seed in enumerate(args.seeds):
                started = perf_counter()
                workload = make_workload(spec, seed, args.queries, args.batch_size, args.k)
                generation_seconds = perf_counter() - started
                for pair_index in range(args.pairs):
                    order = ["baseline", "candidate"]
                    if (pair_index + seed_index + case_index) % 2:
                        order.reverse()
                    pair = {"scenario": name, "seed": seed, "pair_index": pair_index,
                            "order": order, "rows": spec[0], "dimension": spec[1],
                            "updated_rows": spec[2], "padding_bytes": spec[3],
                            "group_modulus": workload["group_modulus"],
                            "mixed_filters": workload["mixed_filters"],
                            "workload_sha256": workload["sha256"],
                            "generation_and_oracle_seconds_excluded": generation_seconds}
                    for label in order:
                        print(f"{name} seed={seed} pair={pair_index + 1}/{args.pairs} {label}", flush=True)
                        pair[label] = one_run(getattr(args, label), workload, args)
                    require(pair["baseline"]["response_sha256"] == pair["candidate"]["response_sha256"],
                            f"{name} seed {seed}: baseline/candidate response mismatch")
                    pair["all_responses_identical"] = True
                    report["pairs"].append(pair)
                    report["summary"] = paired_summary(report["pairs"])
                    write_report(args.output, report)
        report["status"] = "passed"
    except Exception as exc:
        report["status"] = "failed"
        report["error"] = f"{type(exc).__name__}: {exc}"
        if pair is not None and not pair.get("all_responses_identical", False):
            report["failed_pair"] = pair
        write_report(args.output, report)
        raise
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    write_report(args.output, report)
    print(json.dumps(report["summary"], indent=2, allow_nan=False))
    print(f"Saved {len(report['pairs'])} verified pairs to {args.output}")


if __name__ == "__main__":
    main()
