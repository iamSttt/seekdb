#!/usr/bin/env python3
"""Exercise disk-backed rows/indexes through the CLI under RLIMIT_AS on Linux."""

import argparse
import math
import os
import resource
import select
import subprocess
import tempfile
import time
from pathlib import Path


def main(binary):
    rows = 50_000
    dimension = 128
    model = {i: (i % 7, [i % 97, i % 89, i % 83, i % 79]) for i in range(rows)}

    def vector(values):
        return "'[" + ",".join(map(str, values + [0] * (dimension - 4))) + "]'"

    def cap():
        resource.setrlimit(resource.RLIMIT_AS, (160 << 20, 160 << 20))

    with tempfile.TemporaryDirectory(prefix="mvdb-spilled-index-") as directory:
        def start(mode):
            process = subprocess.Popen(
                [binary, "--data", directory, "--mode", mode], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, preexec_fn=cap,
                env={**os.environ, "MVDB_OUT": "tsv-status"})
            return process, bytearray()

        process, pending = start("exact")

        def line():
            deadline = time.monotonic() + 30
            while b"\n" not in pending:
                timeout = max(0, deadline - time.monotonic())
                assert select.select([process.stdout], [], [], timeout)[0], "response timeout"
                chunk = os.read(process.stdout.fileno(), 65536)
                assert chunk, f"unexpected process exit {process.poll()}"
                pending.extend(chunk)
            end = pending.index(b"\n")
            result = bytes(pending[:end]).decode()
            del pending[:end + 1]
            return result

        def send(sql):
            process.stdin.write((sql + ";\n").encode())
            process.stdin.flush()

        def mutate(sql, count=0):
            send(sql)
            expected = f"Query OK, {count} row{'s' if count != 1 else ''} affected"
            assert line() == expected, sql[:120]

        def queries():
            for group in (None, 3, 99):
                for q in ([0, 0, 0, 0], [25, 31, 37, 41]):
                    eligible = [key for key, (g, _) in model.items() if group is None or g == group]
                    distance = lambda key: sum((a-b)**2 for a,b in zip(model[key][1], q))
                    expected = sorted(eligible, key=lambda key: (distance(key), key))[:10]
                    where = "" if group is None else f" WHERE g={group}"
                    send(f"SELECT id,L2_DISTANCE(v,{vector(q)}) AS d FROM t{where} ORDER BY d LIMIT 10")
                    assert line() == "id\td"
                    for key in expected:
                        found, value = line().split("\t")
                        assert int(found) == key, (found, key, group, q)
                        assert math.isclose(float(value), math.sqrt(distance(key)), rel_tol=1e-12, abs_tol=1e-12)

        try:
            mutate("CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(512),v VECTOR(128))")
            payload = "x" * 512
            for base in range(0, rows, 100):
                values = [f"({key},{model[key][0]},'{payload}',{vector(model[key][1])})"
                          for key in range(base, base + 100)]
                mutate("INSERT INTO t VALUES " + ",".join(values), 100)
            mutate("CREATE VECTOR INDEX t_v ON t(v)")
            files = [os.readlink(p) for p in (Path("/proc") / str(process.pid) / "fd").iterdir()]
            assert any("/.payload-" in path for path in files), files
            assert any("/.flat-" in path for path in files), files
            queries()
            original = model.copy()
            mutate("BEGIN")
            mutate(f"UPDATE t SET v={vector([0,0,0,0])} WHERE id=7", 1)
            model[7] = (0, [0,0,0,0])
            mutate("DELETE FROM t WHERE id>=49995", 5)
            for key in range(49995, rows):
                del model[key]
            queries()
            mutate("ROLLBACK")
            model = original
            queries()
            mutate("BEGIN")
            mutate(f"UPDATE t SET v={vector([0,0,0,0])} WHERE id=11", 1)
            model[11] = (11 % 7, [0,0,0,0])
            send(f"INSERT INTO t VALUES(50001,1,'new',{vector([0,0,0,0])}),(10,1,'duplicate',{vector([0,0,0,0])})")
            assert line().startswith("ERROR 1062 ")
            mutate("COMMIT")
            mutate("BEGIN")
            mutate("DELETE FROM t WHERE id=0", 1)
            process.kill()
            process.wait(timeout=10)
            process.stdin.close()
            process.stdout.close()
            process, pending = start("ann")
            queries()
            send("SELECT id FROM t WHERE id=50001")
            assert line() == "id"
            mutate("BEGIN")
            mutate("DELETE FROM t WHERE id=11", 1)
            process.terminate()
            assert process.wait(timeout=30) == 0
            assert not pending and process.stdout.read() == b""
            process.stdin.close()
            process.stdout.close()
            process, pending = start("ann")
            queries()
            process.stdin.close()
            assert process.wait(timeout=30) == 0
            assert not pending and process.stdout.read() == b""
            print("PASS 50000 rows, 128d disk index, 160MiB address limit, filtered TopK, "
                  "index mutations/rollback, failed insert, acknowledged commit after SIGKILL, "
                  "SIGTERM rollback and ann restart")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    main(str(Path(args.binary).resolve()))
