import os,sys,json,time,ctypes as C,resource,hashlib
from pathlib import Path
import numpy as np
resource.setrlimit(resource.RLIMIT_STACK,(64*1024*1024,resource.getrlimit(resource.RLIMIT_STACK)[1]))
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
import argparse
parser=argparse.ArgumentParser(description='CAGRA bridge microbenchmark; not official SQL scoring')
parser.add_argument('--library',type=Path,required=True)
parser.add_argument('--data',type=Path,required=True)
parser.add_argument('--dataset',required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--queries',type=int,default=4096)
a=parser.parse_args()
w=a.data; label=a.library.parent.name; name=a.dataset; tag=a.output.name
a.output.parent.mkdir(parents=True,exist_ok=True)
if a.queries < 1: raise ValueError('queries must be positive')
b=np.load(w/(name+'_base.npy'));q=np.load(w/(name+'_query.npy'));gt=np.load(w/(name+'_truth.npy'));limit=a.queries;q=q[:limit];gt=gt[:limit]
assert b.dtype==q.dtype==np.float32 and b.flags.c_contiguous and q.flags.c_contiguous
assert b.ndim==q.ndim==2 and b.shape[1]==q.shape[1] and len(b)>=100
assert len(q)==len(gt)>0 and gt.shape[1]>=100
lib=C.CDLL(str(a.library.resolve()));fp=C.POINTER(C.c_float);up=C.POINTER(C.c_uint)
lib.seekdb_cuvs_build.argtypes=[fp,C.c_long,C.c_long];lib.seekdb_cuvs_build.restype=C.c_void_p
lib.seekdb_cuvs_search.argtypes=[C.c_void_p,fp,C.c_long,C.c_long,up,fp];lib.seekdb_cuvs_search.restype=C.c_int
lib.seekdb_cuvs_free.argtypes=[C.c_void_p];lib.seekdb_cuvs_free.restype=None
lib.seekdb_cuvs_serialize.argtypes=[C.c_void_p,C.c_char_p];lib.seekdb_cuvs_serialize.restype=C.c_int
lib.seekdb_cuvs_deserialize.argtypes=[C.c_char_p,C.c_long,C.c_long];lib.seekdb_cuvs_deserialize.restype=C.c_void_p
hip=C.CDLL('/opt/rocm/lib/libamdhip64.so');hip.hipDeviceSynchronize.restype=C.c_int
def sync():
    rc=hip.hipDeviceSynchronize()
    if rc:raise RuntimeError('HIP sync '+str(rc))
ids=np.empty(100,np.uint32);ds=np.empty(100,np.float32)
def search(h,i,k):
    rc=lib.seekdb_cuvs_search(h,q[i].ctypes.data_as(fp),1,k,ids.ctypes.data_as(up),ds.ctypes.data_as(fp));sync()
    if rc:raise RuntimeError('search '+str(rc))
    if len(set(ids[:k].tolist()))!=k or np.any(ids[:k]>=len(b)) or not np.isfinite(ds[:k]).all():raise RuntimeError('result integrity')
    return ids[:k].copy()
r={'label':label,'dataset':name,'tag':tag,'n':len(b),'dim':b.shape[1],'nq':len(q),'scope':'bridge only, not official SQL'};h=None
try:
    t=time.perf_counter();h=lib.seekdb_cuvs_build(b.ctypes.data_as(fp),len(b),b.shape[1]);sync()
    if not h:raise RuntimeError('build failed')
    r['build_s']=time.perf_counter()-t;print('BUILT',r,flush=True)
    search(h,0,10);r['build_first_s']=time.perf_counter()-t
    snap=a.output.with_suffix('.snapshot');t=time.perf_counter()
    if lib.seekdb_cuvs_serialize(h,str(snap).encode()):raise RuntimeError('serialize failed')
    sync();r['serialize_s']=time.perf_counter()-t;r['snapshot_bytes']=snap.stat().st_size
    before=search(h,0,100);lib.seekdb_cuvs_free(h);h=None;t=time.perf_counter()
    h=lib.seekdb_cuvs_deserialize(str(snap).encode(),len(b),b.shape[1]);sync()
    if not h:raise RuntimeError('reload failed')
    r['reload_s']=time.perf_counter()-t;after=search(h,0,100);r['snapshot_same']=bool(np.array_equal(before,after))
    if not r['snapshot_same']: raise RuntimeError('snapshot round trip changed search results')
    for k in (10,100):
        matched=0;times=[]
        for i in range(len(q)):
            t=time.perf_counter();out=search(h,i,k);times.append(time.perf_counter()-t);matched+=len(set(out.tolist())&set(gt[i,:k].tolist()))
        r['recall'+str(k)]=matched/(len(q)*k);r['qps'+str(k)]=len(q)/sum(times);r['p95_ms'+str(k)]=float(np.percentile(times,95)*1000)
    r['integrity']='pass';r['library_sha256']=hashlib.sha256(a.library.read_bytes()).hexdigest()
    a.output.with_suffix('.json').write_text(json.dumps(r,indent=2));print('RESULT',json.dumps(r),flush=True)
finally:
    if h:lib.seekdb_cuvs_free(h)
