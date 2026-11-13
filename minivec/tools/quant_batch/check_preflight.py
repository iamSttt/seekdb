"""Pure Python checks only. This does not compile or execute the C++ prototype."""
import ast
import hashlib
import json
import random
from pathlib import Path

w = Path(__file__).resolve().parent
for name in ('run_linux.py', 'check_preflight.py', 'freeze_manifest.py'):
    ast.parse((w/name).read_text(encoding='utf8'))
reference = json.loads((w/'reference-manifest.json').read_bytes())
for name, expected in reference['files'].items():
    raw = (w/'reference'/name).read_bytes()
    assert len(raw) == expected['bytes']
    assert hashlib.sha256(raw).hexdigest() == expected['sha256']
    assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest() == expected['git_blob']
bound_checks = 0
for n in range(65537):
    assert 65025*n < 2**32
    assert (n//16)*2*65025 < 2**31
    bound_checks += 2
rng = random.Random(0x8234)
model_checks = 0
for n in (0, 1, 15, 16, 17, 31, 32, 127, 128, 129, 511, 512, 513, 2049):
    query = [rng.randrange(-128, 128) for _ in range(n)]
    rows = [[rng.randrange(-128, 128) for _ in range(n)] for _ in range(8)]
    lanes = [[0]*8 for _ in range(8)]
    stop = n//16*16
    for start in range(0, stop, 16):
        q = query[start:start+16]
        for r in range(8):
            for lane in range(8):
                lanes[r][lane] += sum((rows[r][start+2*lane+j]-q[2*lane+j])**2 for j in (0, 1))
    for r in range(8):
        model = sum(lanes[r])+sum((rows[r][i]-query[i])**2 for i in range(stop, n))
        assert model == sum((x-y)**2 for x, y in zip(rows[r], query))
        model_checks += 1
result = {'status': 'pure_python_preflight_passed', 'range_bound_checks': bound_checks,
          'independent_lane_model_rows': model_checks, 'reference_byte_binding': 'passed',
          'native_compilations': 0, 'native_executions': 0, 'database_runs': 0, 'pgo_builds': 0,
          'limitations': 'Python arithmetic model and syntax only; native C++, SIMD code generation and timing remain unexecuted.'}
(w/'preflight.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result))
