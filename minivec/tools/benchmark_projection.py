#!/usr/bin/env python3
"""I14 paired wide-projection screen, with complete independent value oracles.

Timing ends at receipt of the complete JSONL line. JSON parsing and every-element
oracle checks run after that timer. This is transport/engine wall time, not an
isolated serializer microbenchmark and not an official score.

The default suite remains wide768/many16. Explicit wide128 uses the same
1024 rows and 96-codepoint strings as wide768 to screen the guard boundary;
one tested boundary does not establish gains for every dimension above it.
"""
import argparse
from datetime import datetime, timezone
import hashlib
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from json_protocol import Session, require, strict_json

DEFAULT_CASES = ("wide768", "many16")
CASES = {"wide768": (1024, 768, 96), "many16": (8192, 16, 256),
         "wide128": (1024, 128, 96), "many64": (8192, 64, 256),
         "many127": (8192, 127, 256)}
LOW, HIGH = -(1 << 63), (1 << 63) - 1
F32_MAX = float.fromhex("0x1.fffffep127")
F32_TINY = float.fromhex("0x1p-149")
RUNTIME_KEYS = ("MVDB_QUERY_THREADS", "MVDB_PARSE_THREADS", "MVDB_SINGLE_THREAD",
                "MVDB_QUERY_FANOUT_ROWS", "MVDB_WAKE_US_OVERRIDE", "MVDB_ANN_OFF", "MVDB_ANN_PROBE_PERCENT")


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")


