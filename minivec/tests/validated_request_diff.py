#!/usr/bin/env python3
"""Compare complete JSONL responses and state for reference/candidate binaries.

Usage: python3 validated_request_diff.py --baseline /old/minivec --candidate /new/minivec
Only Python's standard library and the adjacent json_protocol.py are needed.
This is a correctness test, not a performance measurement. --describe needs no binaries.
The one intentional compatibility change is rejecting raw-NUL-truncated JSON
before execution; those differences are counted separately, never hidden.
"""

import argparse
from contextlib import ExitStack, contextmanager
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import tempfile

from json_protocol import Session, strict_json


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def execute(sql, ident="check"):
    return encoded({"id": ident, "op": "execute", "sql": sql})


def object_bytes(pairs):
    return b"{" + b",".join(encoded(key) + b":" + value for key, value in pairs) + b"}"


def cases():
    result = []

    def add(name, raw, invalid=False):
        assert b"\n" not in raw and raw.strip(), name
        result.append((name, raw, invalid))

    fields = [("id", b"-0 \t"), ("op", b'"execute"'),
              ("sql", b'"SHOW TABLES;"'), ("search_mode", b'"exact"')]
    for index, order in enumerate(itertools.permutations(fields)):
        for mode in (b'"exact"', b'"ann"'):
            pairs = [(key, mode if key == "search_mode" else value) for key, value in order]
            add(f"order-{index}-{mode.decode()}", b" \t" + object_bytes(pairs) + b"\r ")
    for ident in (b"null", b"true", b"false", b"0", b"-0", b"-9223372036854775808",
                  b"9223372036854775807", b"9223372036854775808", b"18446744073709551615",
                  b"18446744073709551616", b"-9223372036854775809", b"1267650600228229401496703205376",
                  b"1.000", b"-0.0", b"1e+10", b"[]", b"{}", b'[1,{"a":[true,null]}]',
                  b'"simple"', b'""', b'"escaped\\n\\\""', b'"escaped\\u0000id"', '"中文"'.encode(),
                  b'"\\ud83d\\ude00"'):
        for first in (True, False):
            pairs = [("id", ident + b" \t\r"), ("op", b'"execute"'), ("sql", b'"SHOW TABLES;"')]
            if not first:
                pairs = pairs[1:] + pairs[:1]
            add(f"id-{ident!r}-first-{first}", object_bytes(pairs))
    for sql in ("", " ", "SHOW TABLES;", "SELECT id FROM probe ORDER BY id;",
                "SELECT id FROM probe; SELECT id FROM probe;", "SELECT * FROM missing;",
                "SELECT note FROM probe WHERE note = '中文';", "SELECT 'escaped\\literal';"):
        add("sql-" + repr(sql), execute(sql))
    # Known schema failures must keep validation order and lexical id echo.
    for name in ("id", "op", "sql", "search_mode"):
        for value in (None, True, 12, [], {}, "wrong"):
            request = {"id": "schema", "op": "execute", "sql": "SHOW TABLES;", "search_mode": "exact"}
            request[name] = value
            add(f"type-{name}-{value!r}", encoded(request))
        request = {"id": "schema", "op": "execute", "sql": "SHOW TABLES;", "search_mode": "exact"}
        del request[name]
        add("missing-" + name, encoded(request))
    for value in (b"null", b"true", b"17", b'"text"', b"[]", b"{}"):
        add("nonobject-" + repr(value), value)
    for key in ("unknown", "中", "nested"):
        for first in (True, False):
            pairs = [(key, b'{"v":[1,2]}'), ("id", b"99"), ("op", b'"execute"'), ("sql", b'"SHOW TABLES;"')]
            if not first:
                pairs = pairs[1:] + pairs[:1]
            add(f"unknown-{key}-{first}", object_bytes(pairs))
    common = b'"id":7,"op":"execute","sql":"SHOW TABLES;"'
    for key, value in fields:
        # Include repeated keys before their colon/value is validated: the old
        # fast scanner deliberately throws before that later syntax is read.
        prefix = object_bytes([(key, value), ("unknown", b"0")])[1:-1]
        for first in (encoded(key) + b":" + value, prefix):
            for suffix in (b":" + value, b":", b"x", b":[]", b':"\\ud800"'):
                add(f"duplicate-{key}-{first!r}-{suffix!r}", b"{" + first + b"," + encoded(key) + suffix + b"}")
    for raw in (b'{"id":7,"\\u0069d":8,"op":"execute","sql":"SHOW TABLES;"}',
                b'{"id":{"x":1,"x":2},"op":"execute","sql":"SHOW TABLES;"}',
                b'{"op":"shutdown","id":1,"op":"execute"}',
                b'{"id":1,"op":"shutdown"} trailing',
                b'{"id":1,"op":"shutdown","bad":"\xff"}',
                b'{"id":1,"op":"shutdown",}',
                b'{"id":1,"op":null,"sql":"SHOW TABLES;"}'):
        add("rejection-" + repr(raw), raw, True)
    for raw in (b'{"\\u0069d":7,"op":"execute","sql":"SHOW TABLES;"}',
                b'{"id":7,"\\u006fp":"execute","s\\u0071l":"SHOW TABLES;"}',
                b'{"id":7,"op":"exec\\u0075te","sql":"SHOW TABLES;","search_mode":"ex\\u0061ct"}'):
        add("escaped-valid-" + repr(raw), raw)
    for ident in (b"00", b"-01", b"+1", b"1.", b"1e", b"1e+", b"1e400", b"NaN", b"Infinity"):
        add("bad-number-" + repr(ident), b'{"id":' + ident + b',"op":"execute","sql":"SHOW TABLES;"}', True)
    good = b"{" + common + b"}"
    for end in range(1, len(good)):
        add(f"truncated-{end}", good[:end], True)
    for suffix in (b"garbage", b"{}", b"true", b",", b"\x00", b"\v"):
        add("suffix-" + repr(suffix), good + suffix, True)
    add("nul-hidden-tail", good + b'\x00{"op":"shutdown"}', True)
    for pos in (0, 1, 7, 8, 9, 16):
        for bad in (b"\x00", b"\x01", b"\x1f", b"\x7f\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80"):
            ident = b'"' + b"a" * pos + bad + b'"'
            add(f"bad-string-{pos}-{bad!r}", b'{"id":' + ident + b',"op":"execute","sql":"SHOW TABLES;"}', True)
    # Probe all-or-nothing validation: these would remove the sentinel row if
    # a syntactically valid SQL prefix were executed before envelope validation.
    destructive = execute("DELETE FROM probe;", "atomic")
    # Raw-NUL mutation cases run separately against the candidate below: the
    # reference really executes their valid prefix and would corrupt shared
    # state, so treating them as equivalent rejections would be a false test.
    for suffix in (b"x", b"{}"):
        add("atomic-suffix-" + repr(suffix), destructive + suffix, True)
    add("atomic-late-invalid-utf8", destructive[:-1] + b',"tail":"\xff"}', True)
    add("atomic-late-duplicate", destructive[:-1] + b',"id":8}', True)
    # Large owned SQL then a late fallback. SQL is valid and read-only, so state
    # checks stay meaningful; this also crosses multiple input read boundaries.
    wide = "/*" + "w" * (256 * 1024) + "*/ SHOW TABLES;"
    add("wide-direct", execute(wide))
    for tail in (b',"unknown":0}', ',"未知":0}'.encode(), b',"search_mode":"exact"}', b',"id":2}'):
        add("wide-late-" + repr(tail), execute(wide)[:-1] + tail)
    rng = random.Random(173019)
    for index in range(160):
        mutable = bytearray(good)
        pos = rng.randrange(len(mutable))
        operation = rng.randrange(3)
        value = rng.choice(b'{}[],:"\\0129 x\t\r')
        if operation == 0:
            del mutable[pos]
        elif operation == 1:
            mutable[pos] = value
        else:
            mutable.insert(pos, value)
        add(f"mutated-{index}", bytes(mutable))
    return result


