#!/usr/bin/env python3
"""Exercise the current TSV adapter through a Linux image launch command.

Run with a disposable data directory shared by both launches. This checks
the repository's local protocol and does not use an official driver.
"""

import os
import select
import subprocess
import sys
import time
import uuid
from contextlib import contextmanager


@contextmanager
def session(command):
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as db:
        try:
            yield db
            db.stdin.close()
            assert db.wait(timeout=10) == 0
            assert db.stdout.read() == b"", "unexpected trailing output"
        finally:
            if db.poll() is None:
                db.kill()
                db.wait(timeout=10)


def send(db, sql):
    db.stdin.write(sql.encode("utf-8"))
    db.stdin.flush()


def expect(db, expected):
    received = bytearray()
    deadline = time.monotonic() + 10
    while len(received) < len(expected):
        remaining = max(0, deadline - time.monotonic())
        assert select.select([db.stdout], [], [], remaining)[0], bytes(received)
        data = os.read(db.stdout.fileno(), 65536)
        assert data, f"unexpected EOF: {received!r}"
        received.extend(data)
        assert expected.startswith(received), (expected, received)


def smoke(command):
    table = "image_smoke_" + uuid.uuid4().hex
    ok = b"Query OK, 0 rows affected\n"
    one = b"Query OK, 1 row affected\n"
    with session(command) as db:
        send(db, f"CREATE TABLE {table}(id BIGINT PRIMARY KEY, s VARCHAR(20));\n")
        expect(db, ok)
        send(db, "BEGIN;\n")
        expect(db, ok)
        send(db, f"INSERT INTO {table} VALUES(1,'a;''b'),(2,'keep');\n")
        expect(db, b"Query OK, 2 rows affected\n")
        send(db, "COMMIT;\n")
        expect(db, ok)
        send(db, f"SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, b"id\ts\n1\ta;'b\n2\tkeep\n")

        send(db, f"UPDATE {table} SET s='quote\\")
        time.sleep(0.05)
        send(db, "';and'")
        time.sleep(0.05)
        send(db, "'tail' WHERE id=1; /")
        expect(db, one)
        send(db, "*" + "x" * 140_000 + "; still a comment *")
        time.sleep(0.05)
        send(db, f"/ SELECT id,s FROM {table} WHERE id=1;\n")
        expect(db, b"id\ts\n1\tquote';and'tail\n")

        send(db, f"UPDATE {table} SET s='changed'\n")
        time.sleep(0.05)
        send(db, f"WHERE id=1; /* a comment with ; */ SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, one + b"id\ts\n1\tchanged\n2\tkeep\n")

        send(db, "BEGIN;\n")
        expect(db, ok)
        send(db, f"INSERT INTO {table} VALUES(3,'partial'),(1,'duplicate');\n")
        expect(db, b"ERROR 1062 (HY000): Duplicate entry '1' for key 'PRIMARY'\n")
        send(db, f"DELETE FROM {table} WHERE id=2; SELECT id FROM {table};\n")
        expect(db, one + b"id\n1\n")

    with session(command) as db:
        send(db, f"SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, b"id\ts\n1\tchanged\n2\tkeep\n")

    # Escaped status output preserves column/row boundaries and literal slashes.
    escaped = table + "_escaped"
    with session(command) as db:
        send(db, f"CREATE TABLE {escaped}(id BIGINT PRIMARY KEY,a VARCHAR(32),b VARCHAR(32));\n")
        expect(db, ok)
        send(db, f"INSERT INTO {escaped} VALUES "
             r"(1,'x\t','y'),(2,'x','\ty'),(3,'line\nbreak','slash\\t\0\r'),(4,'','');" + "\n")
        expect(db, b"Query OK, 4 rows affected\n")
        expected = b"id\ta\tb\n1\tx\\t\ty\n2\tx\t\\ty\n3\tline\\nbreak\tslash\\\\t\\0\\r\n4\t\t\n"
        for limit in (" LIMIT 10", " LIMIT 4096", ""):
            send(db, f"SELECT id,a,b FROM {escaped} ORDER BY id{limit};\n")
            expect(db, expected)

    print("PASS image launch, live status, quoted delimiters, fragmented SQL, EOF and restart")
    print("PASS escaped TSV fields across bounded and streaming result paths")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "--":
        raise SystemExit("usage: container_smoke.py -- IMAGE_LAUNCH_COMMAND [ARGS...]")
    smoke(sys.argv[2:])
