import os
os.environ['OPENBLAS_NUM_THREADS']='24'
os.environ['OMP_NUM_THREADS']='24'
from pathlib import Path
import numpy as np
os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:24])
w=Path(__file__).resolve().parent; out=w/'full';out.mkdir(exist_ok=True)
rng=np.random.default_rng(20261101)
for dim in (256,128):
    name='normal'+str(dim);b=np.load(w/(name+'_base.npy'),mmap_mode='r')
    q=np.concatenate([np.load(w/(name+'_query.npy')),rng.standard_normal((3840,dim),dtype=np.float32)])
    bn=np.einsum('ij,ij->i',b,b);truth=[]
    for start in range(0,len(q),32):
        z=q[start:start+32]; dist=np.einsum('ij,ij->i',z,z)[:,None]+bn[None,:]-2*z@b.T
        p=np.argpartition(dist,99,axis=1)[:,:100]
        p=np.take_along_axis(p,np.argsort(np.take_along_axis(dist,p,axis=1),axis=1),axis=1)
        truth.append(p.astype(np.uint32))
    target=out/(name+'_base.npy')
    if not target.exists(): target.symlink_to(w/(name+'_base.npy'))
    np.save(out/(name+'_query.npy'),q);np.save(out/(name+'_truth.npy'),np.concatenate(truth))
    print('FULL_READY',name,flush=True)
