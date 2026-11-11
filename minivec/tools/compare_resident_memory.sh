#!/usr/bin/env bash
# Same-source diagnostic: isolate borrowed row payloads from CRC/query changes.
set -euo pipefail
python3 - <<'PY'
from pathlib import Path
source = Path('Dockerfile').read_text()
needle = 'g++ -O3 '
assert source.count(needle) == 1
Path('protocol-results/Dockerfile-owned').write_text(
    source.replace(needle, needle + '-DMVDB_DISABLE_BORROWED_VECTORS '))
PY
docker build --platform linux/amd64 --file protocol-results/Dockerfile-owned -t minivec:jsonl-owned .
for seed in 38021 7319 19417; do
  if [ "$seed" = 7319 ]; then order='borrowed owned'; else order='owned borrowed'; fi
  for version in $order; do
    image=minivec:jsonl
    if [ "$version" = owned ]; then image=minivec:jsonl-owned; fi
    python3 minivec/tools/benchmark_jsonl.py --image "$image" --seed "$seed" \
      --output "protocol-results/resident-$version-$seed.json"
  done
done
python3 - <<'PY'
import json
from pathlib import Path
from statistics import median
rows = []
for seed in (38021, 7319, 19417):
    pair = {v: json.loads(Path(f'protocol-results/resident-{v}-{seed}.json').read_text())
            for v in ('owned', 'borrowed')}
    row = {'seed': seed, 'query_speedup': pair['owned']['query']['exact']['total_seconds'] /
           pair['borrowed']['query']['exact']['total_seconds']}
    for v, report in pair.items():
        snapshots = report['container_profile']['snapshots']
        row[v + '_sampled_peak_bytes'] = max(
            (s.get('memory_peak_bytes') or 0) for s in snapshots)
        row[v + '_query_seconds'] = report['query']['exact']['total_seconds']
        row[v + '_ingest_seconds'] = report['ingest_seconds']
    rows.append(row)
summary = {'kind': 'resident_memory_same_source_comparison', 'samples': rows,
           'median_query_speedup': median(r['query_speedup'] for r in rows),
           'scope': 'Only MVDB_DISABLE_BORROWED_VECTORS differs. Sampled container peaks are not guaranteed final writeback metrics; zero denotes missing data, not zero memory cost.'}
Path('protocol-results/resident-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))
PY
