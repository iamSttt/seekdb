import subprocess,sys,os,time
from pathlib import Path
w=Path(__file__).resolve().parent
for dataset in ['normal256','normal128']:
 while not (w/(dataset+'_truth.npy')).exists(): time.sleep(1)
 for label,a,g,i,n in [('baseline',2,128,256,100),('submitted',2,128,192,100),('iterative',3,128,128,20),('nn128_30',2,128,128,30),('nn128_50',2,128,128,50),('nn192_30',2,128,192,30),('nn64_30',2,64,128,30)]:
  print('START',dataset,label,flush=True)
  with (w/(label+'_'+dataset+'.log')).open('w') as out:
   try: p=subprocess.run([sys.executable,str(w/'screen.py'),label,dataset,str(a),str(g),str(i),str(n)],stdout=out,stderr=subprocess.STDOUT,timeout=240);print('DONE',dataset,label,p.returncode,flush=True)
   except subprocess.TimeoutExpired: print('TIMEOUT',dataset,label,flush=True)
print('COMPLETE',flush=True)
