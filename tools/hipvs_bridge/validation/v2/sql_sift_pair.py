"""Independent fresh-instance SQL pairs. Development-only, not official scoring."""
import os, sys, subprocess, time, json, threading, signal
from pathlib import Path
import pymysql
w=Path(__file__).resolve().parent
repo=next((w/'source').iterdir()); binary=repo/'build_release/src/observer/seekdb'
cpus=sorted(os.sched_getaffinity(0))[:24];os.sched_setaffinity(0,cpus)
deadline=time.monotonic()+1800
while not (w/'full/sift250k_truth.npy').exists():
    if time.monotonic()>deadline:raise RuntimeError('build/bridge verification timeout')
    time.sleep(3)
for dataset,labels in [('sift250k',['frozen','candidate96'])]:
    for label in labels:
        tag='sql_'+dataset+'_'+label; base=w/'instances'/tag
        base.mkdir(parents=True,exist_ok=False)
        env=os.environ.copy();env['LD_LIBRARY_PATH']=str(w/label)+':/opt/hipvs/lib:/opt/rocm/lib'
        command=[str(binary),'--base-dir='+str(base),'--port=2881','--nodaemon','--parameter','_memory_budget=16G','--parameter','datafile_size=4G','--parameter','datafile_maxsize=8G','--parameter','log_disk_size=2G','--log-level','INFO']
        print('START',tag,flush=True)
        out=(w/(tag+'_server.log')).open('w')
        server=subprocess.Popen(command,cwd=base,env=env,stdout=out,stderr=subprocess.STDOUT)
        stop=threading.Event(); mem={'vram_peak_sampled':0,'server_rss_peak_sampled':0,'interval_seconds':0.05}
        def sample():
            while not stop.is_set():
                try:
                    paths=list(Path('/sys/class/drm').glob('card[0-9]*/device/mem_info_vram_used'))
                    mem['vram_peak_sampled']=max(mem['vram_peak_sampled'],sum(int(p.read_text()) for p in paths))
                    for line in Path(f'/proc/{server.pid}/status').read_text().splitlines():
                        if line.startswith('VmRSS:'):mem['server_rss_peak_sampled']=max(mem['server_rss_peak_sampled'],int(line.split()[1])*1024)
                except (OSError,ValueError):pass
                stop.wait(0.05)
        monitor=threading.Thread(target=sample);monitor.start()
        try:
            ready=False
            for _ in range(180):
                if server.poll() is not None:raise RuntimeError('server exited '+str(server.returncode))
                try:
                    conn=pymysql.connect(host='127.0.0.1',port=2881,user='root',connect_timeout=2);conn.close();ready=True;break
                except pymysql.MySQLError:time.sleep(1)
            if not ready:raise RuntimeError('server startup timeout')
            maps=Path(f'/proc/{server.pid}/maps').read_text()
            assert str(w/label/'libseekdb_cuvs_bridge.so') in maps,'wrong bridge loaded'
            (w/(tag+'_loaded_library.txt')).write_text('\n'.join(x for x in maps.splitlines() if 'libseekdb_cuvs_bridge' in x))
            with (w/(tag+'.log')).open('w') as log:
                result=subprocess.run([sys.executable,str(w/'bench_sql.py'),'--data',str(w/'full'),'--dataset',dataset,'--backend','cuvs','--output',str(w/tag),'--queries','4096'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
            print('DONE',tag,result.returncode,flush=True)
            if result.returncode:raise RuntimeError((w/(tag+'.log')).read_text()[-2500:])
        finally:
            stop.set();monitor.join();(w/(tag+'_memory.json')).write_text(json.dumps(mem,indent=2))
            server.terminate()
            try:server.wait(timeout=45)
            except subprocess.TimeoutExpired:server.kill();server.wait()
            out.close()
print('SQL PAIRS COMPLETE',flush=True)
