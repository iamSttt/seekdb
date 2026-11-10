#!/usr/bin/env bash
# Compare immutable, previously validated JSONL source with the candidate image.
set -euo pipefail
baseline=1419648efd4bfb3abc5753403343790d79ef2b76
root=$(git rev-parse --show-toplevel)
cd "$root"
if ! git cat-file -e "$baseline^{commit}" 2>/dev/null; then
  git fetch --depth=1 origin "$baseline"
fi
context=$(mktemp -d)
trap 'rm -rf -- "$context"' EXIT
git archive "$baseline" | tar -x -C "$context"
docker build --platform linux/amd64 -t minivec:jsonl-baseline "$context"
for seed in 38021 7319 19417; do
  if [ "$seed" = 7319 ]; then order='candidate baseline'; else order='baseline candidate'; fi
  for version in $order; do
    image=minivec:jsonl
    if [ "$version" = baseline ]; then image=minivec:jsonl-baseline; fi
    python3 minivec/tools/benchmark_jsonl.py --image "$image" --seed "$seed" \
      --output "protocol-results/benchmark-$version-$seed.json"
  done
done
python3 - <<'PY'
import json
from pathlib import Path
from statistics import median
ratios = []
for seed in (38021, 7319, 19417):
    base = json.loads(Path(f'protocol-results/benchmark-baseline-{seed}.json').read_text())
    candidate = json.loads(Path(f'protocol-results/benchmark-candidate-{seed}.json').read_text())
    ratios.append(base['query']['exact']['total_seconds'] / candidate['query']['exact']['total_seconds'])
summary = {'baseline_commit': '1419648efd4bfb3abc5753403343790d79ef2b76',
           'exact_query_speedups': ratios, 'median_exact_query_speedup': median(ratios),
           'scope': 'Local synthetic, paired on one runner with identical Docker resource limits; not a platform score.'}
Path('protocol-results/paired-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))
PY
