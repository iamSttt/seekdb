#!/usr/bin/env python3
"""Check the organizer's public fixtures against MiniVectorDB's local adapter.

Exercise live TSV status responses and cross-check queries in batch legacy TSV.
The public fixtures specify SQL results, not the official process protocol.
"""

import argparse
import json
import math
import os
import re
import select
import subprocess
import tempfile
import time
from collections import Counter
from pathlib import Path


# The current adapter has no end-of-result marker. The public fixture outputs
# are small, so a short quiet interval after stdout arrives delimits a local
# test response. Expected row counts are only used to check the collected data.
RESPONSE_QUIET_SECONDS = 0.025
MAX_LIVE_RESPONSE_BYTES = 8 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def compare_rows(actual, expected, unordered):
    require(len(actual) == len(expected), f"expected {len(expected)} rows, got {len(actual)}")
    if unordered:
        require(all(type(cell) in (int, str) for row in expected for cell in row),
                "unordered floating-point rows are outside the supported fixtures")
        require(Counter(map(tuple, actual)) == Counter(tuple(map(str, row)) for row in expected),
                f"unexpected rows {actual!r}, expected multiset {expected!r}")
        return
    for observed, wanted in zip(actual, expected):
        require(len(observed) == len(wanted), f"row width mismatch: {observed!r}")
        for value, target in zip(observed, wanted):
            equal = (math.isclose(float(value), target, rel_tol=1e-5, abs_tol=1e-5)
                     if isinstance(target, float) else value == str(target))
            require(equal, f"expected {target!r}, got {value!r}")


def fields(line, mode):
    values = line.split("\t")
    if mode != "tsv-status":
        return values
    escapes = {"t": "\t", "n": "\n", "r": "\r", "0": "\0", "\\": "\\"}
    return [re.sub(r"\\([tnr0\\])", lambda m: escapes[m[1]], value) for value in values]


def output_lines(output):
    if not output:
        return iter(())
    require(output.endswith(b"\n"), "stdout is missing the final newline")
    return iter(output[:-1].decode("utf-8").split("\n"))


def check_result(lines, result, mode):
    if "error" in result:
        line = next(lines)
        # The public package requires a failure and nonzero error code.
        error = re.fullmatch(r"ERROR ([1-9]\d*) \([A-Z0-9]+\): .+", line)
        require(error, f"expected {result['error']!r}, got {line!r}")
    elif "columns" in result:
        count = len(result["rows"])
        header = fields(next(lines), mode)
        rows = [fields(next(lines), mode) for _ in range(count)]
        require(header == result["columns"], f"unexpected column names {header!r}")
        compare_rows(rows, result["rows"], result.get("unordered", False))
    elif mode == "tsv-status":
        line = next(lines)
        ack = re.fullmatch(r"Query OK, (\d+) rows? affected", line)
        require(ack, f"expected successful statement, got {line!r}")
        if "affected_rows" in result:
            require(int(ack[1]) == result["affected_rows"], f"affected rows mismatch: {line!r}")


def require_no_extra_lines(lines):
    extra = next(lines, None)
    require(extra is None, f"unexpected trailing output: {extra!r}")


def launch_args(binary, data, mode, search_mode):
    return [str(binary), "--data", data, "--mode", search_mode], {**os.environ, "MVDB_OUT": mode}


def run_batch_segment(binary, data, mode, search_mode, statements, timeout):
    argv, env = launch_args(binary, data, mode, search_mode)
    payload = "\n".join(sql for _, sql, _ in statements) + "\n"
    run = subprocess.run(argv, input=payload.encode("utf-8"),
                         capture_output=True, env=env, timeout=timeout)
    require(run.returncode == 0, f"exit {run.returncode}: {run.stderr.decode(errors='replace')}")
    lines = output_lines(run.stdout)
    for index, statement, result in statements:
        try:
            check_result(lines, result, mode)
        except (StopIteration, ValueError) as exc:
            raise ValueError(f"batch {mode} statement {index}: {statement}\n{exc or 'missing output'}") from exc
    require_no_extra_lines(lines)


def write_statement(process, sql, deadline):
    payload = memoryview((sql + "\n").encode("utf-8"))
    while payload:
        remaining = deadline - time.monotonic()
        require(remaining > 0, "timeout writing SQL to stdin")
        _, writable, _ = select.select([], [process.stdin], [], remaining)
        require(writable, "timeout writing SQL to stdin")
        try:
            written = os.write(process.stdin.fileno(), payload)
        except BlockingIOError:
            continue
        require(written > 0, "database stdin stopped accepting input")
        payload = payload[written:]


def read_live_response(process, deadline):
    response = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        require(remaining > 0, "timeout waiting for stdout with stdin open")
        delay = min(remaining, RESPONSE_QUIET_SECONDS) if response else remaining
        readable, _, _ = select.select([process.stdout], [], [], delay)
        if not readable:
            require(response, "timeout waiting for stdout with stdin open")
            return response
        chunk = os.read(process.stdout.fileno(), 65536)
        require(chunk, "unexpected stdout EOF with stdin still open")
        response.extend(chunk)
        require(len(response) <= MAX_LIVE_RESPONSE_BYTES, "public fixture response exceeds 8 MiB")


