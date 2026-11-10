#!/usr/bin/env python3
"""Compare JSONL TopK results with an independent exact float32 distance oracle."""

import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import random
import struct
import sys
import tempfile
from typing import NamedTuple


PROTOCOL_CHECK = Path(__file__).resolve().parents[2] / "protocol_check"
sys.path.insert(0, str(PROTOCOL_CHECK))
from _transport import Session


DIMENSION = 128
F32 = struct.Struct("!f")
U32 = struct.Struct("!I")
MIN_F32 = 2.0 ** -149
MAX_F32 = F32.unpack(U32.pack(0x7F7FFFFF))[0]
NEAR_WINNER = -9007199254740993
NEAR_EARLY = 9007199254740997


class Vector(NamedTuple):
    sql: str
    scaled: tuple


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def stored_vector(values=()):
    coordinates = tuple(F32.unpack(F32.pack(value))[0] for value in values)
    require(len(coordinates) <= DIMENSION, "oversized test vector")
    coordinates += (0.0,) * (DIMENSION - len(coordinates))
    # Every finite float32 is an integral multiple of 2^-149. Recover the
    # integers from Python's exact ratios, without using the engine's decoder.
    scaled = tuple((numerator << 149) // denominator
                   for numerator, denominator in map(float.as_integer_ratio, coordinates))
    return Vector("'[" + ",".join(map(repr, coordinates)) + "]'", scaled)


def canonical_squared(left, right):
    scaled_square_sum = sum((a - b) ** 2 for a, b in zip(left.scaled, right.scaled))
    # Round only the exact sum, once, to binary64. Distinct real distances
    # that have the same binary64 representation use the primary-key tie rule.
    return float(Fraction(scaled_square_sum, 1 << 298))


def finite_float32(rng):
    bits = rng.getrandbits(32)
    if bits & 0x7F800000 == 0x7F800000:
        bits &= ~0x00800000  # keep exponent 254 instead of NaN or Infinity
    return F32.unpack(U32.pack(bits))[0]


def fixtures():
    rng = random.Random(7319)
    h = 2.0 ** 30
    near_query = stored_vector((0, 0, 0, MIN_F32))
    near_early = stored_vector((h, 16))
    near_late = stored_vector((h, 16, 8, 8))
    near_next = stored_vector((h, 16, 8, 0, 8))
    threshold = canonical_squared(near_early, near_query)
    require(threshold == canonical_squared(near_late, near_query) == 2.0 ** 60 + 256,
            "the near rows must tie after rounding")
    require(canonical_squared(near_next, near_query) == math.nextafter(threshold, math.inf),
            "the farther row must be exactly one binary64 ULP away")
    # A naive double square of the late row rounds 8 - 2^-149 to 8,
    # overshoots the heap threshold, and could lose the smaller primary key.
    early = [(NEAR_EARLY, 3, near_early), (21, 3, near_next)]
    late = [(NEAR_WINNER, 3, near_late), (-8, 3, near_next)]

    rows = []
    for index, key in enumerate(rng.sample(range(-100000, 100000), 240)):
        if index < 176:
            vector = stored_vector(rng.uniform(-2, 2) for _ in range(DIMENSION))
            rows.append((key, index % 2, vector))
        else:
            vector = stored_vector(finite_float32(rng) for _ in range(DIMENSION))
            rows.append((key, 2, vector))
    mixed_query = stored_vector((MAX_F32, -MAX_F32, MIN_F32, -MIN_F32, h))
    mixed_opposite = stored_vector((-MAX_F32, MAX_F32, -MIN_F32, MIN_F32, -h))
    rows += [(400000, 2, mixed_query), (400001, 2, mixed_opposite)]

    identical = stored_vector((3, -4))
    rows.extend((key, 4, identical) for key in range(200001, 200012))
    late.append((200000, 4, identical))  # arrives after the k=10 heap fills
    rng.shuffle(rows)
    random_query = stored_vector(rng.uniform(-2, 2) for _ in range(DIMENSION))
    return early + rows + late, near_query, mixed_query, random_query


def execute(session, sql, label, *, affected=None, mode="exact"):
    response = session.request(sql, mode=mode)
    require(response.get("ok") is True, f"{label}: {response.get('error')!r}")
    if affected is not None:
        actual = response.get("affected_rows")
        require(type(actual) is int and actual == affected,
                f"{label}: affected_rows {actual!r}, expected {affected}")
    return response