@contextmanager
def start(binary, data, timeout):
    env = dict(os.environ, MVDB_PROTOCOL="jsonl")
    with tempfile.TemporaryFile() as stderr:
        session = Session([binary, "--data", str(data), "--protocol", "jsonl"], env, timeout, stderr)
        try:
            ready = session.read_line("ready")
            assert strict_json(ready) == {"type": "ready"}, ready
            yield session
            session.proc.stdin.close()
            assert session.read_line("EOF") == b"", "unexpected trailing response"
            assert session.proc.wait(timeout=timeout) == 0, "nonzero exit"
        finally:
            if session.proc.poll() is None:
                session.proc.kill()
                session.proc.wait(timeout=timeout)


def compare(left, right, name, raw, invalid=False, stats=None):
    left.send(raw + b"\n")
    right.send(raw + b"\n")
    a, b = left.read_line(name), right.read_line(name)
    if not a or not b:
        raise AssertionError(f"{name}: request={raw[:320]!r}\nreference={a[:1200]!r}\ncandidate={b[:1200]!r}")
    before, response = strict_json(a), strict_json(b)
    if a != b:
        intentional = (stats is not None and b"\x00" in raw and before.get("ok") is True
                       and response.get("id") is None and response.get("ok") is False
                       and response.get("error") == {"code": "invalid_request",
                                                    "message": "raw NUL byte in JSON request"})
        if not intentional:
            raise AssertionError(f"{name}: request={raw[:320]!r}\nreference={a[:1200]!r}\ncandidate={b[:1200]!r}")
        stats["intentional_raw_nul_rejections"] += 1
        stats["intentional_raw_nul_differential_changes"] += 1
    assert isinstance(response, dict), (name, response)
    if invalid:
        assert response.get("ok") is False, (name, response)
    return response


