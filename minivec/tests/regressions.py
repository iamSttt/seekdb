#!/usr/bin/env python3
"""Independent SQL regressions, not the unpublished competition driver."""
import argparse
import math
import os
from pathlib import Path
import random
import subprocess
import tempfile


def run(binary, directory, sql):
    p = subprocess.run([binary, '--data', str(directory)], input=sql,
                       text=True, capture_output=True, timeout=30)
    assert p.returncode == 0, (p.returncode, p.stderr)
    return p.stdout


def suite(binary):
    passed = 0
    def check(name, sql, expected, verify=None):
        nonlocal passed
        with tempfile.TemporaryDirectory(prefix='mvdb-regression-') as d:
            out = run(binary, d, sql)
            if verify: verify(out)
            else: assert out == expected, (name, expected, out)
        passed += 1
        print('PASS', name, flush=True)

    check('distance ties use ascending primary key', '''
CREATE TABLE t(id BIGINT PRIMARY KEY, v VECTOR(2));
INSERT INTO t VALUES(9,'[1,0]'),(2,'[-1,0]'),(5,'[0,1]');
SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2;
SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') DESC LIMIT 2;
''', 'id\n2\n5\nid\n2\n5\n')
    check('non-projected ordering and exact BIGINT order', '''
CREATE TABLE t(id BIGINT PRIMARY KEY, n BIGINT);
INSERT INTO t VALUES(3,9007199254740993),(2,9007199254740992),(1,9007199254740993);
SELECT id FROM t ORDER BY n;
SELECT id FROM t ORDER BY n DESC;
''', 'id\n2\n1\n3\nid\n1\n3\n2\n')
    check('filter before top-k and empty limit', '''
CREATE TABLE t(id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(2));
INSERT INTO t VALUES(1,0,'[0,0]'),(8,1,'[5,0]'),(3,1,'[3,0]');
SELECT id FROM t WHERE g=1 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 10;
SELECT id FROM t WHERE g=2 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 10;
SELECT id FROM t ORDER BY id LIMIT 0;
''', 'id\n3\n8\nid\nid\n')
    check('quoted vector-like VARCHAR preserves spelling', '''
CREATE TABLE t(id BIGINT PRIMARY KEY, s VARCHAR(12));
INSERT INTO t VALUES(1,'[1, 2]'),(2,'[3]');
SELECT s FROM t WHERE s='[1, 2]';
''', 's\n[1, 2]\n')
    check('Unicode codepoint length and order', '''
CREATE TABLE t(id BIGINT PRIMARY KEY, s VARCHAR(2));
INSERT INTO t VALUES(3,'😀'),(1,'中'),(2,'a');
SELECT id FROM t ORDER BY s;
''', 'id\n2\n1\n3\n')

    def check_atomic(out):
        lines = out.splitlines()
        errors = [x for x in lines if x.startswith('ERROR ')]
        assert len(errors) == 2, out
        assert [x for x in lines if not x.startswith('ERROR ')] == ['id','1','4','id','1','4'], out
    check('failed explicit and implicit INSERT are atomic', '''
CREATE TABLE t(id BIGINT PRIMARY KEY);
INSERT INTO t VALUES(1);
BEGIN;
INSERT INTO t VALUES(4);
INSERT INTO t VALUES(2),(1);
COMMIT;
SELECT id FROM t ORDER BY id;
INSERT INTO t VALUES(3),(1);
SELECT id FROM t ORDER BY id;
''', None, check_atomic)

    def check_extreme(out):
        lines = out.splitlines()
        assert not any(x.startswith('ERROR') for x in lines), out
        assert lines[0] == 'id\td' and lines[1].split('\t')[0] == '2', out
        distances = [float(x.split('\t')[1]) for x in lines[1:]]
        assert all(math.isfinite(x) for x in distances), out
        assert distances[0] == 0 and distances[1] > 6e38, out
    check('finite float32 endpoints do not overflow distance', '''
CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1));
INSERT INTO t VALUES(1,'[3.4028234663852886e38]'),(2,'[-3.4028234663852886e38]');
SELECT id,L2_DISTANCE(v,'[-3.4028234663852886e38]') AS d FROM t ORDER BY d;
''', None, check_extreme)

    # Independent Python state model; every mutation is followed by a sorted
    # projection. Includes repeated keys, transactions, rollback and commits.
    rng = random.Random(20261109)
    sql = ['CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);']
    model = {}
    snapshot = None
    expected = []
    for i in range(200):
        action = rng.randrange(7)
        key, value = rng.randrange(35), rng.randrange(-100,100)
        if action == 0 and snapshot is None:
            snapshot = model.copy(); sql.append('BEGIN;')
        elif action == 1 and snapshot is not None:
            model = snapshot; snapshot = None; sql.append('ROLLBACK;')
        elif action == 2 and snapshot is not None:
            snapshot = None; sql.append('COMMIT;')
        elif action in (3,4) and key not in model:
            model[key] = value; sql.append(f'INSERT INTO t VALUES({key},{value});')
        elif action == 5:
            if key in model: model[key] = value
            sql.append(f'UPDATE t SET n={value} WHERE id={key};')
        else:
            model.pop(key, None); sql.append(f'DELETE FROM t WHERE id={key};')
        sql.append('SELECT id,n FROM t ORDER BY id;')
        expected.append('id\tn\n'+''.join(f'{k}\t{v}\n' for k,v in sorted(model.items())))
    check('200-operation independent transaction model', '\n'.join(sql), ''.join(expected))

    # Independent exact TopK oracle over explicitly float32-stored values.
    import struct
    f32 = lambda x: struct.unpack('f', struct.pack('f', x))[0]
    dim = 17
    vectors = {i: [f32(rng.uniform(-5,5)) for _ in range(dim)] for i in range(161)}
    sql = ['CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(17));']
    sql.append('INSERT INTO t VALUES'+','.join(
        f"({i},{i%4},'[{','.join(map(str,v))}]')" for i,v in vectors.items())+';')
    expected = []
    for j in range(45):
        q = [f32(rng.uniform(-5,5)) for _ in range(dim)]
        group = j % 5
        ids = [i for i in vectors if group == 4 or i % 4 == group]
        ranked = sorted(ids, key=lambda i: (sum((a-b)**2 for a,b in zip(vectors[i],q)),i))[:10]
        where = '' if group == 4 else f' WHERE g={group}'
        sql.append(f"SELECT id FROM t{where} ORDER BY L2_DISTANCE(v,'[{','.join(map(str,q))}]') LIMIT 10;")
        expected.append('id\n'+''.join(f'{i}\n' for i in ranked))
    check('45 filtered and unfiltered TopK oracle queries', '\n'.join(sql), ''.join(expected))
    # Pipe boundaries/newlines are not SQL statement boundaries. In particular,
    # UPDATE's WHERE may arrive in a subsequent write; never execute its prefix.
    with tempfile.TemporaryDirectory(prefix='mvdb-fragment-') as d:
        p = subprocess.Popen([binary,'--data',d],stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
        p.stdin.write('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT); INSERT INTO t VALUES(1,1); SELECT id FROM t LIMIT 0;\n')
        p.stdin.flush(); assert p.stdout.readline()=='id\n'
        p.stdin.write('UPDATE t SET n=2\n');p.stdin.flush()
        import time
        time.sleep(0.05)
        p.stdin.write('WHERE id=999; SELECT n FROM t;\n');p.stdin.flush()
        assert p.stdout.readline()=='n\n' and p.stdout.readline()=='1\n'
        p.stdin.close(); assert p.wait(timeout=10)==0
    passed+=1
    print('PASS fragmented multiline statement preserves WHERE',flush=True)
    print(f'RESULT {passed} independent regression groups passed', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--binary', default='./minivec')
    args = p.parse_args(); suite(str(Path(args.binary).resolve()))
