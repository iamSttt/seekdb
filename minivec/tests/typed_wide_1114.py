#!/usr/bin/env python3
"""Paired live output oracle for the narrow R33 typed-wide port; no timings."""
import argparse
from contextlib import ExitStack
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import tempfile

from json_protocol import MAX_RESPONSE_BYTES, launch, require, strict_json


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def vector_sql(values):
    return '[' + ','.join(repr(value) for value in values) + ']'


def quote(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


def same_value(actual, expected, field='rows'):
    require(type(actual) is type(expected), field + ': JSON type differs')
    if isinstance(expected, float):
        require(math.isfinite(actual) and struct.pack('<d', actual) == struct.pack('<d', expected),
                field + ': numeric bits/zero sign differ')
    elif isinstance(expected, list):
        require(len(actual) == len(expected), field + ': length differs')
        for index, (got, want) in enumerate(zip(actual, expected)):
            same_value(got, want, field + '[' + str(index) + ']')
    else:
        require(actual == expected, field + ': value differs')


def fixtures():
    keys = [(1 << 63) - 1, -(1 << 63), (1 << 53) + 1, -((1 << 53) + 1), 7, -3]
    texts = ['中\0\n\t"\\😀', 'same', "quote'\r\b\f", 'same', '', 'é中😀']
    result = {}
    for dimension in (16, 127, 128, 129, 768):
        rows = []
        for index, key in enumerate(keys):
            ordinary = [float(((j + index * 3) % 17) - 8) / 8 for j in range(dimension)]
            ordinary[0] = -0.0
            extreme = [-0.0, 0.0, f32(1), f32(0x7f7fffff), -f32(0x7f7fffff), 0.125]
            extreme += [float((j % 11) - 5) / 8 for j in range(dimension - len(extreme))]
            number = (1 << 63) - 1 if index % 2 else -(1 << 63)
            rows.append([key, number, texts[index], ordinary, extreme])
        result[dimension] = rows
    return result


def distance(row):
    # Independent exact rational sum -> binary64 RNE -> sqrt, matching the
    # public canonical-distance contract without using an engine implementation.
    return math.sqrt(float(sum((Fraction(value) ** 2 for value in row[3]), Fraction())))


def run(baseline, candidate, timeout):
    transcript = hashlib.sha256()
    sequence = 0
    checks = []
    data = fixtures()
    saved = {key: value for key, value in os.environ.items() if key.startswith('MVDB_')}
    for key in saved:
        del os.environ[key]
    try:
        with tempfile.TemporaryDirectory(prefix='mvdb-wide-base-') as left, \
                tempfile.TemporaryDirectory(prefix='mvdb-wide-candidate-') as right:
            directories = [Path(left), Path(right)]
            programs = [baseline, candidate]

            def request(sessions, sql, *, columns=None, rows=None, ok=True, mode='exact', ident='generated'):
                nonlocal sequence
                sequence += 1
                if ident == 'generated':
                    ident = sequence
                payload = {'id': ident, 'op': 'execute', 'sql': sql, 'search_mode': mode}
                for session in sessions:
                    session.send_json(payload)
                lines = [session.read_line('paired typed-wide response') for session in sessions]
                require(all(line and line.endswith(b'\n') and len(line) <= MAX_RESPONSE_BYTES for line in lines),
                        'empty/oversized/unterminated output')
                require(lines[0] == lines[1], 'baseline/candidate JSON bytes differ for ' + sql[:200])
                transcript.update(lines[0])
                response = strict_json(lines[0])
                sessions[0].check_response(response, ident, ok)
                if columns is not None:
                    same_value(response['columns'], columns, 'columns')
                    same_value(response['rows'], rows)
                if not ok:
                    require(set(response) == {'id', 'ok', 'error'}, 'failed query leaked result members')
                checks.append({'sequence': sequence, 'ok': ok, 'independent_rows': columns is not None})

            def check_table(sessions, dimension, rows):
                table = 'wide' + str(dimension)
                by_id = sorted(rows, key=lambda row: row[0])
                q = vector_sql([0.0] * dimension)
                request(sessions, f'SELECT id,n,s,v FROM {table} ORDER BY id LIMIT 10;',
                        columns=['id', 'n', 's', 'v'], rows=[row[:4] for row in by_id], ident=None)
                positive = sorted((row for row in rows if row[1] > 0), key=lambda row: -row[0])
                request(sessions, f'SELECT s,v,id,s,n FROM {table} WHERE n>0 ORDER BY id DESC LIMIT 4096;',
                        columns=['s', 'v', 'id', 's', 'n'],
                        rows=[[row[2], row[3], row[0], row[2], row[1]] for row in positive], ident=True)
                request(sessions, f'SELECT * FROM {table} ORDER BY s;',
                        columns=['id', 'n', 's', 'v'],
                        rows=[row[:4] for row in sorted(rows, key=lambda row: (row[2], row[0]))])
                request(sessions, f'SELECT id,v FROM {table} WHERE id=99 ORDER BY id LIMIT 0;',
                        columns=['id', 'v'], rows=[])
                ranked = sorted(rows, key=lambda row: (distance(row), row[0]))
                request(sessions, f'SELECT id,L2_DISTANCE(v,{q}) AS dist FROM {table} ORDER BY dist LIMIT 10;',
                        columns=['id', 'dist'], rows=[[row[0], distance(row)] for row in ranked])
                descending = sorted(rows, key=lambda row: (-distance(row), row[0]))
                request(sessions, f'SELECT id,v,L2_DISTANCE(v,{q}) AS dist FROM {table} ORDER BY dist DESC LIMIT 4096;',
                        columns=['id', 'v', 'dist'], rows=[[row[0], row[3], distance(row)] for row in descending],
                        ident='顺序\n\0"')
                negative = [row for row in by_id if row[1] < 0]
                request(sessions, f'SELECT v,v,id FROM {table} WHERE n<0 ORDER BY id LIMIT 4096;',
                        columns=['v', 'v', 'id'], rows=[[row[3], row[3], row[0]] for row in negative])
                request(sessions, f'SELECT id,s FROM {table} ORDER BY id LIMIT 4096;',
                        columns=['id', 's'], rows=[[row[0], row[2]] for row in by_id])
                # ann currently keeps exact fallback in both versions; mode
                # routing and output are compared without claiming ANN recall.
                request(sessions, f'SELECT id,v FROM {table} ORDER BY id LIMIT 10;',
                        columns=['id', 'v'], rows=[[row[0], row[3]] for row in by_id], mode='ann')
                # The SQL subset permits one VECTOR column per table. Keep
                # float32-extreme output in a separate table from distance data.
                request(sessions, f'SELECT id,v FROM extreme{dimension} ORDER BY id LIMIT 4096;',
                        columns=['id', 'v'], rows=[[row[0], row[4]] for row in by_id])
                request(sessions, f'SELECT * FROM extreme{dimension} ORDER BY id;',
                        columns=['id', 'v'], rows=[[row[0], row[4]] for row in by_id])

            with ExitStack() as stack:
                sessions = [stack.enter_context(launch(binary, directory, timeout, '--protocol', 'jsonl'))
                            for binary, directory in zip(programs, directories)]
                for dimension, rows in data.items():
                    table = 'wide' + str(dimension)
                    request(sessions, f'CREATE TABLE {table}(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(65535),'
                                      f'v VECTOR({dimension}));')
                    values = ','.join('(' + ','.join([str(row[0]), str(row[1]), quote(row[2]),
                                                     vector_sql(row[3])]) + ')' for row in rows)
                    request(sessions, f'INSERT INTO {table} VALUES {values};')
                    request(sessions, f'CREATE TABLE extreme{dimension}(id BIGINT PRIMARY KEY,v VECTOR({dimension}));')
                    extremes = ','.join('(' + str(row[0]) + ',' + vector_sql(row[4]) + ')' for row in rows)
                    request(sessions, f'INSERT INTO extreme{dimension} VALUES {extremes};')
                    check_table(sessions, dimension, rows)
                    zero = vector_sql([0.0] * dimension)
                    # NULL is not a stored Value type in this SQL subset. Keep
                    # its rejection and the following unchanged result, rather
                    # than silently inventing nullable-value support.
                    request(sessions, f"INSERT INTO {table} VALUES (99,NULL,'null',{zero});", ok=False)
                    overflow = '[1e400' + ',0' * (dimension - 1) + ']'
                    request(sessions, f"INSERT INTO {table} VALUES (99,1,'bad',{overflow});", ok=False)
                    request(sessions, 'BEGIN;')
                    key = rows[0][0]
                    request(sessions, f'UPDATE {table} SET v={zero} WHERE id={key};')
                    request(sessions, f'SELECT id,v FROM {table} WHERE id={key};',
                            columns=['id', 'v'], rows=[[key, [0.0] * dimension]])
                    request(sessions, 'ROLLBACK;')
                    request(sessions, f'SELECT id,v FROM {table} WHERE id={key};',
                            columns=['id', 'v'], rows=[[key, rows[0][3]]])
                for session in sessions:
                    session.shutdown()
            with ExitStack() as stack:
                sessions = [stack.enter_context(launch(binary, directory, timeout, '--protocol', 'jsonl'))
                            for binary, directory in zip(programs, directories)]
                for dimension, rows in data.items():
                    check_table(sessions, dimension, rows)
                for session in sessions:
                    session.shutdown()
    finally:
        os.environ.update(saved)
    return {'status': 'passed', 'requests': sequence, 'dimensions': list(data),
            'byte_identical_response_sha256': transcript.hexdigest(), 'checks': checks,
            'typed_path_directly_instrumented': False,
            'scope': 'Live paired byte equality plus independent numeric/type/string/order fixtures; no performance or internal branch-counter claim.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse to overwrite result')
    result = run(args.baseline.resolve(), args.candidate.resolve(), args.timeout)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf8')
    print(json.dumps({key: result[key] for key in ('status', 'requests', 'dimensions', 'byte_identical_response_sha256')}))
