#!/usr/bin/env python3
"""Actual-binary batch8 correctness/dispatch gate; no performance claim."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import sys

TRACE_PREFIX = "MVDB_BATCH8_TRACE "
TRACE_SCHEMA = "MVDB_BATCH8_TRACE/v1"
BASE_CASES = (
    "unindexed_10", "batch_1", "batch_10", "filter_10", "desc_10",
    "ann_exact_fallback_10", "limit_zero", "ratio_boundary", "unbounded",
)
EXPECTED_CASES = tuple(f"{path}_{name}" for path in ("serial", "fanout") for name in BASE_CASES) + (
    "serial_tail_dimension129", "serial_small7_no_batch", "serial_zero_cap_no_batches",
)
ENVIRONMENT_KEYS = (
    "MVDB_PROTOCOL", "MVDB_OUT", "MVDB_SINGLE_THREAD", "MVDB_QUERY_THREADS",
    "MVDB_QUERY_FANOUT_ROWS", "MVDB_WAKE_US_OVERRIDE", "MVDB_PARSE_THREADS",
    "MVDB_ANN_PROBE_PERCENT", "MVDB_ANN_OFF", "MVDB_BATCH8_TRACE",
)
TRACE_KEYS = {"path", "worker", "first", "last", "batch_begin", "batch_end", "batches",
              "dimension", "k", "row_isa", "batch_isa"}


def require(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()


def load_oracle(repo):
    path = repo / "minivec/tests/topk_bounds.py"
    spec = importlib.util.spec_from_file_location("batch8_existing_topk_oracle", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@contextmanager
def environment(threads):
    saved = {key: os.environ.get(key) for key in ENVIRONMENT_KEYS}
    selected = {"MVDB_QUERY_THREADS": str(threads), "MVDB_PARSE_THREADS": "1",
                "MVDB_ANN_OFF": "1", "MVDB_BATCH8_TRACE": "1"}
    try:
        for key in ENVIRONMENT_KEYS:
            os.environ.pop(key, None)
        os.environ.update(selected)
        yield selected
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def fixture(oracle, count, dimension, *, zero_cap=False):
    oracle.DIMENSION = dimension
    rng = random.Random(0xB87D192)
    keys = rng.sample(range(-500000, 500000), count)
    raw = [[rng.randint(-96, 96) / 64 for _ in range(dimension)] for _ in range(count)]
    if zero_cap:
        raw = [[.5] * dimension for _ in range(count)]
        query = oracle.stored_vector([.5] * dimension)
    else:
        if count > 1:
            raw[1] = raw[0][:]
        # The two equal nearest rows exercise the PK tie rule while the
        # nonzero offset leaves the certified integer cap available.
        query = oracle.stored_vector(value + .125 for value in raw[0])
    rows = [(key, index % 3, oracle.stored_vector(vector))
            for index, (key, vector) in enumerate(zip(keys, raw))]
    records = {key: (category, vector) for key, category, vector in rows}
    require(len(records) == count, "duplicate fixture primary keys")
    if not zero_cap and count > 1:
        ranked = sorted((oracle.canonical_squared(vector, query), key) for key, _, vector in rows)
        require({ranked[0][1], ranked[1][1]} == {keys[0], keys[1]}, "nearest tie fixture invalid")
        require(ranked[0][0] == ranked[1][0] > 0, "tie cap is zero")
    return rows, records, query


def parse_traces(data):
    traces = []
    for line in data.decode("utf-8", errors="strict").splitlines():
        if line.startswith(TRACE_PREFIX):
            def unique(items):
                result = {}
                for key, value in items:
                    require(key not in result, "duplicate trace key")
                    result[key] = value
                return result
            trace = json.loads(line[len(TRACE_PREFIX):], object_pairs_hook=unique)
            require(type(trace) is dict and set(trace) == TRACE_KEYS, "wrong trace field inventory")
            for key in ("worker", "first", "last", "batch_begin", "batch_end", "batches", "dimension", "k"):
                require(type(trace[key]) is int and trace[key] >= 0, f"bad trace integer {key}")
            require(trace["path"] in ("serial", "fanout"), "bad trace path")
            require(trace["row_isa"] == trace["batch_isa"] == "avx2", "unexpected batch/row ISA")
            traces.append(trace)
        elif "MVDB_BATCH8_TRACE" in line:
            raise AssertionError("malformed/interleaved trace line")
    return traces


def verify_traces(traces, *, path, count, dimension, k, expected):
    if expected == "none":
        require(not traces, "excluded query entered a batch branch")
        return
    require(expected in ("positive", "zero"), "invalid expectation")
    expected_spans = [(0, count)] if path == "serial" else [(0, count // 2), (count // 2, count)]
    require(len(traces) == len(expected_spans), "missing/duplicate branch trace; actual binary must exercise AVX2")
    ordered = sorted(traces, key=lambda item: item["worker"])
    for worker, (trace, (first, last)) in enumerate(zip(ordered, expected_spans)):
        require((trace["path"], trace["worker"], trace["first"], trace["last"]) ==
                (path, worker, first, last), "scan partitions differ")
        require(trace["dimension"] == dimension and trace["k"] == k, "query shape differs")
        begin, end, batches = trace["batch_begin"], trace["batch_end"], trace["batches"]
        require(begin == first + k and first <= begin <= end <= last, "bad heap-fill/batch bounds")
        require((end - begin) % 8 == 0 and batches == (end - begin) // 8,
                "batch count not bound to completed loop span")
        if expected == "positive":
            require(batches > 0, "batch kernel was not exercised")
            # This particular fixture has nonzero nearest distance and a
            # usable cap throughout; only the incomplete tail stays scalar.
            require(last - end < 8, "positive fixture unexpectedly fell back before its tail")
        else:
            require(batches == 0 and begin == end, "zero cap unexpectedly ran a batch")


def runtime_identity(session, binary_sha, expected_environment, threads):
    pid = session.proc.pid
    proc = Path("/proc") / str(pid)
    observed = {}
    for item in (proc / "environ").read_bytes().split(b"\0"):
        if b"=" not in item:
            continue
        key, value = item.split(b"=", 1)
        key = key.decode("utf-8", errors="strict")
        if key in ENVIRONMENT_KEYS:
            observed[key] = value.decode("utf-8", errors="strict")
    require(observed == expected_environment, "actual process environment differs")
    executable_sha = sha(proc / "exe")
    require(executable_sha == binary_sha, "actual process executable differs")
    tasks = sorted(int(path.name) for path in (proc / "task").iterdir())
    require(len(tasks) == threads and pid in tasks, "requested parser/query worker topology not present")
    return {"pid": pid, "native_tasks": tasks, "executable_sha256": executable_sha,
            "runtime_environment": observed}


def run_group(oracle, binary, log, checks, *, path, count, dimension, extra=None):
    threads = 2 if path == "fanout" else 1
    rows, records, query = fixture(oracle, count, dimension, zero_cap=extra == "zero")
    config = json.loads((oracle.PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    binary_sha = sha(binary)
    with environment(threads) as selected:
        session = oracle.Session(config, log, command=[str(binary), "--data", "{data_dir}", "--protocol", "jsonl"],
                                 case_timeout=180)
        try:
            session.start()
            oracle.execute(session, f"CREATE TABLE docs(id BIGINT PRIMARY KEY, grp BIGINT, embedding VECTOR({dimension}));", "create")
            oracle.execute(session, "BEGIN;", "begin")
            for start in range(0, count, 64):
                chunk = rows[start:start + 64]
                values = ",".join(f"({key},{category},{vector.sql})" for key, category, vector in chunk)
                oracle.execute(session, f"INSERT INTO docs VALUES {values};", "load", affected=len(chunk))
            oracle.execute(session, "COMMIT;", "commit")
            # No query/index operation above is permitted to emit a scan trace.
            require(not parse_traces(log.read_bytes()), "setup unexpectedly emitted batch trace")
            request_traces = []

            def check(name, k, expected="none", *, group=None, descending=False, mode="exact"):
                offset = log.stat().st_size
                if k is None:
                    response = oracle.execute(session,
                        f"SELECT id FROM docs ORDER BY L2_DISTANCE(embedding,{query.sql}) ASC;", name)
                    require(response.get("columns") == ["id"] and type(response.get("rows")) is list and
                            all(type(row) is list and len(row) == 1 and type(row[0]) is int for row in response["rows"]),
                            "bad unbounded BIGINT rows")
                    actual = [row[0] for row in response["rows"]]
                    want = [key for _, key in sorted((oracle.canonical_squared(vector, query), key)
                                                     for key, (_, vector) in records.items())]
                    require(actual == want, "unbounded exact ranking differs")
                else:
                    actual = oracle.check_topk(session, records, query, name, k=k, group=group,
                                               descending=descending, mode=mode)
                traces = parse_traces(log.read_bytes()[offset:])
                verify_traces(traces, path=path, count=count, dimension=dimension, k=k, expected=expected)
                request_traces.extend(traces)
                identity = runtime_identity(session, binary_sha, selected, threads)
                checks.append({"name": name, "status": "passed", "count": count, "dimension": dimension,
                               "k": k, "mode": mode, "group": group, "descending": descending,
                               "oracle": "existing float32 integer-square/Fraction single binary64 rounding",
                               "result_count": len(actual), "result_sha256": digest(actual),
                               "expected_trace": expected, "traces": traces, "runtime_identity": identity})

            if extra is None:
                check(f"{path}_unindexed_10", 10)
            oracle.execute(session, "CREATE VECTOR INDEX docs_v ON docs(embedding);", "index")
            if extra == "tail":
                check("serial_tail_dimension129", 10, "positive")
            elif extra == "small":
                check("serial_small7_no_batch", 1)
            elif extra == "zero":
                check("serial_zero_cap_no_batches", 1, "zero")
            else:
                check(f"{path}_batch_1", 1, "positive")
                check(f"{path}_batch_10", 10, "positive")
                check(f"{path}_filter_10", 10, group=1)
                check(f"{path}_desc_10", 10, descending=True)
                check(f"{path}_ann_exact_fallback_10", 10, mode="ann")
                check(f"{path}_limit_zero", 0)
                check(f"{path}_ratio_boundary", count // 8)
                check(f"{path}_unbounded", None)
            session.shutdown()  # existing transport asserts exit0/no extra stdout
            require(parse_traces(log.read_bytes()) == request_traces, "unattributed trace outside checked requests")
        finally:
            session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    binary, output, repo = args.binary.resolve(), args.output.resolve(), args.repo.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    logs = output.parent / (output.stem + "-logs")
    logs.mkdir(exist_ok=False)
    report = {"status": "running", "binary_sha256": sha(binary), "trace_schema": TRACE_SCHEMA,
              "expected_cases": list(EXPECTED_CASES), "checks": [], "scope": "actual binary correctness and dispatch; not a benchmark",
              "trace_enabled_only_for_gate": True,
              "oracle_source_sha256": sha(repo / "minivec/tests/topk_bounds.py"),
              "transport_source_sha256": sha(repo / "protocol_check/_transport.py")}
    try:
        require(sys.platform == "linux", "actual executable gate requires Linux /proc")
        oracle = load_oracle(repo)
        for path in ("serial", "fanout"):
            run_group(oracle, binary, logs / f"{path}.stderr.log", report["checks"], path=path, count=1027, dimension=128)
        for extra, count, dimension in (("tail", 1033, 129), ("small", 7, 128), ("zero", 33, 128)):
            run_group(oracle, binary, logs / f"{extra}.stderr.log", report["checks"], path="serial", count=count,
                      dimension=dimension, extra=extra)
        require(tuple(check["name"] for check in report["checks"]) == EXPECTED_CASES, "case inventory/order differs")
        require(sha(binary) == report["binary_sha256"], "executable changed during gate")
        report["status"] = "passed"
        report["checks_count"] = len(report["checks"])
        print(f"PASS actual-image batch8 TopK oracle and dispatch ({len(EXPECTED_CASES)} cases)")
    except Exception as exc:
        report["status"] = "failed"
        report["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        report["stderr_logs"] = [{"path": str(path.relative_to(output.parent)), "bytes": path.stat().st_size,
                                  "sha256": sha(path)} for path in sorted(logs.glob("*.log"))]
        output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
