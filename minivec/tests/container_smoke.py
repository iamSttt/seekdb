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
    with session(command) as db:
        send(db, f"CREATE TABLE {table}(id BIGINT PRIMARY KEY, s VARCHAR(20));"
                 f"BEGIN; INSERT INTO {table} VALUES(1,'a;''b'),(2,'keep'); COMMIT;"
                 f"SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, b"id\ts\n1\ta;'b\n2\tkeep\n")

        send(db, f"UPDATE {table} SET s='changed'\n")
        time.sleep(0.05)
        send(db, f"WHERE id=1; /* a comment with ; */ SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, b"id\ts\n1\tchanged\n2\tkeep\n")

        send(db, f"BEGIN; DELETE FROM {table} WHERE id=2; SELECT id FROM {table};\n")
        expect(db, b"id\n1\n")

    with session(command) as db:
        send(db, f"SELECT id,s FROM {table} ORDER BY id;\n")
        expect(db, b"id\ts\n1\tchanged\n2\tkeep\n")

    print("PASS image launch, live stdin, quoted delimiters, fragmented SQL, EOF and restart")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "--":
        raise SystemExit("usage: container_smoke.py -- IMAGE_LAUNCH_COMMAND [ARGS...]")
    smoke(sys.argv[2:])
