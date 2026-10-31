import subprocess,sys,time,os
from pathlib import Path
w=Path(__file__).resolve().parent
while 'SQL PAIRS COMPLETE' not in (w/'sql_sift_pair.log').read_text():time.sleep(3)
for label,cmd in [('functional',[sys.executable,str(w/'functional_v2.py')]),('demo_prepare',[sys.executable,str(w/'prepare_demo.py'),'--output',str(w/'full')]),('profile',['/opt/rocm/bin/rocprofv3','--kernel-trace','--stats','-d',str(w/'profile_v2'),'--',sys.executable,str(w/'bench_roundtrip.py'),'--library',str(w/'candidate96/libseekdb_cuvs_bridge.so'),'--data',str(w/'full'),'--dataset','sift250k','--output',str(w/'profiled_sift'),'--queries','16'])]:
 print('START',label,flush=True)
 with (w/('extra_'+label+'.log')).open('w') as out:r=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,timeout=300)
 print('DONE',label,r.returncode,flush=True)
print('EXTRA COMPLETE',flush=True)
