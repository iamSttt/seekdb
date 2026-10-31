import subprocess,sys,time,os,shutil,hashlib,json
from pathlib import Path
w=Path(__file__).resolve().parent;repo=next((w/'source').iterdir())
while 'EXTRA COMPLETE' not in (w/'extra_v2.log').read_text():time.sleep(3)
shutil.copyfile(w/'candidate96_final.c',repo/'tools/hipvs_bridge/seekdb_cuvs_bridge.c')
with (w/'build_final.log').open('w') as log:r=subprocess.run([sys.executable,str(w/'build_full.py')],stdout=log,stderr=subprocess.STDOUT)
assert r.returncode==0,'final build failed'
(w/'final96').mkdir(exist_ok=True);shutil.copyfile(w/'build/libseekdb_cuvs_bridge.so',w/'final96/libseekdb_cuvs_bridge.so')
(w/'final_source_sha.txt').write_text(hashlib.sha256((repo/'tools/hipvs_bridge/seekdb_cuvs_bridge.c').read_bytes()).hexdigest())
p=(w/'sql_pair.py').read_text().replace("[('normal256',['candidate96','frozen']),('normal128',['frozen','candidate96'])]","[('demo',['vsag','cuvs'])]").replace("str(w/label)","str(w/'final96')").replace("str(w/label/'libseekdb_cuvs_bridge.so')","str(w/'final96/libseekdb_cuvs_bridge.so')").replace("'--backend','cuvs'","'--backend',label").replace("'--queries','4096'","'--queries','100'")
(w/'sql_demo_pair.py').write_text(p)
with (w/'sql_demo_pair.log').open('w') as log:r=subprocess.run([sys.executable,str(w/'sql_demo_pair.py')],stdout=log,stderr=subprocess.STDOUT)
assert r.returncode==0,'CPU/GPU SQL functional test failed'
print('POST COMPLETE',flush=True)