def finish_live_segment(process, stderr, timeout, restart):
    require(process.poll() is None, "database exited before shutdown")
    if restart:
        process.terminate()  # SIGTERM on POSIX, with stdin still open.
    else:
        process.stdin.close()
    readable, _, _ = select.select([process.stdout], [], [], timeout)
    require(readable, "timeout waiting for stdout to close during shutdown")
    extra = os.read(process.stdout.fileno(), 65536)
    require(not extra, f"unexpected stdout during shutdown: {extra!r}")
    code = process.wait(timeout=timeout)
    if code != 0:
        size = stderr.seek(0, os.SEEK_END)
        stderr.seek(max(0, size - 4096))
        action = "SIGTERM restart" if restart else "EOF shutdown"
        raise ValueError(f"{action} exited {code}: {stderr.read().decode(errors='replace')}")


def run_live_segment(binary, data, mode, search_mode, statements, timeout, restart):
    argv, env = launch_args(binary, data, mode, search_mode)
    # A file allows stderr diagnostics to be arbitrarily chatty without
    # blocking the process or interfering with stdout.
    with tempfile.TemporaryFile() as stderr, subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=stderr, env=env, bufsize=0) as process:
        try:
            os.set_blocking(process.stdin.fileno(), False)
            for index, statement, result in statements:
                try:
                    deadline = time.monotonic() + timeout
                    write_statement(process, statement, deadline)
                    response = read_live_response(process, deadline)
                    lines = output_lines(response)
                    check_result(lines, result, mode)
                    require_no_extra_lines(lines)
                except (OSError, StopIteration, ValueError) as exc:
                    raise ValueError(f"live {mode} statement {index}: {statement}\n{exc or 'missing output'}") from exc
            finish_live_segment(process, stderr, timeout, restart)
        finally:
            if process.poll() is None:
                process.kill()


def run_case(binary, script, expected, mode, timeout, *, live=False):
    search_mode = "exact"
    statement_index = 0
    in_transaction = False
    statements = []
    with tempfile.TemporaryDirectory(prefix="mvdb-public-") as data:
        def flush_segment(restart=False):
            if not statements:
                return
            if live:
                run_live_segment(binary, data, mode, search_mode, statements, timeout, restart)
            else:
                run_batch_segment(binary, data, mode, search_mode, statements, timeout)
            statements.clear()

        for line in script:
            if not line.startswith("--"):
                result = expected[statement_index]
                statement_index += 1
                statements.append((statement_index, line, result))
                if "error" not in result:
                    command = line.rstrip(";").strip().upper()
                    if command == "BEGIN":
                        in_transaction = True
                    elif command in ("COMMIT", "ROLLBACK"):
                        in_transaction = False
                continue

            flush_segment(restart=line == "--restart")
            if line == "--restart":
                in_transaction = False
            elif line in ("--mode exact", "--mode ann"):
                # This executable selects its mode at launch. The public mode
                # directive occurs outside a transaction, so reopening here is safe.
                require(not in_transaction, "cannot switch launch mode during a transaction")
                search_mode = line.split()[1]
            else:
                raise ValueError(f"unsupported driver instruction: {line}")
        flush_segment()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path, help="official mysqltest directory")
    parser.add_argument("--keep-going", action="store_true", help="report all failing cases")
    parser.add_argument("--timeout", type=float, default=30,
                        help="seconds per live statement, shutdown wait or batch segment")
    parser.add_argument("--batch-status", action="store_true",
                        help="use the former batch runner for status mode too")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a positive finite number")
    if not args.batch_status and os.name != "posix":
        parser.error("live pipe checks require POSIX; use --batch-status for batch checks")
    status_live = not args.batch_status
    binary = args.binary.resolve()
    manifest = json.loads((args.cases / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["version"] in ("minivectordb-public-basic-v1", "minivectordb-public-v4"),
            "unsupported fixture version")
    statements = queries = affected = errors = failures = 0
    for case in manifest["cases"]:
        path = args.cases / "cases" / case["id"]
        test_path = args.cases / case["test"] if "test" in case else path.with_suffix(".test")
        result_path = args.cases / case["result"] if "result" in case else path.with_suffix(".result")
        script = [line.strip() for line in test_path.read_text(encoding="utf-8").splitlines()
                  if line.strip()]
        expected = json.loads(result_path.read_text(encoding="utf-8"))
        sql_count = sum(not line.startswith("--") for line in script)
        require(sql_count == len(expected) == case["statements"], f"fixture count mismatch: {path}")
        try:
            run_case(binary, script, expected, "tsv-status", args.timeout, live=status_live)
            run_case(binary, script, expected, "tsv", args.timeout)
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL {case['id']}\n{exc}")
            if not args.keep_going:
                raise SystemExit(1) from exc
            continue
        statements += sql_count
        queries += sum("columns" in item for item in expected)
        affected += sum("affected_rows" in item for item in expected)
        errors += sum("error" in item for item in expected)
        print(f"PASS {case['id']} ({sql_count} statements)")
    if failures:
        raise SystemExit(f"FAIL {manifest['version']}: {failures} of {len(manifest['cases'])} cases")
    print(f"PASS {manifest['version']}: {len(manifest['cases'])} cases, {statements} statements, "
          f"{queries} queries checked in both output modes, {affected} affected-row assertions, "
          f"{errors} expected errors")
    print(f"Local adapter checks: {'live' if status_live else 'batch'} TSV status and batch legacy TSV.")


if __name__ == "__main__":
    main()
