"""Exercise the fixed-library dataset view, padding and snapshot recovery."""
import ctypes as C
import json
import os
import resource
from pathlib import Path
import numpy as np

resource.setrlimit(resource.RLIMIT_STACK, (64*1024*1024, resource.getrlimit(resource.RLIMIT_STACK)[1]))
os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:24])
w = Path(__file__).resolve().parent
lib = C.CDLL(str(w/'compact/libseekdb_cuvs_bridge.so'))
fp = C.POINTER(C.c_float)
lib.seekdb_cuvs_build.argtypes = [fp, C.c_long, C.c_long]
lib.seekdb_cuvs_build.restype = C.c_void_p
lib.seekdb_cuvs_free.argtypes = [C.c_void_p]
lib.seekdb_cuvs_copy_dataset.argtypes = [C.c_void_p, fp, C.c_long, C.c_long]
lib.seekdb_cuvs_copy_dataset.restype = C.c_int
lib.seekdb_cuvs_serialize.argtypes = [C.c_void_p, C.c_char_p]
lib.seekdb_cuvs_deserialize.argtypes = [C.c_char_p, C.c_long, C.c_long]
lib.seekdb_cuvs_deserialize.restype = C.c_void_p
rng = np.random.default_rng(20261031)
results = []
for n, dim in [(1024,16),(1024,127),(1024,128),(1024,256),(250000,256)]:
    b = rng.standard_normal((n,dim), dtype=np.float32)
    h = lib.seekdb_cuvs_build(b.ctypes.data_as(fp), n, dim)
    assert h
    staging_fd = None
    try:
        recovered = np.full(n*dim+32, np.float32(123456))
        dest = recovered[:n*dim].reshape(n,dim)
        assert lib.seekdb_cuvs_copy_dataset(h,dest.ctypes.data_as(fp),n,dim)==0
        assert np.array_equal(dest.view(np.uint32),b.view(np.uint32)), 'FP32 bits changed'
        assert np.all(recovered[n*dim:]==123456), 'buffer overrun'
        dest.fill(321)
        assert lib.seekdb_cuvs_copy_dataset(h,dest.ctypes.data_as(fp),n,dim+1)!=0
        assert np.all(dest==321), 'invalid shape wrote output'
        assert lib.seekdb_cuvs_copy_dataset(h,None,n,dim)!=0
        assert lib.seekdb_cuvs_copy_dataset(None,dest.ctypes.data_as(fp),n,dim)!=0
        staging_fd = os.memfd_create('seekdb-cuvs-copy-test',os.MFD_CLOEXEC)
        filename=f'/proc/self/fd/{staging_fd}'.encode()
        assert lib.seekdb_cuvs_serialize(h,filename)==0
        lib.seekdb_cuvs_free(h);h=None
        h=lib.seekdb_cuvs_deserialize(filename,n,dim);assert h
        assert lib.seekdb_cuvs_copy_dataset(h,dest.ctypes.data_as(fp),n,dim)==0
        assert np.array_equal(dest.view(np.uint32),b.view(np.uint32)), 'reload FP32 bits changed'
        results.append({'n':n,'dim':dim,'exact_fp32_roundtrip':True,'invalid_inputs':'pass','canary':'pass','memfd_snapshot':'pass'})
        print(results[-1],flush=True)
    finally:
        if h:lib.seekdb_cuvs_free(h)
        if staging_fd is not None:os.close(staging_fd)
(w/'test_dataset_copy.json').write_text(json.dumps({'status':'PASS','cases':results},indent=2))
print('DATASET COPY PASS',flush=True)
