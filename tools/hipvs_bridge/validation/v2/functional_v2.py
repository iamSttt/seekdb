"""Validate candidate ABI and batched search independently of scored timings."""
import os, ctypes as C, json
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import numpy as np
w=Path(__file__).resolve().parent
os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:24])
q=np.load(w/'full/normal256_query.npy'); b=np.load(w/'full/normal256_base.npy')
fp=C.POINTER(C.c_float);up=C.POINTER(C.c_uint)
lib=C.CDLL(str(w/'candidate96/libseekdb_cuvs_bridge.so'))
lib.seekdb_cuvs_deserialize.argtypes=[C.c_char_p,C.c_long,C.c_long];lib.seekdb_cuvs_deserialize.restype=C.c_void_p
lib.seekdb_cuvs_search.argtypes=[C.c_void_p,fp,C.c_long,C.c_long,up,fp];lib.seekdb_cuvs_search.restype=C.c_int
lib.seekdb_cuvs_free.argtypes=[C.c_void_p];lib.seekdb_cuvs_free.restype=None
h=lib.seekdb_cuvs_deserialize(str(w/'final_bridge_normal256_candidate96.snapshot').encode(),len(b),b.shape[1])
assert h,'reload failed'
def call(nq,k,distances=True):
    ids=np.full((nq,k),2**32-1,np.uint32);ds=np.full((nq,k),np.nan,np.float32)
    rc=lib.seekdb_cuvs_search(h,q.ctypes.data_as(fp),nq,k,ids.ctypes.data_as(up),ds.ctypes.data_as(fp) if distances else None)
    assert rc==0,(nq,k,rc)
    assert np.all(ids<len(b)) and all(len(set(row.tolist()))==k for row in ids)
    if distances:
        assert np.isfinite(ds).all()
        # Check the FP32 squared-L2 output against returned vector IDs.
        for i in range(min(nq,8)):
            expected=np.sum((b[ids[i]]-q[i])**2,axis=1)
            assert np.allclose(ds[i],expected,rtol=2e-5,atol=2e-4),(nq,k,'distance')
    print('PASS',nq,k,distances,flush=True)
try:
    cases=[(1,10),(1,100),(8,100),(128,100),(1024,100),(4096,100),(3,10)]
    for nq,k in cases:call(nq,k)
    call(1,100,False)
    with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(lambda _:call(1,100),range(16)))
    result={'status':'PASS','batch_cases':cases,'null_distance':'pass','concurrent_calls':16,'workers':4,'distance_check':'FP32 squared L2 versus returned IDs'}
    (w/'functional_v2.json').write_text(json.dumps(result,indent=2))
finally:lib.seekdb_cuvs_free(h)
