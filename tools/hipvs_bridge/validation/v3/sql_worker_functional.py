"""Concurrent SQL, index lifetime, filtering and CPU backend regression."""
import os,sys,json,time,subprocess,uuid
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import pymysql
w=Path('/workspace/work/opt-v2');v3=w/'v3';repo=next((w/'source').iterdir())
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
deadline=time.monotonic()+1800
while 'SQL PAIRS COMPLETE' not in (v3/'sql_worker_pair.log').read_text():
    if time.monotonic()>deadline:raise RuntimeError('SQL pair timeout')
    time.sleep(3)
base=w/'instances/sql3_sift250k_worker'
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(w/'final96')+':/opt/hipvs/lib:/opt/rocm/lib'
log=(v3/'functional_server.log').open('w')
server=subprocess.Popen([str(repo/'build_release/src/observer/seekdb'),'--base-dir='+str(base),'--port=2881','--nodaemon','--parameter','_memory_budget=16G','--parameter','datafile_size=4G','--parameter','datafile_maxsize=8G','--parameter','log_disk_size=2G','--log-level','INFO'],cwd=base,env=env,stdout=log,stderr=subprocess.STDOUT)
def connect():
    return pymysql.connect(host='127.0.0.1',port=2881,user='root',autocommit=True,read_timeout=120)
def text_vector(row):return '['+','.join(format(float(x),'.9g') for x in row)+']'
def ids(rows,k,n):
    out=[int(row[0]) for row in rows]
    assert len(out)==k and len(set(out))==k and all(0<=i<n for i in out)
    return out
try:
    for _ in range(180):
        assert server.poll() is None,'server exited'
        try:conn=connect();break
        except pymysql.MySQLError:time.sleep(1)
    else:raise RuntimeError('startup timeout')
    record=json.loads((w/'sql3_sift250k_worker.json').read_text());db=record['database']
    q=np.load(w/'full/sift250k_query.npy');gt=np.load(w/'full/sift250k_truth.npy')
    def client(c):
        connection=connect();matched=0
        try:
            with connection.cursor() as cur:
                cur.execute('USE '+db)
                for step in range(32):
                    i=c*32+step
                    cur.execute('SELECT id FROM items ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 100',(text_vector(q[i]),))
                    got=ids(cur.fetchall(),100,250000);matched+=len(set(got)&set(gt[i,:100].tolist()))
        finally:connection.close()
        return matched
    with ThreadPoolExecutor(max_workers=8) as pool:matches=sum(pool.map(client,range(8)))
    recall=matches/(256*100);assert recall>=0.995,recall
    # Only newly named test tables are created/dropped. Existing benchmark data
    # remains intact, and every index is independently built from current rows.
    rng=np.random.default_rng(20261031);b=rng.normal(size=(512,16)).astype(np.float32);probe=rng.normal(size=16).astype(np.float32)
    truth=np.argsort(np.sum((b-probe)**2,axis=1))[:10]
    testdb='worker_lifecycle_'+uuid.uuid4().hex[:10]
    lifecycle=[]
    with conn.cursor() as cur:
        cur.execute('SET SESSION ob_query_timeout=120000000');cur.execute('SET SESSION ob_trx_timeout=120000000')
        cur.execute('CREATE DATABASE '+testdb);cur.execute('USE '+testdb)
        for cycle,backend in enumerate(['cuvs','cuvs','vsag','cuvs']):
            name='test_'+str(cycle)
            cur.execute(f'CREATE TABLE {name}(id BIGINT PRIMARY KEY,v VECTOR(16))')
            cur.executemany(f'INSERT INTO {name} VALUES(%s,%s)',[(i,text_vector(row)) for i,row in enumerate(b)])
            cur.execute(f'CREATE VECTOR INDEX idx ON {name}(v) WITH(distance=l2,type=hnsw,lib={backend},m=16,ef_construction=200,ef_search=64)')
            cur.execute(f'SELECT id FROM {name} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(probe),))
            got=ids(cur.fetchall(),10,512);r=len(set(got)&set(truth.tolist()))/10
            assert r>=0.9,(cycle,backend,r)
            cur.execute(f'SELECT id FROM {name} WHERE MOD(id,2)=0 ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(probe),))
            filtered=ids(cur.fetchall(),10,512);assert all(i%2==0 for i in filtered)
            lifecycle.append({'backend':backend,'recall10':r,'filtered_integrity':'pass'})
            cur.execute('DROP TABLE '+name)
    conn.close()
    result={'status':'PASS','concurrent_clients':8,'concurrent_queries':256,'recall100':recall,'restart_load':'pass','index_create_drop_cycles':lifecycle}
    (v3/'sql_worker_functional.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
finally:
    server.terminate()
    try:server.wait(timeout=45)
    except subprocess.TimeoutExpired:server.kill();server.wait()
    log.close()
