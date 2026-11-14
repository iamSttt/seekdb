#!/usr/bin/env python3
"""Independent live checks for MiniVectorDB's default JSON Lines protocol."""

import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
from queue import Empty, Full, Queue
import signal
import subprocess
import sys
import tempfile
from threading import Thread


MAX_RESPONSE_BYTES = 16 * 1024 * 1024
ABOVE_UINT64 = 1 << 64
WIDE_REQUEST_ID = 1 << 100


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def strict_json(line):
    def nonfinite(token):
        raise ValueError(f"nonfinite JSON number {token!r}")

    def finite_float(token):
        value = float(token)
        return value if math.isfinite(value) else nonfinite(token)

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    return json.loads(line.decode("utf-8"), parse_constant=nonfinite,
                      parse_float=finite_float, object_pairs_hook=unique_object)


def check_value(actual, expected, field):
    if type(expected) is float:
        equal = type(actual) in (int, float) and math.isclose(
            actual, expected, rel_tol=1e-5, abs_tol=1e-5)
    else:
        equal = type(actual) is type(expected)
        if equal and type(expected) is list:
            equal = len(actual) == len(expected)
            if equal:
                for index, (value, target) in enumerate(zip(actual, expected)):
                    check_value(value, target, f"{field}[{index}]")
                return
        elif equal:
            equal = actual == expected
    require(equal, f"{field}: got {actual!r}, expected {expected!r}")


def check_error(response):
    error = response.get("error")
    require(type(error) is dict and type(error.get("code")) is str and error["code"],
            f"missing nonempty error.code in {response!r}")


