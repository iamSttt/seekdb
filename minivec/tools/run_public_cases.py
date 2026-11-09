#!/usr/bin/env python3
"""Check the organizer's public results through MiniVectorDB's CLI.

The organizer's fixture JSON describes SQL results. Check the image's
TSV status output and cross-check query rows through legacy TSV output.
"""

import argparse
import json
import math
import os
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path


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


def run_segment(binary, data, mode, search_mode, statements):
    env = {**os.environ, "MVDB_OUT": mode}
    run = subprocess.run([str(binary), "--data", data, "--mode", search_mode],
                         input="\n".join(sql for _, sql, _ in statements) + "\n",
                         encoding="utf-8", capture_output=True, env=env, timeout=30)
    require(run.returncode == 0, f"exit {run.returncode}: {run.stderr}")
    lines = iter(run.stdout.removesuffix("\n").split("\n") if run.stdout else [])
    escapes = {"t": "\t", "n": "\n", "r": "\r", "0": "\0", "\\": "\\"}
    def fields(line):
        values = line.split("\t")
        if mode == "tsv-status":
            return [re.sub(r"\\([tnr0\\])", lambda m: escapes[m[1]], value) for value in values]
        return values
    for index, statement, result in statements:
        try:
            if "error" in result:
                line = next(lines)
                # The public package requires a failure and nonzero error code.
                error = re.fullmatch(r"ERROR ([1-9]\d*) \([A-Z0-9]+\): .+", line)
                require(error, f"expected {result['error']!r}, got {line!r}")
            elif "columns" in result:
                count = len(result["rows"])
                header = fields(next(lines))
                rows = [fields(next(lines)) for _ in range(count)]
                require(header == result["columns"], f"unexpected column names {header!r}")
                compare_rows(rows, result["rows"], result.get("unordered", False))
            elif mode == "tsv-status":
                line = next(lines)
                ack = re.fullmatch(r"Query OK, (\d+) rows? affected", line)
                require(ack, f"expected successful statement, got {line!r}")
                if "affected_rows" in result:
                    require(int(ack[1]) == result["affected_rows"], f"affected rows mismatch: {line!r}")
        except (StopIteration, ValueError) as exc:
            raise ValueError(f"{mode} statement {index}: {statement}\n{exc or 'missing output'}") from exc
    extra = next(lines, None)
    require(extra is None, f"unexpected trailing {mode} output: {extra!r}")


def run_case(binary, script, expected, mode):
    search_mode = "exact"
    statement_index = 0
    in_transaction = False
    statements = []
    with tempfile.TemporaryDirectory(prefix="mvdb-public-") as data:
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

            if statements:
                run_segment(binary, data, mode, search_mode, statements)
                statements = []
            if line == "--restart":
                in_transaction = False
            elif line in ("--mode exact", "--mode ann"):
                # This executable selects its mode at launch. The public mode
                # directive occurs outside a transaction, so reopening here is safe.
                require(not in_transaction, "cannot switch launch mode during a transaction")
                search_mode = line.split()[1]
            else:
                raise ValueError(f"unsupported driver instruction: {line}")
        if statements:
            run_segment(binary, data, mode, search_mode, statements)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path, help="official mysqltest directory")
    parser.add_argument("--keep-going", action="store_true", help="report all failing cases")
    args = parser.parse_args()
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
            for mode in ("tsv-status", "tsv"):
                run_case(args.binary.resolve(), script, expected, mode)
        except (ValueError, subprocess.TimeoutExpired) as exc:
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


if __name__ == "__main__":
    main()
