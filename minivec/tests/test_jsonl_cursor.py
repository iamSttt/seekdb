#!/usr/bin/env python3
"""Actual-binary JSONL cursor regression; writer chunks are not read chunks."""
import argparse
import hashlib
import json
import math
from pathlib import Path
from queue import Empty
import tempfile

from json_protocol import launch, require, check_value


EXPECTED_CASES = (
    "fragment_writer_1", "fragment_writer_7", "fragment_writer_4096",
    "fragment_writer_65536", "large_64k_writer_4096", "large_64k_writer_65536",
    "large_256k_writer_4096", "large_256k_writer_65536",
    "vector_insert_writer_65536", "crlf_split",
    "eof_valid_unterminated_no_execute", "eof_truncated_json_no_execute",
    "duplicate_keys_no_execute", "malformed_frame_no_execute",
    "shutdown_same_write_discards_tail", "escaped_newline",
)
MODEL_SCOPE = "pure framing model; no database execution or actual read-chunk measurement"


def wire(ident, sql=None, op="execute", ending=b"\n"):
    request = {"id": ident, "op": op}
    if sql is not None:
        request.update(sql=sql, search_mode="exact")
    return json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode() + ending


def send_chunks(db, payload, size):
    for offset in range(0, len(payload), size):
        db.send(payload[offset:offset + size])


def response(db, ident, ok=True, rows=None):
    result = db.check_response(db.read_json(str(ident)), ident, ok)
    if rows is not None:
        check_value(result.get("rows"), rows, "rows")
    return result


def no_response(db):
    try:
        unexpected = db.lines.get(timeout=0.05)
    except Empty:
        return
    raise AssertionError(f"response before physical newline: {unexpected!r}")


def create(db):
    db.execute("CREATE TABLE frames(id BIGINT PRIMARY KEY, value VARCHAR(64));")


def contents(db, rows):
    db.execute("SELECT id,value FROM frames ORDER BY id;",
               columns=["id", "value"], rows=rows)


def fragmented(binary, data, timeout, chunk):
    with launch(binary, data, timeout) as db:
        create(db)
        first = wire("first", "INSERT INTO frames VALUES(1,'雪');")
        second = wire("second", "SELECT id,value FROM frames ORDER BY id;")
        tail = wire("tail", "INSERT INTO frames VALUES(2,'tail');", ending=b"")
        payload = first + second + tail
        send_chunks(db, payload, chunk)
        response(db, "first")
        response(db, "second", rows=[[1, "雪"]])
        no_response(db)
        db.send(b"\n")
        response(db, "tail")
        contents(db, [[1, "雪"], [2, "tail"]])
        db.shutdown()
    return {"writer_chunk_bytes": chunk, "payload_bytes": len(payload) + 1,
            "tail_waited_for_newline": True}


def large(binary, data, timeout, padding, chunk):
    with launch(binary, data, timeout) as db:
        create(db)
        sql = "INSERT INTO frames VALUES(1,'large') /*" + "x" * padding + "*/;"
        payload = wire("large", sql)
        send_chunks(db, payload, chunk)
        response(db, "large")
        contents(db, [[1, "large"]])
        db.shutdown()
    return {"writer_chunk_bytes": chunk, "payload_bytes": len(payload),
            "kind": "valid INSERT with block-comment padding"}