def sql_quote(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def vector_sql(vector):
    return sql_quote("[" + ",".join(map(str, vector)) + "]")


def sha(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def save(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n")
    temporary.replace(path)


def workload(name, seed):
    count, dimension, width = CASES[name]
    rng = random.Random(seed)
    rows = []
    for index in range(count):
        key = LOW if index == 0 else HIGH if index == count - 1 else index
        number = (LOW, HIGH, 0, -1, 9007199254740993)[index % 5] if index % 7 else rng.randrange(LOW, HIGH + 1)
        text = (f"row-{index}-中😀é-e\u0301-\u2028-\"\\'\t\n\r\0-" + "中文é😀" * width)[:width]
        vector = [rng.randrange(-2048, 2048) / 16.0 for _ in range(dimension)]
        # Exact float32 endpoints and signed zero also exercise formatting. The
        # distance query has identical first four coordinates, so remaining
        # squared differences are dyadic and sum exactly in binary64.
        vector[:4] = [-0.0, F32_TINY, F32_MAX, -F32_MAX]
        rows.append([key, number, text, vector])
    ranked = sorted(rows, key=lambda row: row[0])
    insert_rows = rows[:]
    rng.shuffle(insert_rows)
    requests = [(f"CREATE TABLE projection(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR({width}),v VECTOR({dimension}));", 0)]
    for start in range(0, count, 64):
        batch = insert_rows[start:start + 64]
        sql = "INSERT INTO projection VALUES " + ",".join(
            f"({key},{number},{sql_quote(text)},{vector_sql(vector)})" for key, number, text, vector in batch) + ";"
        requests.append((sql, len(batch)))
    query = [-0.0, F32_TINY, F32_MAX, -F32_MAX] + [0.0] * (dimension - 4)
    nearest = sorted(rows, key=lambda row: (sum((a - b) ** 2 for a, b in zip(row[3], query)), row[0]))[:10]
    phases = []
    for label, suffix, selected in (("top10", "ORDER BY id LIMIT 10", ranked[:10]),
                                    ("large4096", "ORDER BY id LIMIT 4096", ranked[:4096]),
                                    ("all", "ORDER BY id", ranked)):
        for wide in (False, True):
            columns = ["id", "n", "s", "v"] if wide else ["id"]
            phases.append({"label": ("wide_" if wide else "narrow_") + label,
                "sql": "SELECT " + ",".join(columns) + " FROM projection " + suffix + ";",
                "columns": columns, "rows": selected if wide else [[row[0]] for row in selected]})
    phases.append({"label": "wide_distance10", "sql": "SELECT id,n,s,v FROM projection ORDER BY L2_DISTANCE(v," + vector_sql(query) + ") LIMIT 10;",
                   "columns": ["id", "n", "s", "v"], "rows": nearest})
    return {"name": name, "seed": seed, "rows": count, "dimension": dimension, "string_codepoints": width,
            "requests": requests, "phases": phases}


def verify_value(actual, expected, where):
    if isinstance(expected, list):
        require(type(actual) is list and len(actual) == len(expected), where + ": array shape differs")
        for index, (value, target) in enumerate(zip(actual, expected)):
            verify_value(value, target, where + f"[{index}]")
    elif type(expected) is float:
        require(type(actual) is float and math.isfinite(actual) and actual == expected,
                where + ": float32 roundtrip value differs")
        if expected == 0:
            require(math.copysign(1, actual) == math.copysign(1, expected), where + ": zero sign differs")
    else:
        require(type(actual) is type(expected) and actual == expected, where + ": scalar value/type differs")


def exchange(session, sql, *, expected=None, affected=None):
    session.sequence += 1
    ident = session.sequence
    request = {"id": ident, "op": "execute", "sql": sql, "search_mode": "exact"}
    raw_request = encoded(request) + b"\n"
    started = perf_counter()
    session.send(raw_request)
    raw = session.read_line("projection response")
    seconds = perf_counter() - started
    # Retain an auditable failing frame's size/hash even when strict parsing or
    # the independent oracle rejects it. The bounded prefix is synthetic data.
    session.projection_last_response = {"seconds": seconds, "request_bytes": len(raw_request),
        "response_bytes": len(raw), "response_sha256": hashlib.sha256(raw).hexdigest(),
        "response_prefix": raw[:512].decode("utf-8", "replace")}
    require(raw and raw.endswith(b"\n"), "missing or incomplete response line")
    response = strict_json(raw)
    require(type(response) is dict and response.get("id") == ident and type(response.get("id")) is int and
            response.get("ok") is True, "request failed: " + repr(response)[:512])
    if expected is not None:
        require(set(response) == {"id", "ok", "columns", "rows"}, "unexpected projection envelope")
        verify_value(response["columns"], expected["columns"], "columns")
        verify_value(response["rows"], expected["rows"], "rows")
    else:
        require(set(response) == {"id", "ok", "affected_rows"} and
                type(response["affected_rows"]) is int and response["affected_rows"] == affected,
                "incorrect affected rows")
    return {"seconds": seconds, "request_bytes": len(raw_request), "response_bytes": len(raw),
            "response_sha256": hashlib.sha256(raw).hexdigest()}


def one_run(binary, work, args, result=None):
    environment = os.environ.copy()
    environment.pop("MVDB_OUT", None)
    environment.pop("MVDB_PROTOCOL", None)
    if result is None:
        result = {}
    result.update(status="running", phases={}, lifecycle_seconds={})
    with tempfile.TemporaryDirectory(prefix="mvdb-projection-") as directory:
        log_path = Path(directory) / "stderr.log"
        with log_path.open("wb") as errors:
            start = perf_counter()
            session = Session([str(binary), "--data", directory, "--protocol", "jsonl"], environment, args.timeout, errors)
            try:
                require(session.read_json("ready") == {"type": "ready"}, "invalid ready")
                result["lifecycle_seconds"]["startup"] = perf_counter() - start
                ingestion = []
                for sql, affected in work["requests"]:
                    ingestion.append(exchange(session, sql, affected=affected))
                result["lifecycle_seconds"]["create_and_insert"] = sum(item["seconds"] for item in ingestion)
                result["ingest_request_bytes"] = sum(item["request_bytes"] for item in ingestion)
                for phase in work["phases"]:
                    samples = [exchange(session, phase["sql"], expected=phase) for _ in range(args.repetitions)]
                    result["phases"][phase["label"]] = {"requests": samples, "wall_seconds": sum(item["seconds"] for item in samples),
                        "response_bytes": sum(item["response_bytes"] for item in samples), "rows_per_response": len(phase["rows"]),
                        "columns": phase["columns"], "independent_oracle": "every element, exact integer/string/float32 and signed zero"}
                start = perf_counter()
                session.shutdown()
                result["lifecycle_seconds"]["shutdown"] = perf_counter() - start
                result["status"] = "passed"
            except Exception as error:
                result.update(status="failed", error=f"{type(error).__name__}: {error}",
                              last_response=getattr(session, "projection_last_response", None))
                errors.flush()
                content = log_path.read_bytes()
                result["stderr"] = {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest(),
                                    "tail": content[-4096:].decode("utf-8", "replace")}
                raise
            finally:
                session.close()
        content = log_path.read_bytes()
        result["stderr"] = {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest(),
                            "tail": content[-4096:].decode("utf-8", "replace")}
    return result


def summarize(pairs):
    grouped = {}
    for pair in pairs:
        case = grouped.setdefault(pair["scenario"], {})
        for phase, baseline in pair["baseline"]["phases"].items():
            candidate = pair["candidate"]["phases"][phase]
            case.setdefault(phase, []).append(baseline["wall_seconds"] / candidate["wall_seconds"])
    return {case: {phase: {"pairs": len(ratios), "median_baseline_over_candidate": median(ratios),
                          "min_baseline_over_candidate": min(ratios), "max_baseline_over_candidate": max(ratios)}
                   for phase, ratios in phases.items()} for case, phases in grouped.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", action="append", choices=CASES)
    parser.add_argument("--seeds", type=int, nargs="+", default=[7319, 8911, 13337])
    parser.add_argument("--pairs", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Linux CI is required for comparable timing")
    if args.pairs < 1 or args.repetitions < 1 or args.timeout <= 0 or len(set(args.seeds)) != len(args.seeds):
        parser.error("invalid repetitions/seeds/timeout")
    inputs = args.output.with_suffix(".inputs")
    if args.output.exists() or args.output.with_name(args.output.name + ".tmp").exists() or inputs.exists():
        parser.error("refusing to overwrite evidence")
    args.baseline, args.candidate = args.baseline.resolve(), args.candidate.resolve()
    for binary in (args.baseline, args.candidate):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("executable binary is missing")
    cpus = sorted(os.sched_getaffinity(0))[:2]
    if len(cpus) != 2:
        parser.error("two allowed CPUs are required")
    os.sched_setaffinity(0, cpus)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    inputs.mkdir()
    report = {"status": "running", "kind": "wide_projection_screen", "official_score": None,
        "created_utc": datetime.now(timezone.utc).isoformat(), "pairs": [], "workloads": [],
        "parameters": {"seeds": args.seeds, "pairs_per_seed": args.pairs, "repetitions_per_phase": args.repetitions},
        "host": {"platform": platform.platform(), "affinity": cpus},
        "environment": {key: os.environ[key] for key in RUNTIME_KEYS if key in os.environ},
        "harness_sha256": sha(Path(__file__)),
        "binaries": {label: {"path": str(getattr(args, label)), "sha256": sha(getattr(args, label))} for label in ("baseline", "candidate")},
        "limits": ["Three seeds x one pair is screening only; retain negative results and confirm wins.",
            "Wall time: request write through complete response-line receipt; parsing/oracle are excluded.",
            "Includes transport, projection and ORDER BY work; it is not serializer-only CPU time.",
            "Same host and warm shared cache, fixed phase order, no official memory/IO scoring.",
            "Full projected values are checked independently for every response, not just compared across binaries."]}
    save(args.output, report)
    pair = None
    try:
        for case_index, name in enumerate(args.scenario or DEFAULT_CASES):
            for seed_index, seed in enumerate(args.seeds):
                start = perf_counter()
                work = workload(name, seed)
                input_path = inputs / f"{name}-{seed}.jsonl"
                with input_path.open("wb") as file:
                    for sql, affected in work["requests"]:
                        file.write(encoded({"sql": sql, "affected_rows": affected}) + b"\n")
                    for phase in work["phases"]:
                        file.write(encoded({"sql": phase["sql"], "label": phase["label"], "columns": phase["columns"], "expected_rows": len(phase["rows"])}) + b"\n")
                input_hash = sha(input_path)
                generation = perf_counter() - start
                report["workloads"].append({"scenario": name, "seed": seed, "rows": work["rows"],
                    "dimension": work["dimension"], "string_codepoints": work["string_codepoints"],
                    "input_path": str(input_path), "input_bytes": input_path.stat().st_size,
                    "input_sha256": input_hash, "generation_and_oracle_seconds_excluded": generation})
                save(args.output, report)
                for index in range(args.pairs):
                    order = ["baseline", "candidate"]
                    if (case_index + seed_index + index) % 2:
                        order.reverse()
                    pair = {"scenario": name, "seed": seed, "pair_index": index, "order": order, "input_sha256": input_hash}
                    for side in order:
                        print(f"projection {name} seed={seed} pair={index + 1} {side}", flush=True)
                        pair[side] = {}
                        one_run(getattr(args, side), work, args, pair[side])
                    for label, baseline in pair["baseline"]["phases"].items():
                        candidate = pair["candidate"]["phases"][label]
                        require([item["response_sha256"] for item in baseline["requests"]] ==
                                [item["response_sha256"] for item in candidate["requests"]], "raw response format differs: " + label)
                    pair["all_responses_identical_and_independently_verified"] = True
                    report["pairs"].append(pair)
                    report["summary"] = summarize(report["pairs"])
                    save(args.output, report)
        after = {label: sha(getattr(args, label)) for label in ("baseline", "candidate")}
        require(all(after[label] == report["binaries"][label]["sha256"] for label in after), "binary changed during measurement")
        report["status"] = "passed"
    except Exception as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}", failed_pair=pair)
        save(args.output, report)
        raise
    report["completed_utc"] = datetime.now(timezone.utc).isoformat()
    save(args.output, report)
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
