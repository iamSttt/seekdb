import subprocess,sys,os,time
from pathlib import Path
w=Path(__file__).resolve().parent
deadline=time.monotonic()+1800
while 'COMPLETE' not in (w/'queue.log').read_text():
 if time.monotonic()>deadline: raise RuntimeError('queue wait timeout')
 time.sleep(2)
for dataset in ['normal256','normal128']:
 while not (w/(dataset+'_truth.npy')).exists(): time.sleep(1)
 for label,a,g,i,n in [('wide_nn128_10',2,128,128,10),('wide_nn128_20',2,128,128,20),('wide_nn64_10',2,64,64,10),('wide_nn64_30',2,64,64,30)]:
  print('START',dataset,label,flush=True)
  with (w/(label+'_'+dataset+'.log')).open('w') as out:
   try: p=subprocess.run([sys.executable,str(w/'screen2.py'),label,dataset,str(a),str(g),str(i),str(n)],stdout=out,stderr=subprocess.STDOUT,timeout=240);print('DONE',dataset,label,p.returncode,flush=True)
   except subprocess.TimeoutExpired: print('TIMEOUT',dataset,label,flush=True)
print('COMPLETE',flush=True)