class Session:
    def __init__(self, argv, env, timeout, stderr):
        self.timeout = timeout
        self.sequence = 0
        self.lines = Queue(maxsize=8)
        self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=stderr)
        self.reader = Thread(target=self.read_stdout, daemon=True)
        self.reader.start()

    def read_stdout(self):
        while True:
            line = self.proc.stdout.readline(MAX_RESPONSE_BYTES + 1)
            try:
                self.lines.put_nowait(line)
            except Full:
                return
            if not line or len(line) > MAX_RESPONSE_BYTES:
                return

    def read_line(self, context):
        try:
            return self.lines.get(timeout=self.timeout)
        except Empty as exc:
            raise TimeoutError(f"timed out waiting for {context}") from exc

    def read_json(self, context):
        line = self.read_line(context)
        require(line, f"database exited while waiting for {context}")
        require(len(line) <= MAX_RESPONSE_BYTES and line.endswith(b"\n"),
                f"oversized or unterminated {context}: {line[:240]!r}")
        try:
            response = strict_json(line)
        except (ValueError, UnicodeError) as exc:
            raise AssertionError(f"invalid {context}: {line[:240]!r}") from exc
        require(type(response) is dict, f"{context} is not a JSON object: {response!r}")
        return response

    def send(self, line):
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

    def send_json(self, request, ascii_only=False):
        line = json.dumps(request, ensure_ascii=ascii_only, allow_nan=False) + "\n"
        self.send(line.encode("utf-8"))

    def check_response(self, response, ident, ok):
        check_value(response.get("id"), ident, "response id")
        require(response.get("ok") is ok, f"expected ok={ok}: {response!r}")
        if not ok:
            check_error(response)
        return response

    def execute(self, sql, *, ident=None, mode="exact", ok=True, affected=None,
                columns=None, rows=None, ascii_only=False):
        self.sequence += 1
        if ident is None:
            ident = self.sequence
        self.send_json({"id": ident, "op": "execute", "sql": sql, "search_mode": mode},
                       ascii_only=ascii_only)
        response = self.check_response(self.read_json(f"response to {sql!r}"), ident, ok)
        if affected is not None:
            check_value(response.get("affected_rows"), affected, "affected_rows")
        if columns is not None:
            check_value(response.get("columns"), columns, "columns")
            check_value(response.get("rows"), rows, "rows")

    def expect_unchanged_after_invalid(self, raw, columns, rows):
        encoded = raw.encode("utf-8") if isinstance(raw, str) else raw
        self.send(encoded + b"\n")
        response = self.read_json(f"rejection of invalid frame {raw!r}")
        require(response.get("ok") is False, f"invalid frame succeeded: {response!r}")
        check_error(response)
        ident = "check-after-invalid-frame"
        self.send_json({"id": ident, "op": "execute", "sql": "SELECT * FROM tx ORDER BY id;",
                        "search_mode": "exact"})
        response = self.read_json(f"response after invalid frame {raw!r}")
        self.check_response(response, ident, True)
        check_value(response.get("columns"), columns, "columns after invalid frame")
        check_value(response.get("rows"), rows, "rows after invalid frame")

    def finish(self):
        self.proc.stdin.close()
        require(self.proc.wait(timeout=self.timeout) == 0,
                f"expected a clean exit, got {self.proc.returncode}")
        extra = self.read_line("stdout EOF after shutdown")
        require(extra == b"", f"unexpected stdout after shutdown: {extra[:240]!r}")

    def shutdown(self, ident="shutdown\t测试"):
        self.send_json({"id": ident, "op": "shutdown"})
        self.check_response(self.read_json("shutdown response"), ident, True)
        self.finish()

    def crash(self):
        self.proc.kill()
        status = self.proc.wait(timeout=self.timeout)
        if os.name == "posix":
            require(status == -signal.SIGKILL, f"expected SIGKILL, got {status}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=self.timeout)
        self.reader.join(timeout=self.timeout)
        require(not self.reader.is_alive(), "stdout reader did not stop after process exit")
        self.proc.stdin.close()
        self.proc.stdout.close()


@contextmanager
def launch(binary, data, timeout, *options, protocol=None, ready=True, parse_threads=None):
    env = os.environ.copy()
    env.pop("MVDB_PROTOCOL", None)
    env.pop("MVDB_OUT", None)
    if not ready:
        env["MVDB_OUT"] = "tsv-status"
    if protocol is not None:
        env["MVDB_PROTOCOL"] = protocol
    if parse_threads is not None:
        env["MVDB_PARSE_THREADS"] = str(parse_threads)
    argv = [str(binary), "--data", str(data), *options]
    with tempfile.TemporaryFile() as stderr:
        session = Session(argv, env, timeout, stderr)
        failed = True
        try:
            if ready:
                # No input has been written. Startup must send and flush ready by itself.
                check_value(session.read_json("startup ready"), {"type": "ready"}, "ready")
            yield session
            failed = False
        finally:
            session.close()
            if failed:
                size = stderr.seek(0, os.SEEK_END)
                stderr.seek(max(0, size - 4096))
                print(f"database stderr tail: {stderr.read().decode('utf-8', 'replace')!r}",
                      file=sys.stderr)


def typed_rows(binary, data, timeout):
    low, high = -(1 << 63), (1 << 63) - 1
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE docs(id BIGINT PRIMARY KEY, value BIGINT, "
                   "digits VARCHAR(32), txt VARCHAR(32), v VECTOR(2));",
                   ident='0009007199254740993\t建😀\n"\\\0', ascii_only=True)
        db.execute("INSERT INTO docs VALUES\n"
                   rf"({low},{high},'0009007199254740993','中😀;''文\t\n\r\0\\end" "\u2028"
                   f"','[0.1,-2.5]'),({high},{low},'9223372036854775807','','[0,3.25]');",
                   ident=9007199254740993, affected=2)
        message = "中😀;'文\t\n\r\0\\end\u2028"
        db.execute("SELECT * FROM docs ORDER BY id;", ident=-9007199254740995,
                   columns=["id", "value", "digits", "txt", "v"],
                   rows=[[low, high, "0009007199254740993", message, [0.10000000149011612, -2.5]],
                         [high, low, "9223372036854775807", "", [0.0, 3.25]]])
        db.execute(f"SELECT txt AS body FROM docs WHERE id={low} LIMIT 1;", ident="9007199254740993",
                   columns=["body"], rows=[[message]])
        db.execute("SELECT id, v FROM docs ORDER BY id LIMIT 0;", ident=ABOVE_UINT64,
                   columns=["id", "v"], rows=[])
        db.execute("SELECT id FROM docs ORDER BY id LIMIT 0;", ident=low,
                   columns=["id"], rows=[])
        db.execute("SELECT FROM docs;", ident=WIDE_REQUEST_ID, ok=False)
        db.shutdown(ident=WIDE_REQUEST_ID)


def row_encoding_reuse(binary, data, timeout):
    low, high = -(1 << 63), (1 << 63) - 1
    tiny = float.fromhex("0x1p-149")
    normal = float.fromhex("0x1p-126")
    largest = float.fromhex("0x1.fffffep+127")
    adjacent = float.fromhex("0x1.000002p+0")
    expected = [
        [low, "首行😀\n\t'\\", [-0.0, tiny]],
        [0, "", [adjacent, -normal]],
        [high, "末行", [largest, -largest]],
    ]
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE encoded(id BIGINT PRIMARY KEY,label VARCHAR(80),v VECTOR(2));")
        db.execute(
            f"INSERT INTO encoded VALUES({low},'首行😀\\n\\t''\\\\','[-0,1.401298464324817e-45]'),"
            "(0,'','[1.00000011920928955078125,-1.1754943508222875e-38]'),"
            f"({high},'末行','[3.4028234663852886e38,-3.4028234663852886e38]');",
            affected=3)
        # Both bounded TopK and the external-sort path serialize every row to
        # an owned string before clearing the shared array for the next row.
        for suffix in (" LIMIT 10;", ";", " LIMIT 4096;"):
            ident = "reuse" + suffix
            db.send_json({"id": ident, "op": "execute", "search_mode": "exact",
                          "sql": "SELECT id,label,v,L2_DISTANCE(v,'[0,0]') AS d "
                                 "FROM encoded ORDER BY id" + suffix})
            response = db.check_response(db.read_json("reused row encoding"), ident, True)
            require(response["columns"] == ["id", "label", "v", "d"], "reused columns changed")
            rows = response["rows"]
            require(len(rows) == len(expected), "reused row count changed")
            for row, target in zip(rows, expected):
                require(len(row) == 4 and type(row[0]) is int and row[:3] == target,
                        f"row changed after buffer reuse: {row!r}")
                distance = math.sqrt(math.fsum(x * x for x in target[2]))
                require(type(row[3]) is float and row[3] == distance,
                        "reused finite distance did not round-trip")
            require(type(rows[0][2][0]) is float and math.copysign(1.0, rows[0][2][0]) < 0,
                    "reused vector lost negative zero")
            require(rows[0][2][1] == tiny and rows[2][2][0] == largest,
                    "reused vector changed float32 boundary values")
        db.execute("SELECT label FROM encoded ORDER BY id LIMIT 10;",
                   columns=["label"], rows=[[row[1]] for row in expected])
        db.execute("SELECT v,id FROM encoded LIMIT 0;", columns=["v", "id"], rows=[])
        db.execute("SELECT missing FROM encoded;", ok=False)
        db.execute("SELECT id FROM encoded ORDER BY id DESC LIMIT 10;",
                   columns=["id"], rows=[[high], [0], [low]])
        db.shutdown()


def transactions_and_invalid_input(binary, data, timeout):
    columns = ["id", "value"]
    committed = [[1, "original"], [2, "keep"]]
    query = "SELECT * FROM tx ORDER BY id;"
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE tx(id BIGINT PRIMARY KEY,value VARCHAR(16));")
        db.execute("INSERT INTO tx VALUES(1,'original');", affected=1)
        db.execute("BEGIN;")
        db.execute("INSERT INTO tx VALUES(2,'keep');", ident=WIDE_REQUEST_ID, affected=1)
        db.execute("INSERT INTO tx VALUES(3,'partial'),(1,'duplicate');", ident=ABOVE_UINT64, ok=False)
        db.execute("INSERT INTO tx VALUES(4,'syntax'),;", ok=False)
        # The parser's bounded diagnostic context can end inside a UTF-8 character.
        db.execute("CREATE TABLE t (id 好好好好好好好好好好好好);", ok=False)
        db.execute("INSERT INTO tx VALUES(5,'multi'); DELETE FROM tx WHERE id=1;", ok=False)
        db.execute(query, columns=columns, rows=committed)
        db.execute("COMMIT;")

        request = json.dumps({"id": "bad", "op": "execute", "sql": "INSERT INTO tx VALUES(6,'bad');",
                              "search_mode": "exact"})
        invalid = [request + " trailing garbage",
                   request[:-1] + r',"s\u0071l":"DELETE FROM tx WHERE id=1;"}']
        invalid += [request[:-1] + f',"extra":{token}}}' for token in ("NaN", "Infinity", "-Infinity")]
        for raw in invalid:
            db.expect_unchanged_after_invalid(raw, columns, committed)

        # Invalid UTF-8 must fail before SQL executes. Exercise both the scalar
        # tail and the eight-byte span of the ASCII envelope optimization.
        raw_id_request = json.dumps({"id": "MARKER", "op": "execute",
                                     "sql": "INSERT INTO tx VALUES(6,'bad');"}).encode("utf-8")
        for invalid_id in (b"\xff", b"abcdefg\xff", b"\xc0\xaf", b"\xed\xa0\x80"):
            db.expect_unchanged_after_invalid(
                raw_id_request.replace(b"MARKER", invalid_id), columns, committed)
        # The SQL tokenizer ignores comments, so validating only SQL string
        # values would miss this side-effecting request with a malformed frame.
        raw_sql_request = json.dumps({"id": "invalid-sql-utf8", "op": "execute",
                                      "sql": "INSERT INTO tx VALUES(6,'bad'); -- MARKER"}).encode("utf-8")
        db.expect_unchanged_after_invalid(
            raw_sql_request.replace(b"MARKER", b"\xff"), columns, committed)

        db.execute("BEGIN;")
        db.execute("UPDATE tx SET value='temporary' WHERE id=1;", affected=1)
        db.execute("DELETE FROM tx WHERE id=2;", affected=1)
        db.execute(query, columns=columns, rows=[[1, "temporary"]])
        db.shutdown()

    with launch(binary, data, timeout) as db:
        db.execute(query, columns=columns, rows=committed)
        db.shutdown(ident=ABOVE_UINT64)


def crash_recovery(binary, data, timeout):
    query = "SELECT id FROM crash_rows ORDER BY id;"
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE crash_rows(id BIGINT PRIMARY KEY);")
        db.execute("INSERT INTO crash_rows VALUES(1);", affected=1)
        db.execute("BEGIN;")
        db.execute("INSERT INTO crash_rows VALUES(2);", affected=1)
        db.execute("COMMIT;")
        db.crash()

    with launch(binary, data, timeout) as db:
        db.execute(query, columns=["id"], rows=[[1], [2]])
        db.execute("BEGIN;")
        db.execute("DELETE FROM crash_rows WHERE id=1;", affected=1)
        db.execute("INSERT INTO crash_rows VALUES(3);", affected=1)
        db.execute(query, columns=["id"], rows=[[2], [3]])
        db.crash()

    with launch(binary, data, timeout) as db:
        db.execute(query, columns=["id"], rows=[[1], [2]])
        db.shutdown()


def mode_switch(binary, data, timeout):
    query = ("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM neighbors WHERE g='keep' "
             "ORDER BY d LIMIT 10;")
    columns = ["id", "d"]
    initial = [[2, 1.0], [3, 2.0]]
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE neighbors(id BIGINT PRIMARY KEY,g VARCHAR(8),v VECTOR(2));")
        db.execute("INSERT INTO neighbors VALUES(1,'skip','[0,0]'),"
                   "(2,'keep','[1,0]'),(3,'keep','[0,2]');", affected=3)
        db.execute("CREATE VECTOR INDEX neighbors_v ON neighbors(v);")
        db.execute(query, mode="exact", columns=columns, rows=initial)
        db.execute(query, mode="ann", columns=columns, rows=initial)
        db.execute("BEGIN;")
        db.execute("UPDATE neighbors SET v='[0,0]' WHERE id=3;", affected=1)
        db.execute(query, mode="ann", columns=columns, rows=[[3, 0.0], [2, 1.0]])
        db.execute(query, mode="exact", columns=columns, rows=[[3, 0.0], [2, 1.0]])
        db.execute("ROLLBACK;")
        db.execute(query, mode="ann", columns=columns, rows=initial)
        db.shutdown()


def fragmented_jsonl(binary, data, timeout):
    with launch(binary, data, timeout) as db:
        db.execute("CREATE TABLE fragments(id BIGINT PRIMARY KEY,txt VARCHAR(16));")
        ident = "split\n多字节😀"
        request = {"id": ident, "op": "execute", "search_mode": "exact",
                   "sql": "INSERT INTO fragments VALUES(1,'多字节😀');"}
        encoded = json.dumps(request, ensure_ascii=False, allow_nan=False).encode("utf-8")
        # Each byte is a separate write: this splits UTF-8 sequences and the
        # escaped newline in id, while stdin stays open throughout the exchange.
        for byte in encoded:
            db.send(bytes((byte,)))
        try:
            early = db.lines.get(timeout=min(timeout, 0.05))
        except Empty:
            pass
        else:
            raise AssertionError(f"response before JSONL newline: {early[:240]!r}")
        db.send(b"\n")
        response = db.check_response(db.read_json("fragmented UTF-8 request"), ident, True)
        check_value(response.get("affected_rows"), 1, "fragmented affected_rows")
        db.execute("SELECT * FROM fragments ORDER BY id;", columns=["id", "txt"],
                   rows=[[1, "多字节😀"]])
        db.shutdown()


def parallel_insert_errors(binary, data, timeout):
    with launch(binary, data, timeout, parse_threads=2) as db:
        db.execute("CREATE TABLE parsed(id BIGINT PRIMARY KEY);")
        values = [str(i) for i in range(32)]
        # These conversions fail respectively in the caller and worker halves.
        # Each request must join the other half before emitting its error, and
        # leave the persistent pool usable for the next statement.
        for bad_position in (0, 31):
            bad = values.copy()
            bad[bad_position] = str(1 << 63)
            db.execute("INSERT INTO parsed VALUES" + ",".join(f"({v})" for v in bad) + ";",
                       ok=False)
            db.execute("SELECT id FROM parsed ORDER BY id;", columns=["id"], rows=[])
        db.execute("INSERT INTO parsed VALUES" + ",".join(f"({v})" for v in values) + ";",
                   affected=32)
        db.execute("SELECT id FROM parsed ORDER BY id;", columns=["id"],
                   rows=[[i] for i in range(32)])
        db.shutdown()


def legacy_mode(binary, data, timeout):
    with launch(binary, data, timeout, "--protocol", "sql", ready=False) as db:
        db.send(b"CREATE TABLE legacy(id BIGINT PRIMARY KEY);\n")
        check_value(db.read_line("legacy CREATE status"), b"Query OK, 0 rows affected\n", "status")
        db.send(b"INSERT INTO legacy VALUES(7);\n")
        check_value(db.read_line("legacy INSERT status"), b"Query OK, 1 row affected\n", "status")
        db.finish()

    with launch(binary, data, timeout, protocol="sql", ready=False) as db:
        db.send(b"SELECT id FROM legacy;\n")
        check_value(db.read_line("legacy header"), b"id\n", "legacy header")
        check_value(db.read_line("legacy row"), b"7\n", "legacy row")
        db.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, help="path to the database executable")
    parser.add_argument("--binary", type=Path, help="path to the database executable")
    parser.add_argument("--timeout", type=float, default=10, help="seconds per response or exit")
    args = parser.parse_args()
    if args.path and args.binary:
        parser.error("specify the binary by path or --binary")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    binary = (args.binary or args.path or Path(__file__).resolve().parents[1] / "minivec").resolve()
    with tempfile.TemporaryDirectory(prefix="mvdb-jsonl-") as directory:
        for check in (typed_rows, row_encoding_reuse, transactions_and_invalid_input, crash_recovery, mode_switch,
                      fragmented_jsonl, parallel_insert_errors, legacy_mode):
            data = Path(directory) / check.__name__
            data.mkdir()
            check(binary, data, args.timeout)
            print(f"PASS {check.__name__}", flush=True)


if __name__ == "__main__":
    main()