def check_topk(session, records, query, label, *, k, group=None, descending=False, mode="exact"):
    where = "" if group is None else f" WHERE grp={group}"
    direction = "DESC" if descending else "ASC"
    sql = (f"SELECT id FROM docs{where} ORDER BY L2_DISTANCE(embedding,{query.sql}) "
           f"{direction} LIMIT {k};")
    response = execute(session, sql, label, mode=mode)
    rows = response.get("rows")
    require(response.get("columns") == ["id"] and type(rows) is list
            and all(type(row) is list and len(row) == 1 and type(row[0]) is int for row in rows),
            f"{label}: expected single-column BIGINT rows, got {response!r}")
    actual = [row[0] for row in rows]
    sign = -1 if descending else 1
    ranked = sorted((sign * canonical_squared(vector, query), key)
                    for key, (category, vector) in records.items() if group is None or category == group)
    expected = [key for _, key in ranked[:k]]
    require(actual == expected, f"{label}: got {actual}, expected {expected}")
    return actual


def run_checks(session):
    rows, near, mixed, random_query = fixtures()
    records = {key: (group, vector) for key, group, vector in rows}
    require(len(records) == len(rows), "fixture primary keys must be unique")
    oversized = len(records) + 7
    execute(session, f"CREATE TABLE docs(id BIGINT PRIMARY KEY, grp BIGINT, embedding VECTOR({DIMENSION}));",
            "create table")
    execute(session, "BEGIN;", "begin load")
    for start in range(0, len(rows), 64):
        batch = rows[start:start + 64]
        values = ",".join(f"({key},{group},{vector.sql})" for key, group, vector in batch)
        execute(session, f"INSERT INTO docs VALUES {values};", "load rows", affected=len(batch))
    execute(session, "COMMIT;", "commit load")

    check_topk(session, records, near, "unindexed late near-tie", k=1, group=3)
    check_topk(session, records, random_query, "unindexed random filter", k=10, group=0)
    execute(session, "CREATE VECTOR INDEX docs_v ON docs(embedding);", "create index")
    winner = check_topk(session, records, near, "indexed late near-tie", k=1, group=3)
    require(winner == [NEAR_WINNER], "fixture did not exercise the late replacement")
    check_topk(session, records, near, "indexed adjacent distance DESC", k=1, group=3, descending=True)
    check_topk(session, records, near, "near ties and distinct adjacent distances", k=oversized, group=3)
    for descending in (False, True):
        check_topk(session, records, stored_vector(), "identical rows at cutoff", k=10, group=4,
                   descending=descending)
        check_topk(session, records, mixed, "extreme float32 values", k=10, group=2,
                   descending=descending)
    check_topk(session, records, random_query, "unfiltered random", k=10)
    check_topk(session, records, random_query, "all rows DESC", k=oversized, descending=True)
    check_topk(session, records, near, "empty filter", k=1, group=99)

    execute(session, "BEGIN;", "begin vector update")
    updated = stored_vector((2.0 ** 30, 64))
    execute(session, f"UPDATE docs SET embedding={updated.sql} WHERE id={NEAR_WINNER};", "move winner",
            affected=1)
    changed = {**records, NEAR_WINNER: (3, updated)}
    winner = check_topk(session, changed, near, "transaction nearest", k=1, group=3, mode="ann")
    require(winner == [NEAR_EARLY], "fixture update must move the nearest primary key")
    check_topk(session, changed, near, "transaction farthest", k=1, group=3, descending=True)
    execute(session, "ROLLBACK;", "roll back vector update")
    check_topk(session, records, near, "rollback nearest", k=1, group=3)
    check_topk(session, records, near, "rollback farthest", k=1, group=3, descending=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "minivec")
    args = parser.parse_args()
    config = json.loads((PROTOCOL_CHECK / "config.json").read_text(encoding="utf-8"))
    command = [str(args.binary.resolve()), "--data", "{data_dir}", "--protocol", "jsonl"]
    with tempfile.TemporaryDirectory(prefix="minivec-topk-bounds-") as scratch:
        log = Path(scratch) / "database.stderr.log"
        session = Session(config, log, command=command)
        try:
            session.start()
            run_checks(session)
            session.shutdown()
        finally:
            session.close()
    print("PASS JSONL TopK against exact float32 oracle, including ties, filters and rollback")


if __name__ == "__main__":
    main()
