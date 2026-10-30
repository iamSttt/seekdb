"""Offline DDL and individual SQL query benchmark; development data only."""
import argparse
import json
import os
import time
import uuid
from pathlib import Path

import numpy as np
import pymysql

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', default='127.0.0.1')
p.add_argument('--port', type=int, default=2881)
p.add_argument('--user', default='root')
p.add_argument('--data', type=Path, required=True)
p.add_argument('--dataset', required=True)
p.add_argument('--backend', choices=['cuvs', 'vsag'], required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--queries', type=int, default=4096)
a = p.parse_args()
b = np.load(a.data / (a.dataset + '_base.npy'))
q = np.load(a.data / (a.dataset + '_query.npy'))[:a.queries]
gt = np.load(a.data / (a.dataset + '_truth.npy'))[:a.queries]
if not (b.dtype == q.dtype == np.float32 and b.ndim == q.ndim == 2
        and b.shape[1] == q.shape[1] and len(b) >= 100 and len(q) > 0
        and len(q) == len(gt) and gt.shape[1] >= 100):
    raise ValueError('invalid benchmark inputs')
a.output.parent.mkdir(parents=True, exist_ok=True)

def vector_text(x):
    # Nine significant decimal digits round-trip every finite FP32 value.
    return '[' + ','.join(format(float(v), '.9g') for v in x) + ']'

datafile = a.output.with_suffix('.tsv').resolve()
with datafile.open('w') as f:
    for i, row in enumerate(b):
        f.write(str(i) + '\t' + vector_text(row) + '\n')

db = 'cagra_validation_' + uuid.uuid4().hex[:12]
conn = pymysql.connect(host=a.host, port=a.port, user=a.user,
                       password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                       autocommit=True, local_infile=True, read_timeout=900)
result = {'scope': 'development SQL benchmark; not official scoring',
          'dataset': a.dataset, 'backend': a.backend, 'n': len(b),
          'dim': b.shape[1], 'nq': len(q), 'database': db}

def check(rows, k):
    ids = [int(row[0]) for row in rows]
    if len(ids) != k or len(set(ids)) != k or not all(0 <= x < len(b) for x in ids):
        raise RuntimeError('result_integrity')
    return ids

try:
    with conn.cursor() as cur:
        # Large LOAD DATA statements exceed the default short session timeout.
        # Configure this before import; import remains outside the build timer.
        cur.execute('SET SESSION ob_query_timeout=900000000')
        cur.execute('SET SESSION ob_trx_timeout=900000000')
        cur.execute('CREATE DATABASE ' + db)
        cur.execute('USE ' + db)
        cur.execute(f'CREATE TABLE items (id BIGINT PRIMARY KEY, v VECTOR({b.shape[1]}))')
        cur.execute("LOAD DATA LOCAL INFILE %s INTO TABLE items FIELDS TERMINATED BY '\\t' "
                    "LINES TERMINATED BY '\\n' (id,v)", (str(datafile),))
        cur.execute('SELECT COUNT(*) FROM items')
        if cur.fetchone()[0] != len(b):
            raise RuntimeError('incomplete data import')
        queries = [vector_text(row) for row in q]
        start = time.perf_counter()
        cur.execute('CREATE VECTOR INDEX idx_items ON items(v) WITH '
                    f'(distance=l2, type=hnsw, lib={a.backend}, m=16, '
                    'ef_construction=200, ef_search=64)')
        result['ddl_s'] = time.perf_counter() - start
        cur.execute('SELECT id FROM items ORDER BY l2_distance(v, %s) APPROXIMATE LIMIT 10',
                    (queries[0],))
        check(cur.fetchall(), 10)
        result['build_first_s'] = time.perf_counter() - start
        print('BUILD', json.dumps(result), flush=True)
        for k in [10, 100]:
            matched, times = 0, []
            for i, query in enumerate(queries):
                start = time.perf_counter()
                cur.execute('SELECT id FROM items ORDER BY l2_distance(v, %s) '
                            f'APPROXIMATE LIMIT {k}', (query,))
                rows = cur.fetchall()
                times.append(time.perf_counter() - start)
                ids = check(rows, k)
                matched += len(set(ids) & set(gt[i, :k].tolist()))
            result[f'qps{k}'] = len(q) / sum(times)
            result[f'recall{k}'] = matched / (len(q) * k)
            result[f'p95_ms{k}'] = float(np.percentile(times, 95) * 1000)
        result['integrity'] = 'pass'
finally:
    conn.close()
    a.output.with_suffix('.json').write_text(json.dumps(result, indent=2))
print('RESULT', json.dumps(result), flush=True)
# Preserve the isolated database and logs for diagnosis. This script never drops
# an existing database, reuses a graph, or excludes the first query from build time.
