import os,time
from pathlib import Path
import numpy as np
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
import argparse
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
w=a.output;w.mkdir(parents=True,exist_ok=True);rng=np.random.default_rng(20261030)
for dim in (128,256):
    name='synthetic'+str(dim);b=rng.standard_normal((250000,dim),dtype=np.float32);q=rng.standard_normal((4096,dim),dtype=np.float32)
    np.save(w/(name+'_base.npy'),b);np.save(w/(name+'_query.npy'),q)
    gt=np.empty((len(q),100),np.int32);bn=(b*b).sum(1);t=time.perf_counter()
    for i in range(0,len(q),64):
        z=q[i:i+64];d=(z*z).sum(1)[:,None]+bn[None,:]-2*z@b.T
        ids=np.argpartition(d,99,axis=1)[:,:100];order=np.argsort(np.take_along_axis(d,ids,axis=1),axis=1);gt[i:i+len(z)]=np.take_along_axis(ids,order,axis=1)
        if i%1024==0:print(name,i,round(time.perf_counter()-t,2),flush=True)
    np.save(w/(name+'_truth.npy'),gt);print('READY',name,flush=True)
