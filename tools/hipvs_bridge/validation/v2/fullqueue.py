import subprocess,sys,time
from pathlib import Path
w=Path(__file__).resolve().parent
deadline=time.monotonic()+900
while not (w/'full/normal128_truth.npy').exists():
 if time.monotonic()>deadline:raise RuntimeError('data timeout')
 time.sleep(2)
for dataset,labels in [('normal256',['candidate128','frozen','candidate64']),('normal128',['frozen','candidate128','candidate64'])]:
 for label in labels:
  tag='full_'+dataset+'_'+label
  print('START',tag,flush=True)
  with (w/(tag+'.log')).open('w') as out:
   p=subprocess.run([sys.executable,str(w/'bench_bridge.py'),'--library',str(w/label/'libseekdb_cuvs_bridge.so'),'--data',str(w/'full'),'--dataset',dataset,'--output',str(w/tag),'--queries','4096'],stdout=out,stderr=subprocess.STDOUT,timeout=240)
  print('DONE',tag,p.returncode,flush=True)
print('COMPLETE',flush=True)
