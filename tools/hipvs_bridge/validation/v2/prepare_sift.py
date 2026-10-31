"""Public ANN-benchmarks SIFT prefix, not organizer-hidden evaluation data."""
import os
os.environ['OPENBLAS_NUM_THREADS']='24'
os.environ['OMP_NUM_THREADS']='24'
from pathlib import Path
import time, gzip, hashlib, json
import numpy as np
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
w=Path(__file__).resolve().parent; incoming=Path('/workspace/seekdb_hipvs_notebook');out=w/'full'
items=[('sift250k-base.f32.gz',62448158,'3c76517e6d27965d41a4a4533019cbe66bdaca6d9639952dc2410b42708898ad'),('sift4096-query.f32',2097152,'fb5b67515be0378ca691e11ab68c18100e19b9902e6f78b1f6b6e543d074ae04')]
deadline=time.monotonic()+1800
while any(not (incoming/n).exists() or (incoming/n).stat().st_size!=size for n,size,h in items) or 'SQL PAIRS COMPLETE' not in (w/'sql_pair.log').read_text():
    if time.monotonic()>deadline:raise RuntimeError('upload/SQL pair wait timeout')
    time.sleep(3)
for n,size,h in items:assert hashlib.sha256((incoming/n).read_bytes()).hexdigest()==h,n
b=np.frombuffer(gzip.decompress((incoming/items[0][0]).read_bytes()),dtype='<f4').reshape(250000,128).copy()
q=np.fromfile(incoming/items[1][0],dtype='<f4').reshape(4096,128)
assert np.isfinite(b).all() and np.isfinite(q).all() and 0<=b.min()<=b.max()<=255
bn=np.einsum('ij,ij->i',b,b);truth=[]
for start in range(0,len(q),32):
    z=q[start:start+32];dist=np.einsum('ij,ij->i',z,z)[:,None]+bn[None,:]-2*z@b.T
    p=np.argpartition(dist,99,axis=1)[:,:100]
    p=np.take_along_axis(p,np.argsort(np.take_along_axis(dist,p,axis=1),axis=1),axis=1)
    truth.append(p.astype(np.uint32))
np.save(out/'sift250k_base.npy',b);np.save(out/'sift250k_query.npy',q);np.save(out/'sift250k_truth.npy',np.concatenate(truth))
(w/'sift_provenance.json').write_text(json.dumps({'url':'https://ann-benchmarks.com/sift-128-euclidean.hdf5','file_size':525128288,'etag':'2bf1ef4c0c3e17031bd64980774eea09-63','base_rows':'first 250000 train rows','query_rows':'first 4096 test rows','train_byte_range':[6240,128006239],'query_byte_range':[512006240,514103391],'transport_sha256':dict((n,h) for n,size,h in items),'ground_truth':'FP32 exhaustive L2 against the selected 250K rows; not reused 1M ground truth','official_equivalence':'not established'},indent=2))
print('SIFT READY',b.shape,q.shape,flush=True)
