import os
os.environ['OPENBLAS_NUM_THREADS']='24'
os.environ['OMP_NUM_THREADS']='24'
from pathlib import Path
import numpy as np
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
w=Path(__file__).resolve().parent
rng=np.random.default_rng(20261031)
for dim in (256,128):
    name='normal'+str(dim)
    b=rng.standard_normal((250000,dim),dtype=np.float32)
    q=rng.standard_normal((256,dim),dtype=np.float32)
    truth=[];bn=np.einsum('ij,ij->i',b,b)
    for start in range(0,len(q),32):
        z=q[start:start+32];dist=np.einsum('ij,ij->i',z,z)[:,None]+bn[None,:]-2*z@b.T
        p=np.argpartition(dist,99,axis=1)[:,:100]
        p=np.take_along_axis(p,np.argsort(np.take_along_axis(dist,p,axis=1),axis=1),axis=1)
        truth.append(p.astype(np.uint32))
    np.save(w/(name+'_base.npy'),b);np.save(w/(name+'_query.npy'),q);np.save(w/(name+'_truth.npy'),np.concatenate(truth))
    print('READY',name,flush=True)
