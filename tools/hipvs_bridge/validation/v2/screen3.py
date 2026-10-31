"""Development-only CAGRA parameter screening; never an official score."""
import os, sys, time, json, ctypes as C, resource
from pathlib import Path
import numpy as np
resource.setrlimit(resource.RLIMIT_STACK, (64*1024*1024, resource.getrlimit(resource.RLIMIT_STACK)[1]))
os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:24])
w=Path(__file__).resolve().parent
label, dataset, algo, degree, inter, iterations=sys.argv[1:7]
algo,degree,inter,iterations=map(int,(algo,degree,inter,iterations))
b=np.load(w/(dataset+'_base.npy')); q=np.load(w/(dataset+'_query.npy')); gt=np.load(w/(dataset+'_truth.npy'))
lib=C.CDLL(str(w/'screen.so')); fp=C.POINTER(C.c_float); up=C.POINTER(C.c_uint)
lib.seekdb_cuvs_build.argtypes=[fp,C.c_long,C.c_long];lib.seekdb_cuvs_build.restype=C.c_void_p
lib.seekdb_cuvs_search.argtypes=[C.c_void_p,fp,C.c_long,C.c_long,up,fp];lib.seekdb_cuvs_search.restype=C.c_int
lib.seekdb_cuvs_free.argtypes=[C.c_void_p]
lib.bench_config.argtypes=[C.c_int]*9
hip=C.CDLL('/opt/rocm/lib/libamdhip64.so')
def config(itopk=512,width=4,team=0,block=0,search=3):
    lib.bench_config(algo,degree,inter,iterations,itopk,width,team,block,search)
def sync():
    assert hip.hipDeviceSynchronize()==0
config(); t=time.perf_counter(); h=lib.seekdb_cuvs_build(b.ctypes.data_as(fp),len(b),b.shape[1]);sync()
assert h,'build failed'
r={'label':label,'dataset':dataset,'build_s':time.perf_counter()-t,'build_params':[algo,degree,inter,iterations],'queries':len(q),'results':[]}
print('BUILD',json.dumps(r),flush=True)
ids=np.empty(100,np.uint32);ds=np.empty(100,np.float32)
try:
    choices=[(2048,1),(4096,1)]
    for budget,width in choices:
        config(budget,width); row={'itopk':budget,'width':width}
        for k in (10,100):
            matched=0; elapsed=0
            for i in range(len(q)):
                t=time.perf_counter();rc=lib.seekdb_cuvs_search(h,q[i].ctypes.data_as(fp),1,k,ids.ctypes.data_as(up),ds.ctypes.data_as(fp));sync();elapsed+=time.perf_counter()-t
                assert rc==0 and len(set(ids[:k].tolist()))==k and np.all(ids[:k]<len(b)) and np.isfinite(ds[:k]).all(),(rc,i,k)
                matched+=len(set(ids[:k].tolist())&set(gt[i,:k].tolist()))
            row['recall'+str(k)]=matched/(len(q)*k);row['qps'+str(k)]=len(q)/elapsed
        r['results'].append(row);print('QUERY',json.dumps(row),flush=True)
        (w/(label+'_'+dataset+'.json')).write_text(json.dumps(r,indent=2))
finally:
    lib.seekdb_cuvs_free(h)
