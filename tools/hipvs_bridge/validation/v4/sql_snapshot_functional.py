"""Restart both historical and compact snapshots using the candidate kernel."""
import os,sys,json,time,subprocess,uuid
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import pymysql
w=Path(__file__).resolve().parent
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
deadline=time.monotonic()+3600
while 'SQL PAIRS COMPLETE' not in (w/'sql_compact_pair.log').read_text():
    if time.monotonic()>deadline:raise RuntimeError('SQL pair timeout')
    time.sleep(3)
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(w/'compact')+':/opt/hipvs/lib:/opt/rocm/lib'
def connect():return pymysql.connect(host='127.0.0.1',port=2881,user='root',autocommit=True,read_timeout=900)
def text_vector(row):return '['+','.join(format(float(x),'.9g') for x in row)+']'
def checked(rows,k,n):
    ids=[int(row[0]) for row in rows]
    assert len(ids)==len(set(ids))==k and all(0<=i<n for i in ids)
    return ids
results=[]
for dataset,label in [('normal256','v3'),('normal256','compact'),('sift250k','compact')]:
    tag=f'sql4_{dataset}_{label}';base=w/'instances'/tag
    log=(w/f'restart_{dataset}_{label}.log').open('w')
    server=subprocess.Popen([str(w/'seekdb_compact'),'--base-dir='+str(base),'--port=2881','--nodaemon','--parameter','_memory_budget=16G','--parameter','datafile_size=4G','--parameter','datafile_maxsize=8G','--parameter','log_disk_size=2G','--log-level','INFO'],cwd=base,env=env,stdout=log,stderr=subprocess.STDOUT)
    try:
        for _ in range(180):
            assert server.poll() is None,'server exited'
            try:conn=connect();break
            except pymysql.MySQLError:time.sleep(1)
        else:raise RuntimeError('startup timeout')
        record=json.loads((w/(tag+'.json')).read_text());db=record['database']
        q=np.load(w/'full'/f'{dataset}_query.npy');gt=np.load(w/'full'/f'{dataset}_truth.npy')
        def client(c):
            connection=connect();matched=0
            try:
                with connection.cursor() as cur:
                    cur.execute('SET SESSION ob_query_timeout=900000000');cur.execute('USE '+db)
                    for step in range(32):
                        i=c*32+step
                        cur.execute('SELECT id FROM items ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 100',(text_vector(q[i]),))
                        ids=checked(cur.fetchall(),100,250000);matched+=len(set(ids)&set(gt[i,:100]))
            finally:connection.close()
            return matched
        with ThreadPoolExecutor(max_workers=8) as pool:matches=sum(pool.map(client,range(8)))
        recall=matches/(256*100);assert recall>=(0.985 if dataset=='normal256' else 0.995),recall
        with conn.cursor() as cur:
            cur.execute('SET SESSION ob_query_timeout=900000000');cur.execute('USE '+db)
            cur.execute('SELECT id FROM items WHERE MOD(id,2)=0 ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(q[0]),))
            ids=checked(cur.fetchall(),10,250000);assert all(i%2==0 for i in ids)
        result={'snapshot_source':label,'dataset':dataset,'restart_load':'PASS','concurrent_clients':8,'queries':256,'recall100':recall,'filtered_query_integrity':'PASS'}
        results.append(result);print(result,flush=True)
        if dataset=='sift250k':
            rng=np.random.default_rng(20261031);b=rng.normal(size=(512,16)).astype(np.float32);probe=rng.normal(size=16).astype(np.float32)
            truth=np.argsort(np.sum((b-probe)**2,axis=1))[:10]
            testdb='compact_lifecycle_'+uuid.uuid4().hex[:10];cycles=[]
            with conn.cursor() as cur:
                cur.execute('CREATE DATABASE '+testdb);cur.execute('USE '+testdb)
                for cycle,backend in enumerate(['cuvs','vsag','cuvs']):
                    name='test_'+str(cycle)
                    cur.execute(f'CREATE TABLE {name}(id BIGINT PRIMARY KEY,v VECTOR(16))')
                    cur.executemany(f'INSERT INTO {name} VALUES(%s,%s)',[(i,text_vector(row)) for i,row in enumerate(b)])
                    cur.execute(f'CREATE VECTOR INDEX idx ON {name}(v) WITH(distance=l2,type=hnsw,lib={backend},m=16,ef_construction=200,ef_search=64)')
                    cur.execute(f'SELECT id FROM {name} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(probe),))
                    ids=checked(cur.fetchall(),10,512);recall10=len(set(ids)&set(truth))/10;assert recall10>=0.9
                    cur.execute(f'INSERT INTO {name} VALUES(512,%s)',(text_vector(probe),))
                    cur.execute(f'SELECT id FROM {name} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(probe),))
                    ids=checked(cur.fetchall(),10,513);assert 512 in ids
                    cur.execute(f'DELETE FROM {name} WHERE id=512')
                    cur.execute(f'SELECT id FROM {name} ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(text_vector(probe),))
                    ids=checked(cur.fetchall(),10,512)
                    cycles.append({'backend':backend,'recall10':recall10,'insert_delete':'PASS'})
                result['lifecycle']=cycles
            print('LIFECYCLE',cycles,flush=True)
        conn.close()
    finally:
        server.terminate()
        try:server.wait(timeout=45)
        except subprocess.TimeoutExpired:server.kill();server.wait()
        log.close()
(w/'sql_snapshot_functional.json').write_text(json.dumps({'status':'PASS','results':results},indent=2))
print('SNAPSHOT SQL FUNCTIONAL PASS',flush=True)
