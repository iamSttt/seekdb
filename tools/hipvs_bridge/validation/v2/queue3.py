import subprocess,sys,os,time
from pathlib import Path
w=Path(__file__).resolve().parent
deadline=time.monotonic()+1800
while 'COMPLETE' not in (w/'fullqueue.log').read_text():
 if time.monotonic()>deadline: raise RuntimeError('queue wait timeout')
 time.sleep(2)
for dataset in ['normal256','normal128']:
 while not (w/(dataset+'_truth.npy')).exists(): time.sleep(1)
 for label,a,g,i,n in [('fast128_5',2,128,128,5),('fast96_10',2,96,96,10),('fast96_5',2,96,96,5)]:
  print('START',dataset,label,flush=True)
  with (w/(label+'_'+dataset+'.log')).open('w') as out:
   try: p=subprocess.run([sys.executable,str(w/'screen3.py'),label,dataset,str(a),str(g),str(i),str(n)],stdout=out,stderr=subprocess.STDOUT,timeout=240);print('DONE',dataset,label,p.returncode,flush=True)
   except subprocess.TimeoutExpired: print('TIMEOUT',dataset,label,flush=True)
print('COMPLETE',flush=True)
