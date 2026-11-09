#!/usr/bin/env python3
"""Check the organizer's public-basic-v1 results through MiniVectorDB's CLI.

The organizer's fixture JSON describes SQL results. This runner uses the
existing table output for statement success and affected rows, and checks
queries separately through the default TSV output.
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


def table_cells(line):
    require(line.startswith("|") and line.endswith("|"), f"expected table row, got {line!r}")
    # Public v1 has no cells containing delimiters or significant padding.
    return [cell.strip(" ") for cell in line[1:-1].split("|")]


def compare_rows(actual, expected, unordered):
    require(len(actual) == len(expected), f"expected {len(expected)} rows, got {len(actual)}")
    if unordered:
        require(all(type(cell) in (int, str) for row in expected for cell in row),
                "unordered floating-point rows are outside public v1")
        require(Counter(map(tuple, actual)) == Counter(tuple(map(str, row)) for row in expected),
                f"unexpected rows {actual!r}, expected multiset {expected!r}")
        return
    for observed, wanted in zip(actual, expected):
        require(len(observed) == len(wanted), f"row width mismatch: {observed!r}")
        for value, target in zip(observed, wanted):
            equal = (math.isclose(float(value), target, rel_tol=1e-5, abs_tol=1e-5)
                     if isinstance(target, float) else value == str(target))
            require(equal, f"expected {target!r}, got {value!r}")


def run_case(binary, sql, expected, mode):
    env = dict(os.environ)
    env.pop("MVDB_OUT", None)
    if mode == "table":
        env["MVDB_OUT"] = "table"
    with tempfile.TemporaryDirectory(prefix="mvdb-public-") as data:
        run = subprocess.run([str(binary), "--data", data], input="\n".join(sql) + "\n",
                             encoding="utf-8", capture_output=True, env=env, timeout=30)
    require(run.returncode == 0, f"exit {run.returncode}: {run.stderr}")
    lines = iter(run.stdout.splitlines())
    for index, (statement, result) in enumerate(zip(sql, expected), start=1):
        try:
            if "columns" in result:
                count = len(result["rows"])
                if mode == "table":
                    border = next(lines)
                    require(re.fullmatch(r"(?:\+-+)+\+", border), f"expected table, got {border!r}")
                    header = table_cells(next(lines))
                    require(next(lines) == border, "missing header border")
                    rows = [table_cells(next(lines)) for _ in range(count)]
                    require(next(lines) == border, "extra result row or missing closing border")
                    require(next(lines) == f"{count} {'row' if count == 1 else 'rows'} in set",
                            "incorrect result row count")
                else:
                    header = next(lines).split("\t")
                    rows = [next(lines).split("\t") for _ in range(count)]
                require(header == result["columns"], f"unexpected column names {header!r}")
                compare_rows(rows, result["rows"], result.get("unordered", False))
            elif mode == "table":
                line = next(lines)
                ack = re.fullmatch(r"Query OK, (\d+) rows? affected", line)
                require(ack, f"expected successful statement, got {line!r}")
                if "affected_rows" in result:
                    require(int(ack[1]) == result["affected_rows"], f"affected rows mismatch: {line!r}")
        except (StopIteration, ValueError) as exc:
            raise ValueError(f"{mode} statement {index}: {statement}\n{exc or 'missing output'}") from exc
    extra = next(lines, None)
    require(extra is None, f"unexpected trailing {mode} output: {extra!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path, help="official mysqltest directory")
    args = parser.parse_args()
    manifest = json.loads((args.cases / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["version"] == "minivectordb-public-basic-v1", "unsupported fixture version")
    statements = queries = affected = 0
    for case in manifest["cases"]:
        path = args.cases / "cases" / case["id"]
        sql = [line for line in path.with_suffix(".test").read_text(encoding="utf-8").splitlines()
               if line.strip()]
        expected = json.loads(path.with_suffix(".result").read_text(encoding="utf-8"))
        require(len(sql) == len(expected) == case["statements"], f"fixture count mismatch: {path}")
        for mode in ("table", "tsv"):
            run_case(args.binary.resolve(), sql, expected, mode)
        statements += len(sql)
        queries += sum("columns" in item for item in expected)
        affected += sum("affected_rows" in item for item in expected)
        print(f"PASS {case['id']} ({len(sql)} statements)")
    print(f"PASS {manifest['version']}: {len(manifest['cases'])} cases, {statements} statements, "
          f"{queries} queries checked in both modes, {affected} affected-row assertions")


if __name__ == "__main__":
    main()
