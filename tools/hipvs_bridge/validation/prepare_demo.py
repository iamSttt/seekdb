"""Prepare the template's 10K FP32 demo data and recompute top-100 truth."""
import argparse
from pathlib import Path
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--input', type=Path, default=Path('/seekdb_workspace/datasets'))
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
b = np.fromfile(a.input / 'base.f32', np.float32).reshape(-1, 128)
q = np.fromfile(a.input / 'query.f32', np.float32).reshape(-1, 128)
if len(b) < 100 or not len(q):
    raise ValueError('insufficient demo data')
a.output.mkdir(parents=True, exist_ok=True)
truth = np.empty((len(q), 100), np.int32)
bn = (b * b).sum(1)
for start in range(0, len(q), 64):
    z = q[start:start + 64]
    d = (z * z).sum(1)[:, None] + bn[None, :] - 2 * z @ b.T
    truth[start:start + len(z)] = np.argsort(d, axis=1)[:, :100]
for suffix, data in [('base', b), ('query', q), ('truth', truth)]:
    np.save(a.output / ('demo_' + suffix + '.npy'), data)
print('Prepared template demo:', b.shape, q.shape)
