#!/usr/bin/env python3
"""Compare real JSONL binaries and independently verify stored float32 bits.

No decimal -> binary64 -> binary32 conversion is used for expected values.
Use --baseline OLD_ELF --candidate NEW_ELF --output report.json.
This test verifies SQL-visible behavior, not internal fast-path execution or speed.
"""

import argparse
from contextlib import contextmanager
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import tempfile

from json_protocol import Session


DIMENSION = 128
F32 = struct.Struct("<f")
U32 = struct.Struct("<I")
HEX_BITS = {"0x1p-1": 0x3f000000, "-0x1p-1": 0xbf000000,
            "0x1.8p+1": 0x40400000}
KNOWN_DOUBLE_ROUNDING = {
    "0.98356494307518": 0x3f7bcae9,
    "-0.98356494307518": 0xbf7bcae9,
    "3.05607807636261": 0x404396c9,
    "-3.05607807636261": 0xc04396c9,
    # v2: d is one binary64 grid position off the midpoint. Equality-only
    # rejection fails; the second mantissa is larger than 2^63.
    "37.692861557006836": 0x4216c57e,
    "-37.692861557006836": 0xc216c57e,
    "927745.5312499999999": 0x49628018,
    "-927745.5312499999999": 0xc9628018,
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def rational_binary32(value, negative_zero=False):
    """Exact rational rounding with integer quotient/remainder, ties to even."""
    sign = value < 0 or negative_zero
    value = abs(value)
    if not value:
        return int(sign) << 31
    numerator, denominator = value.numerator, value.denominator
    exponent = numerator.bit_length() - denominator.bit_length()
    below_binade = (numerator < denominator << exponent) if exponent >= 0 else (
        numerator << -exponent < denominator)
    if below_binade:
        exponent -= 1
    shift = 23 - max(exponent, -126)
    if shift >= 0:
        numerator <<= shift
    else:
        denominator <<= -shift
    mantissa, remainder = divmod(numerator, denominator)
    if 2 * remainder > denominator or (2 * remainder == denominator and mantissa & 1):
        mantissa += 1
    if exponent < -126:
        magnitude = mantissa
    else:
        if mantissa == 1 << 24:
            exponent += 1
            mantissa >>= 1
        magnitude = 0x7f800000 if exponent > 127 else (
            ((exponent + 127) << 23) + mantissa - (1 << 23))
    return magnitude | (int(sign) << 31)


def token_bits(token):
    if token in HEX_BITS:
        return HEX_BITS[token]
    value = Fraction(token)
    return rational_binary32(value, token.startswith("-") and value == 0)


def received_bits(value):
    # Decode an engine result, not an oracle. Its binary32 value is emitted as
    # an exact binary64 JSON number; packing recovers that stored bit pattern.
    require(type(value) in (int, float) and math.isfinite(value),
            f"invalid returned vector component {value!r}")
    return U32.unpack(F32.pack(value))[0]


def canonical(response):
    # Unlike Python numeric equality, this also distinguishes -0.0 from 0.0,
    # float from integer JSON values, and all response metadata/error fields.
    return json.dumps(response, ensure_ascii=False, allow_nan=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def vector_sql(tokens, quoted=True):
    body = "[" + ",".join(tokens) + "]"
    return "'" + body + "'" if quoted else body


def make_fixtures():
    rows = []

    def add(name, tokens, first_decline=None):
        require(len(tokens) == DIMENSION, "fixture dimension")
        bits = [token_bits(token) for token in tokens]
        require(all(b & 0x7f800000 != 0x7f800000 for b in bits), "nonfinite fixture")
        key = len(rows) + 1
        rows.append({"id": key, "name": name, "tokens": tokens, "bits": bits,
                     "quoted": key % 2 == 1, "first_decline_position": first_decline})

    # Each counterexample would be wrong if a decimal were blindly converted
    # to double first. Exercise the first, middle and final coordinate.
    families = [(f"double_round_{i}", [token]) for i, token in enumerate(KNOWN_DOUBLE_ROUNDING)]
    families += [("integer_midpoint_even_down", ["16777217"]),
                 ("integer_midpoint_even_up", ["16777219"]),
                 ("fraction_midpoint", ["16384.0009765625", "16384.0029296875"]),
                 ("long_decimal", ["0.12345678901234567", "-1.2345678901234567",
                                    "1000000000000000"]),
                 ("scientific", ["1e-6", "-2.5e+3", "1E+0"]),
                 ("hex_compatibility", list(HEX_BITS)),
                 ("leading_plus", ["+0.5", "+0", "+1.25"]),
                 ("leading_dot", [".5", "-.5"]),
                 ("trailing_dot", ["1.", "-0."]),
                 ("underflow", ["1e-1000", "-1e-1000", "1e-45", "-1e-45"])]
    for family, special in families:
        for position in (0, 64, 127):
            tokens = ["0.125000"] * DIMENSION
            tokens[position:position + min(len(special), DIMENSION-position)] = special[:DIMENSION-position]
            add(f"{family}_at_{position}", tokens, position)

    all_fast = ["0", "-0", "0.0", "-0.000000", "0.125000", "-0.5",
                "999999999999999", "0.00000000000001"]
    add("all_short_signed_zero_and_limits", [all_fast[i % len(all_fast)] for i in range(DIMENSION)])
    mixed = ["0.125000"] * 64
    special = [token for _, values in families for token in values]
    mixed += [special[i % len(special)] for i in range(64)]
    add("mixed_after_first_decline", mixed, 64)
    for token, expected in KNOWN_DOUBLE_ROUNDING.items():
        require(token_bits(token) == expected, "known double-rounding oracle mismatch")
    require(token_bits("16777217") == 0x4b800000, "integer midpoint downward parity")
    require(token_bits("16777219") == 0x4b800002, "integer midpoint upward parity")
    require(token_bits("-0") == token_bits("-1e-1000") == 0x80000000, "negative zero oracle")
    require(token_bits("1e-45") == 1 and token_bits("-1e-45") == 0x80000001, "subnormal oracle")
    require(int("9277455312499999999") >= 1 << 63, "v2 unsigned-high-bit fixture")
    return rows


@contextmanager
def running(binary, data, log, timeout):
    env = {key: value for key, value in os.environ.items() if not key.startswith("MVDB_")}
    env["MVDB_PROTOCOL"] = "jsonl"
    with log.open("wb") as stderr:
        session = Session([str(binary), "--data", str(data), "--protocol", "jsonl"],
                          env, timeout, stderr)
        try:
            yield session
        finally:
            session.close()


class Pair:
    def __init__(self, baseline, candidate, report, request_hash, response_hash):
        self.sessions = (baseline, candidate)
        self.report = report
        self.request_hash = request_hash
        self.response_hash = response_hash

    def compare(self, label, responses):
        old, new = map(canonical, responses)
        require(old == new, f"full response mismatch at {label}: baseline={responses[0]!r}; "
                f"candidate={responses[1]!r}")
        self.response_hash.update(label.encode() + b"\0" + old + b"\n")
        self.report["response_checks"].append({"label": label, "sha256": hashlib.sha256(old).hexdigest()})
        return responses[1]

    def ready(self, stage):
        response = self.compare(f"{stage}-ready", [s.read_json("startup ready") for s in self.sessions])
        require(response == {"type": "ready"}, "startup ready envelope")

    def request(self, request, *, ok=True):
        label = request["id"]
        self.request_hash.update(canonical(request) + b"\n")
        responses = []
        for session in self.sessions:
            session.send_json(request)
            responses.append(session.read_json(label))
        response = self.compare(label, responses)
        require(response.get("id") == label and response.get("ok") is ok,
                f"unexpected response status {label}: {response!r}")
        if not ok:
            error = response.get("error")
            require(type(error) is dict and type(error.get("code")) is str and error["code"],
                    f"missing rejection error at {label}")
        return response

    def sql(self, label, statement, *, ok=True, affected=None):
        response = self.request({"id": label, "op": "execute", "sql": statement,
                                 "search_mode": "exact"}, ok=ok)
        if affected is not None:
            require(response.get("affected_rows") == affected, f"affected rows at {label}")
        return response

    def verify_rows(self, label, expected):
        response = self.sql(label, "SELECT id,v FROM decimal_vectors ORDER BY id;")
        require(response.get("columns") == ["id", "v"], f"columns at {label}")
        actual = response.get("rows")
        require(type(actual) is list and len(actual) == len(expected), f"row count at {label}")
        for received, fixture in zip(actual, expected):
            require(type(received) is list and len(received) == 2 and received[0] == fixture["id"],
                    f"row key/order at {label}: {received[:1]!r}")
            require(type(received[1]) is list and len(received[1]) == DIMENSION, f"dimension at {label}")
            bits = [received_bits(component) for component in received[1]]
            if bits != fixture["bits"]:
                index = next(i for i in range(DIMENSION) if bits[i] != fixture["bits"][i])
                raise AssertionError(f"{label}/{fixture['name']}/coordinate{index}: "
                                     f"token={fixture['tokens'][index]!r}, actual={bits[index]:08x}, "
                                     f"expected={fixture['bits'][index]:08x}")
        self.report["bitwise_readbacks"] += len(expected) * DIMENSION
        self.report["state_verifications"] += 1

    def shutdown(self, stage):
        self.request({"id": f"{stage}-shutdown", "op": "shutdown"})
        for session in self.sessions:
            session.finish()


def invalid_statements():
    normal = ["0.125000"] * DIMENSION
    cases = []
    for name, token in (("empty_component", ""), ("garbage", "0.5xyz"),
                        ("incomplete_exponent", "1e"), ("nan", "nan"),
                        ("infinity", "inf"), ("overflow", "1e1000")):
        tokens = normal.copy()
        tokens[64] = token
        cases.append((name, vector_sql(tokens)))
    cases.append(("trailing_comma", "'[" + ",".join(normal) + ",]'"))
    cases.append(("wrong_dimension", vector_sql(normal[:-1])))
    valid = vector_sql(normal)
    for index, (name, bad) in enumerate(cases):
        # A valid first tuple must not survive failure of the later tuple.
        key = 1000 + index * 2
        yield name, f"INSERT INTO decimal_vectors VALUES ({key},{valid}),({key+1},{bad});"
    yield "duplicate_key", f"INSERT INTO decimal_vectors VALUES (2000,{valid}),(1,{valid});"
    yield "trailing_statement", (f"INSERT INTO decimal_vectors VALUES (2001,{valid}); "
                                 "DELETE FROM decimal_vectors WHERE id=1;")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    baseline, candidate = args.baseline.resolve(), args.candidate.resolve()
    fixtures = make_fixtures()
    corpus = [{key: value for key, value in row.items() if key != "bits"} for row in fixtures]
    report = {"status": "running", "scope": "actual JSONL SQL insert/readback/commit/restart; no performance claim",
              "baseline_sha256": hashlib.sha256(baseline.read_bytes()).hexdigest(),
              "candidate_sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
              "oracle": "integer rational to binary32 RNE, quotient/remainder tie-even; explicit known hex bits",
              "corpus_sha256": hashlib.sha256(canonical(corpus)).hexdigest(),
              "rows": len(fixtures), "dimension": DIMENSION,
              "quoted_rows": sum(row["quoted"] for row in fixtures),
              "unquoted_rows": sum(not row["quoted"] for row in fixtures),
              "fixtures": [{key: value for key, value in row.items() if key not in ("tokens", "bits")}
                           for row in fixtures],
              "first_decline_positions": [0, 64, 127],
              "known_double_rounding_counterexamples": {token: f"{bits:08x}" for token, bits in KNOWN_DOUBLE_ROUNDING.items()},
              "v2_nonzero_midpoint_offset_cases": ["37.692861557006836", "-37.692861557006836",
                                                   "927745.5312499999999", "-927745.5312499999999"],
              "internal_fast_path_execution_observed": False,
              "response_comparison": "all parsed response fields; no ignored error/metadata fields; signed zero preserved",
              "response_checks": [], "bitwise_readbacks": 0, "state_verifications": 0,
              "rejected_statements": [], "restart_verified": False}
    request_hash, response_hash = hashlib.sha256(), hashlib.sha256()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="short-decimal-protocol-") as temporary:
            root = Path(temporary)
            data = (root / "baseline-data", root / "candidate-data")
            for stage in ("initial", "restart"):
                with running(baseline, data[0], root / f"baseline-{stage}.stderr", args.timeout) as old, \
                     running(candidate, data[1], root / f"candidate-{stage}.stderr", args.timeout) as new:
                    pair = Pair(old, new, report, request_hash, response_hash)
                    pair.ready(stage)
                    if stage == "initial":
                        pair.sql("create", "CREATE TABLE decimal_vectors(id BIGINT PRIMARY KEY,v VECTOR(128));")
                        pair.sql("begin-load", "BEGIN;")
                        for fixture in fixtures:
                            value = vector_sql(fixture["tokens"], fixture["quoted"])
                            pair.sql(f"insert-{fixture['name']}",
                                     f"INSERT INTO decimal_vectors VALUES ({fixture['id']},{value});", affected=1)
                        pair.verify_rows("read-before-commit", fixtures)
                        pair.sql("commit-load", "COMMIT;")
                        pair.verify_rows("read-after-commit", fixtures)
                        pair.sql("begin-errors", "BEGIN;")
                        for name, statement in invalid_statements():
                            pair.sql(f"reject-{name}", statement, ok=False)
                            pair.verify_rows(f"unchanged-after-{name}", fixtures)
                            report["rejected_statements"].append(name)
                        pair.sql("commit-after-errors", "COMMIT;")
                        pair.verify_rows("read-after-error-commit", fixtures)
                    else:
                        pair.verify_rows("read-after-restart", fixtures)
                        report["restart_verified"] = True
                    pair.shutdown(stage)
        report["status"] = "PASS"
    except Exception as error:
        report["status"] = "FAIL"
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        report["request_sha256"] = request_hash.hexdigest()
        report["response_transcript_sha256"] = response_hash.hexdigest()
        report["full_response_differential_checks"] = len(report["response_checks"])
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "rows", "dimension", "bitwise_readbacks",
                     "full_response_differential_checks", "restart_verified", "corpus_sha256")}))


if __name__ == "__main__":
    main()
