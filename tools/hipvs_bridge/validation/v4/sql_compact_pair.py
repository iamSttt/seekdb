"""Same-machine construction pairs; full SQL queries and repeated fresh graphs."""
import json, os, subprocess, sys, threading, time
from pathlib import Path
import numpy as np
import pymysql
w=Path(__file__).resolve().parent
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
def connect():
    return pymysql.connect(host='127.0.0.1',port=2881,user='root',autocommit=True,read_timeout=900)
deadline=time.monotonic()+3600
while not (w/'build_compact.pass').exists() or not (w/'full/sift250k_truth.npy').exists() or not (w/'full/normal256_truth.npy').exists():
    if time.monotonic()>deadline: raise RuntimeError('build/data preparation timed out')
    time.sleep(3)
for dataset,labels in [('normal256',['v3','compact']),('sift250k',['compact','v3'])]:
    for label in labels:
        tag=f'sql4_{dataset}_{label}';base=w/'instances'/tag;base.mkdir(parents=True,exist_ok=False)
        binary=w/('seekdb_v3' if label=='v3' else 'seekdb_compact')
        bridge=w/('build' if label=='v3' else 'compact')
        env=os.environ.copy();env['LD_LIBRARY_PATH']=str(bridge)+':/opt/hipvs/lib:/opt/rocm/lib'
        out=(w/(tag+'_server.log')).open('w')
        cmd=[str(binary),'--base-dir='+str(base),'--port=2881','--nodaemon','--parameter','_memory_budget=16G','--parameter','datafile_size=4G','--parameter','datafile_maxsize=8G','--parameter','log_disk_size=2G','--log-level','INFO']
        server=subprocess.Popen(cmd,cwd=base,env=env,stdout=out,stderr=subprocess.STDOUT)
        stop=threading.Event();mem={'vram_peak_sampled':0,'server_rss_peak_sampled':0,'interval_seconds':0.05}
        def sample():
            while not stop.is_set():
                try:
                    mem['vram_peak_sampled']=max(mem['vram_peak_sampled'],sum(int(p.read_text()) for p in Path('/sys/class/drm').glob('card[0-9]*/device/mem_info_vram_used')))
                    for line in Path(f'/proc/{server.pid}/status').read_text().splitlines():
                        if line.startswith('VmRSS:'):mem['server_rss_peak_sampled']=max(mem['server_rss_peak_sampled'],int(line.split()[1])*1024)
                except (OSError,ValueError):pass
                stop.wait(0.05)
        monitor=threading.Thread(target=sample);monitor.start()
        try:
            for _ in range(180):
                assert server.poll() is None,'server exited'
                try:conn=connect();conn.close();break
                except pymysql.MySQLError:time.sleep(1)
            else:raise RuntimeError('startup timed out')
            maps=Path(f'/proc/{server.pid}/maps').read_text();assert str(bridge/'libseekdb_cuvs_bridge.so') in maps
            (w/(tag+'_loaded_library.txt')).write_text('\n'.join(x for x in maps.splitlines() if 'libseekdb_cuvs_bridge' in x))
            print('START',tag,flush=True)
            with (w/(tag+'.log')).open('w') as log:
                p=subprocess.run([sys.executable,str(w/'bench_sql.py'),'--data',str(w/'full'),'--dataset',dataset,'--backend','cuvs','--output',str(w/tag),'--queries','4096'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
            if p.returncode:raise RuntimeError((w/(tag+'.log')).read_text()[-4000:])
            record=json.loads((w/(tag+'.json')).read_text());print('RESULT',record,flush=True)
            q=np.load(w/'full'/f'{dataset}_query.npy');gt=np.load(w/'full'/f'{dataset}_truth.npy')
            qt=['['+','.join(format(float(x),'.9g') for x in row)+']' for row in q[:128]]
            repeats=[];conn=connect()
            with conn.cursor() as cur:
                cur.execute('SET SESSION ob_query_timeout=900000000');cur.execute('USE '+record['database'])
                for r in range(3):
                    cur.execute('DROP INDEX idx_items ON items')
                    start=time.perf_counter()
                    cur.execute('CREATE VECTOR INDEX idx_items ON items(v) WITH(distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64)')
                    ddl=time.perf_counter()-start
                    cur.execute('SELECT id FROM items ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 10',(qt[0],));ids=[int(row[0]) for row in cur.fetchall()]
                    full=time.perf_counter()-start;assert len(ids)==len(set(ids))==10 and all(0<=x<250000 for x in ids)
                    matched=0
                    for i,query in enumerate(qt):
                        cur.execute('SELECT id FROM items ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 100',(query,));ids=[int(row[0]) for row in cur.fetchall()]
                        assert len(ids)==len(set(ids))==100 and all(0<=x<250000 for x in ids)
                        matched+=len(set(ids)&set(gt[i,:100]))
                    entry={'repeat':r,'ddl_s':ddl,'build_first_s':full,'recall100':matched/(len(qt)*100)}
                    repeats.append(entry);print('REBUILD',tag,entry,flush=True)
            conn.close();(w/(tag+'_repeats.json')).write_text(json.dumps(repeats,indent=2))
        finally:
            stop.set();monitor.join();(w/(tag+'_memory.json')).write_text(json.dumps(mem,indent=2))
            server.terminate()
            try:server.wait(timeout=45)
            except subprocess.TimeoutExpired:server.kill();server.wait()
            out.close()
            lines=[]
            for p in (base/'log').glob('seekdb.log*'):
                if p.is_file():
                    with p.open(errors='replace') as f:
                        lines.extend(line.rstrip() for line in f if 'GPU CAGRA snapshot' in line)
            (w/(tag+'_snapshot_stages.log')).write_text('\n'.join(lines))
print('SQL PAIRS COMPLETE',flush=True)