def vector_insert(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE vectors(id BIGINT PRIMARY KEY, v VECTOR(128));")
        vector = "[" + ",".join(["0.125000000"] * 128) + "]"
        sql = "INSERT INTO vectors VALUES" + ",".join(
            f"({i},'{vector}')" for i in range(100)) + ";"
        payload = wire("vector", sql)
        send_chunks(db, payload, 65536)
        result = response(db, "vector")
        check_value(result.get("affected_rows"), 100, "inserted rows")
        db.execute("SELECT id FROM vectors ORDER BY id;", columns=["id"],
                   rows=[[i] for i in range(100)])
        db.execute("SELECT v FROM vectors WHERE id=99;", columns=["v"],
                   rows=[[[0.125] * 128]])
        db.shutdown()
    return {"writer_chunk_bytes": 65536, "payload_bytes": len(payload),
            "rows": 100, "dimension": 128}


def crlf(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        create(db)
        payload = wire("crlf", "INSERT INTO frames VALUES(1,'crlf');", ending=b"\r")
        db.send(payload)
        no_response(db)
        db.send(b"\n")
        response(db, "crlf")
        contents(db, [[1, "crlf"]])
        db.shutdown()
    return {"split_between_cr_and_lf": True}


def eof_tail(binary, data, timeout, truncated):
    with launch(binary, data, timeout) as db:
        create(db)
        complete = wire("complete", "INSERT INTO frames VALUES(1,'kept');")
        tail = wire("tail", "INSERT INTO frames VALUES(2,'forbidden');", ending=b"")
        if truncated:
            tail = tail[:-1]  # Missing the JSON object's closing brace as well as newline.
        send_chunks(db, complete + tail, 7)
        response(db, "complete")
        no_response(db)
        db.proc.stdin.close()
        result = response(db, None, ok=False)
        check_value(result["error"]["code"], "invalid_request", "EOF error")
        check_value(result["error"]["message"], "unterminated JSONL request", "EOF message")
        db.finish()
    # Reopening the actual database proves the tail did not mutate persistence.
    with launch(binary, data, timeout) as db:
        contents(db, [[1, "kept"]])
        db.shutdown()
    return {"restart_checked_persistent_rows": True, "tail_executed": False}


def duplicate(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        create(db)
        sql = json.dumps("INSERT INTO frames VALUES(1,'forbidden');")
        for key in ('"sql"', '"\\u0073ql"'):
            raw = ('{"id":"dup","op":"execute","sql":' + sql + ',' + key + ':' + sql + '}\n').encode()
            send_chunks(db, raw, 7)
            result = db.read_json("duplicate key rejection")
            require(result.get("ok") is False and result.get("error", {}).get("code") == "invalid_request",
                    f"duplicate frame accepted: {result!r}")
            contents(db, [])
        db.shutdown()
    return {"duplicate_spellings": ["literal", "escaped-equivalent"], "mutations": 0}


def malformed(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        create(db)
        valid = wire("bad", "INSERT INTO frames VALUES(1,'forbidden');", ending=b"")
        for raw in (valid[:-1] + b"\n", valid + b" garbage\n", b"\n"):
            send_chunks(db, raw, 7)
            result = db.read_json("malformed frame rejection")
            require(result.get("ok") is False and result.get("error", {}).get("code") == "invalid_request",
                    f"malformed frame accepted: {result!r}")
            contents(db, [])
        db.shutdown()
    return {"invalid_shapes": ["missing_brace", "trailing_garbage", "empty_frame"], "mutations": 0}


def shutdown_tail(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        create(db)
        payload = wire("stop", op="shutdown") + wire("tail", "INSERT INTO frames VALUES(1,'forbidden');")
        db.send(payload)
        response(db, "stop")
        # Exit must not wait for EOF, and no response may be emitted for the tail.
        require(db.proc.wait(timeout=timeout) == 0, "shutdown did not exit cleanly")
        db.finish()
    with launch(binary, data, timeout) as db:
        contents(db, [])
        db.shutdown()
    return {"single_writer_send_bytes": len(payload), "restart_checked_persistent_rows": True,
            "read_coalescing_not_assumed": True}


def escaped_newline(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        create(db)
        payload = wire("escaped", "INSERT INTO frames VALUES(1,'a\nb');")
        require(payload.count(b"\n") == 1, "fixture has a non-final physical newline")
        send_chunks(db, payload, 1)
        response(db, "escaped")
        contents(db, [[1, "a\nb"]])
        db.shutdown()
    return {"writer_chunk_bytes": 1, "physical_newlines": 1}


def framing_model(payload, chunk, cursor):
    pending = b""
    searched = scans = calls = 0
    emitted = []
    for offset in range(0, len(payload), chunk):
        pending += payload[offset:offset + chunk]
        begin = 0
        start = searched if cursor else 0
        while True:
            end = pending.find(b"\n", start)
            calls += 1
            scans += (end + 1 if end >= 0 else len(pending)) - start
            if end < 0:
                break
            emitted.append(pending[begin:end])
            begin = end + 1
            start = begin
        pending = pending[begin:]
        searched = len(pending)
    return emitted, pending, scans, calls


def mechanism_model():
    results = []
    cases = [("mixed", b"a\nbb\r\n" + b"x" * 257 + b"\ntail", n) for n in (1, 7, 4096, 65536)]
    cases += [("long", b"x" * (256 * 1024) + b"\n", n) for n in (4096, 65536)]
    for name, payload, chunk in cases:
        old = framing_model(payload, chunk, False)
        new = framing_model(payload, chunk, True)
        require(old[:2] == new[:2], "framing model output mismatch")
        require(new[2] == len(payload), "cursor model rescans or omits bytes")
        require(old[2] >= new[2], "reference model unexpectedly scans less")
        results.append({"name": name, "model_chunk_bytes": chunk, "payload_bytes": len(payload),
                        "reference_scanned_bytes": old[2], "cursor_scanned_bytes": new[2],
                        "reference_find_calls": old[3], "cursor_find_calls": new[3],
                        "emitted_frames": len(new[0]), "tail_bytes": len(new[1])})
    return {"scope": MODEL_SCOPE, "status": "passed", "cases": results,
            "not_a_performance_measurement": True}


def run_case(name, binary, data, timeout):
    if name.startswith("fragment_writer_"):
        return fragmented(binary, data, timeout, int(name.rsplit("_", 1)[1]))
    if name.startswith("large_"):
        _, size, _, chunk = name.split("_")
        return large(binary, data, timeout, int(size[:-1]) * 1024, int(chunk))
    functions = {
        "vector_insert_writer_65536": vector_insert, "crlf_split": crlf,
        "eof_valid_unterminated_no_execute": lambda b, d, t: eof_tail(b, d, t, False),
        "eof_truncated_json_no_execute": lambda b, d, t: eof_tail(b, d, t, True),
        "duplicate_keys_no_execute": duplicate, "malformed_frame_no_execute": malformed,
        "shutdown_same_write_discards_tail": shutdown_tail, "escaped_newline": escaped_newline,
    }
    return functions[name](binary, data, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be finite and positive")
    binary = args.binary.resolve()
    summary = {"status": "running", "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "checks": [], "scope": "actual binary protocol/persistence correctness",
               "fragmentation_scope": "writer chunks; kernel read boundaries are not controlled or measured"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        summary["mechanism_model"] = mechanism_model()
        with tempfile.TemporaryDirectory(prefix="mvdb-cursor-") as directory:
            for name in EXPECTED_CASES:
                data = Path(directory) / name
                data.mkdir()
                check = {"name": name, "status": "running"}
                summary["checks"].append(check)
                try:
                    check.update(run_case(name, binary, data, args.timeout))
                    check["status"] = "passed"
                except Exception as exc:
                    check.update(status="failed", error=f"{type(exc).__name__}: {exc}")
                    raise
                print(f"PASS {name}", flush=True)
        summary["status"] = "passed"
    except Exception as exc:
        summary.update(status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
