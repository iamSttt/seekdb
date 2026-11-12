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
        sql = b"CREATE TABLE bad(id BIGINT PRIMARY KEY,s VARCHAR(65535));\n"
        sql += b"INSERT INTO bad VALUES (1,'" + prefix + b"'),(2,'" + malformed[0] + b"'),(3,'after');\n"
        environment = os.environ.copy()
        environment.pop("MVDB_PROTOCOL", None)
        environment["MVDB_OUT"] = "tsv-status"
        seeded = subprocess.run([str(binary), "--data", directory, "--protocol", "sql"], input=sql,
                                capture_output=True, timeout=timeout, env=environment)
        require(seeded.returncode == 0 and b"ERROR" not in seeded.stdout,
                "legacy fixture failed: " + repr(seeded.stdout[-1024:] + seeded.stderr[-1024:]))
        checks = []
        # Other malformed sequences are seeded in separate tables by the same
        # legacy interface, preserving their bytes without invalid JSON input.
        for index, raw in enumerate(malformed[1:], 1):
            sql = f"CREATE TABLE bad{index}(id BIGINT PRIMARY KEY,s VARCHAR(65535));\n".encode()
            sql += f"INSERT INTO bad{index} VALUES (1,'".encode() + prefix + b"'),(2,'" + raw + b"'),(3,'after');\n"
            seeded = subprocess.run([str(binary), "--data", directory, "--protocol", "sql"], input=sql,
                                    capture_output=True, timeout=timeout, env=environment)
            require(seeded.returncode == 0 and b"ERROR" not in seeded.stdout, "malformed fixture seeding failed")
        with launch(binary, data, timeout, "--protocol", "jsonl") as db:
            for index in range(len(malformed)):
                table = "bad" if index == 0 else f"bad{index}"
                # Normal bounded, streaming/unbounded and large-K external-sort
                # response paths must all discard any partially encoded rows.
                for suffix in ("ORDER BY id LIMIT 10", "", "ORDER BY id LIMIT 4096"):
                    db.sequence += 1
                    ident = db.sequence
                    query = f"SELECT id,s FROM {table} {suffix};"
                    db.send_json({"id": ident, "op": "execute", "sql": query, "search_mode": "exact"})
                    error = db.read_json("atomic failure response")
                    db.check_response(error, ident, False)
                    require(set(error) == {"id", "ok", "error"}, "partial success payload leaked into error")
                    require("UTF-8" in error["error"].get("message", ""), "failure did not reach UTF-8 serialization")
                    # A queued partial line or stale success object would break
                    # the next correlated response, even if the first looked OK.
                    db.execute(f"SELECT id,s FROM {table} WHERE id=3;", columns=["id", "s"], rows=[[3, "after"]])
                    db.execute(f"SELECT id FROM {table} ORDER BY id;", columns=["id"], rows=[[1], [2], [3]])
                    checks.append({"malformed_hex": malformed[index].hex(), "path": suffix or "unbounded",
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
              "scope": "18 malformed stored UTF-8 response failures followed by valid requests; not performance"}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