def check_state(left, right, name):
    response = compare(left, right, name, execute("SELECT id FROM probe ORDER BY id;", "state"))
    assert response.get("ok") is True and response.get("rows") == [[1]], (name, response)


def single_request(session, name, raw):
    session.send(raw + b"\n")
    line = session.read_line(name)
    assert line, name
    return strict_json(line)


def check_single_state(session, name):
    response = single_request(session, name, execute("SELECT id FROM probe ORDER BY id;", "state"))
    assert response.get("ok") is True and response.get("rows") == [[1]], (name, response)


def nul_atomicity_frames():
    return [execute("INSERT INTO probe VALUES (2, 'must-not-insert', [2,3]);", "nul-insert") + b"\x00",
            execute("DELETE FROM probe;", "nul-delete") + b"\x00",
            b'{"id":"nul-shutdown","op":"shutdown"}\x00',
            execute("INSERT INTO probe VALUES (3, 'must-not-insert', [3,4]);", "nul-hidden") + b'\x00{"id":9}']


def check_nul_atomicity(candidate, data, timeout, summary):
    with start(candidate, data, timeout) as session:
        check_single_state(session, "nul-start-state")
        for index, raw in enumerate(nul_atomicity_frames()):
            response = single_request(session, f"nul-atomic-{index}", raw)
            assert response == {"id": None, "ok": False,
                                "error": {"code": "invalid_request", "message": "raw NUL byte in JSON request"}}, response
            summary["intentional_raw_nul_rejections"] += 1
            summary["nul_atomicity_cases"] += 1
            # This request also proves an invalid shutdown did not close input.
            check_single_state(session, f"state-after-nul-{index}")
        assert single_request(session, "nul-finish", b'{"id":0,"op":"shutdown"}').get("ok") is True
    with start(candidate, data, timeout) as session:
        check_single_state(session, "nul-restart-state")
        assert single_request(session, "nul-restart-finish", b'{"id":0,"op":"shutdown"}').get("ok") is True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", "--reference", dest="reference")
    parser.add_argument("--candidate")
    parser.add_argument("--output", help="write the successful test summary as JSON")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--expected-raw-nul-changes", type=int, choices=(0, 2), default=2,
                        help="2 for original EE; 0 for a parent that already rejects raw NUL")
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()
    inventory = cases()
    fingerprint = hashlib.sha256()
    for name, raw, invalid in inventory:
        fingerprint.update(len(raw).to_bytes(8, "little") + raw + bytes([invalid]))
    summary = {"cases": len(inventory), "sha256": fingerprint.hexdigest(),
               "intentional_raw_nul_rejections": 0, "intentional_raw_nul_differential_changes": 0,
               "nul_atomicity_cases": 0}
    summary["nul_atomicity_sha256"] = hashlib.sha256(b"".join(
        len(raw).to_bytes(8, "little") + raw for raw in nul_atomicity_frames())).hexdigest()
    if args.describe:
        print(json.dumps(summary))
        return
    if not args.reference or not args.candidate:
        parser.error("--baseline and --candidate are required unless --describe is used")
    reference, candidate = str(Path(args.reference).resolve()), str(Path(args.candidate).resolve())
    with tempfile.TemporaryDirectory(prefix="mvdb-envelope-diff-") as temp:
        directories = [Path(temp) / "reference", Path(temp) / "candidate"]
        with ExitStack() as stack:
            left = stack.enter_context(start(reference, directories[0], args.timeout))
            right = stack.enter_context(start(candidate, directories[1], args.timeout))
            for sql in ("CREATE TABLE probe(id BIGINT PRIMARY KEY, note VARCHAR(8192), v VECTOR(2));",
                        "INSERT INTO probe VALUES (1, 'sentinel', [1,2]);"):
                assert compare(left, right, "setup", execute(sql)).get("ok") is True
            for name, raw, invalid in inventory:
                compare(left, right, name, raw, invalid, summary)
                check_state(left, right, "state-after-" + name)
            assert compare(left, right, "shutdown", b'{"id":0,"op":"shutdown"}').get("ok") is True
        # Accepted shutdown ignores sql/mode values but still validates all JSON.
        # Also verify an open transaction is rolled back on these fallback paths.
        shutdowns = [b'{"id":1,"op":"shutdown","sql":null,"search_mode":17}',
                     b'{"id":18446744073709551616,"op":"shutdown","extra":{"x":[]}}',
                     b'{"id":"\\u0061","op":"shut\\u0064own"}']
        for index, shutdown in enumerate(shutdowns):
            with ExitStack() as stack:
                left = stack.enter_context(start(reference, directories[0], args.timeout))
                right = stack.enter_context(start(candidate, directories[1], args.timeout))
                check_state(left, right, f"restart-{index}")
                for sql in ("BEGIN;", "INSERT INTO probe VALUES (2, 'rollback', [2,3]);"):
                    assert compare(left, right, "txn", execute(sql)).get("ok") is True
                assert compare(left, right, "shutdown-fallback", shutdown).get("ok") is True
        with ExitStack() as stack:
            left = stack.enter_context(start(reference, directories[0], args.timeout))
            right = stack.enter_context(start(candidate, directories[1], args.timeout))
            check_state(left, right, "final-recovery")
            compare(left, right, "final-shutdown", b'{"id":0,"op":"shutdown"}')
        check_nul_atomicity(candidate, directories[1], args.timeout, summary)
    assert summary["intentional_raw_nul_differential_changes"] == args.expected_raw_nul_changes, summary
    assert summary["nul_atomicity_cases"] == 4, summary
    summary["result"] = "pass"
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
