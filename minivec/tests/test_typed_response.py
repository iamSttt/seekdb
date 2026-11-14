#!/usr/bin/env python3
"""Live JSONL error atomicity after a stored malformed UTF-8 value.

The legacy SQL interface intentionally permits arbitrary string bytes. Seed the
fixture there, then exercise the production JSONL serializer after reopening.
No durable-file editing or malformed JSON request can bypass the target path.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from json_protocol import launch, require


def run(binary, timeout):
    malformed = [b"\xff", b"\xc0\x80", b"\xed\xa0\x80", b"\xe4\xb8", b"\xf4\x90\x80\x80", b"\x80"]
    with tempfile.TemporaryDirectory(prefix="mvdb-typed-response-") as directory:
        data = Path(directory)
        # The first projected row is longer than 64 KiB. Each failed SELECT
        # reaches serialization of a later row, after a valid output prefix.
        prefix = "中".encode("utf-8") * 30000
        environment = os.environ.copy()
        environment.pop("MVDB_PROTOCOL", None)
        environment["MVDB_OUT"] = "tsv-status"
        checks = []
        fixtures = []
        # Keep all original scalar cases. 127d falls back to DOM under the guard,
        # while projecting 128d selects typed output, including the malformed row.
        for dimension in (None, 127, 128):
            vector = ("[" + ",".join(["0"] * dimension) + "]").encode() if dimension else None
            for index, raw in enumerate(malformed):
                base = "bad" if dimension is None else f"badvec{dimension}_"
                table = base if index == 0 else f"{base}{index}"
                schema = f",v VECTOR({dimension})" if dimension else ""
                cell = b"," + vector if dimension else b""
                sql = f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,s VARCHAR(65535){schema});\n".encode()
                sql += f"INSERT INTO {table} VALUES (1,'".encode() + prefix + b"'" + cell
                sql += b"),(2,'" + raw + b"'" + cell + b"),(3,'after'" + cell + b");\n"
                seeded = subprocess.run([str(binary), "--data", directory, "--protocol", "sql"], input=sql,
                                        capture_output=True, timeout=timeout, env=environment)
                require(seeded.returncode == 0 and b"ERROR" not in seeded.stdout,
                        "legacy fixture failed: " + repr(seeded.stdout[-1024:] + seeded.stderr[-1024:]))
                fixtures.append((table, dimension, raw))
        with launch(binary, data, timeout, "--protocol", "jsonl") as db:
            for table, dimension, raw in fixtures:
                columns = ["id", "s", "v"] if dimension else ["id", "s"]
                projection = ",".join(columns)
                # Normal bounded, streaming/unbounded and large-K external-sort
                # response paths must all discard any partially encoded rows.
                for suffix in ("ORDER BY id LIMIT 10", "", "ORDER BY id LIMIT 4096"):
                    db.sequence += 1
                    ident = db.sequence
                    query = f"SELECT {projection} FROM {table} {suffix};"
                    db.send_json({"id": ident, "op": "execute", "sql": query, "search_mode": "exact"})
                    error = db.read_json("atomic failure response")
                    db.check_response(error, ident, False)
                    require(set(error) == {"id", "ok", "error"}, "partial success payload leaked into error")
                    require("UTF-8" in error["error"].get("message", ""), "failure did not reach UTF-8 serialization")
                    # A queued partial line or stale success object would break
                    # the next correlated response, even if the first looked OK.
                    row = [3, "after"] + ([[0.0] * dimension] if dimension else [])
                    db.execute(f"SELECT {projection} FROM {table} WHERE id=3;", columns=columns, rows=[row])
                    db.execute(f"SELECT id FROM {table} ORDER BY id;", columns=["id"], rows=[[1], [2], [3]])
                    checks.append({"malformed_hex": raw.hex(), "path": suffix or "unbounded",
                                   "projected_vector_dimension": dimension,
                                   "valid_prefix_bytes": len(prefix), "status": "passed"})
            db.shutdown()
        return checks


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error("refusing to overwrite evidence")
    checks = run(args.binary.resolve(), args.timeout)
    result = {"status": "passed", "checks": checks,
              "scope": "54 scalar/127d/128d stored UTF-8 response failures followed by valid requests; not performance"}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
