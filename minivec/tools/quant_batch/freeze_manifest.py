"""Freeze exact prototype deliverables; does not execute native code."""
import hashlib
import json
from pathlib import Path

w = Path(__file__).resolve().parent
files = {p.relative_to(w).as_posix(): {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
         for p in sorted(w.rglob('*')) if p.is_file() and p.name != 'manifest.json' and '__pycache__' not in p.parts}
result = {'schema_version': 1, 'kind': 'standalone_batch8_quantized_distance_prototype',
          'status': 'prepared_not_native_tested', 'batch_rows': 8, 'database_runs': 0,
          'pgo_builds': 0, 'native_compilations': 0, 'native_executions': 0, 'files': files}
(w/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
raw = (w/'manifest.json').read_bytes()
print(json.dumps({'path': str(w/'manifest.json'), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(), 'files': len(files)}))
