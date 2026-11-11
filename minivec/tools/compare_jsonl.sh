#!/usr/bin/env bash
# Compare immutable, previously validated JSONL source with the candidate image.
set -euo pipefail
baseline=ff9c6bfbe89e1045e12ac56652165245c834630c
previous=788b1677bf266877526f01394822bf031ed55a0a
root=$(git rev-parse --show-toplevel)
cd "$root"
context=$(mktemp -d)
trap 'rm -rf -- "$context"' EXIT
for version in baseline previous; do
  ref=$baseline
  if [ "$version" = previous ]; then ref=$previous; fi
  if ! git cat-file -e "$ref^{commit}" 2>/dev/null; then
    git fetch --depth=1 origin "$ref"
  fi
  mkdir "$context/$version"
  git archive "$ref" | tar -x -C "$context/$version"
  docker build --platform linux/amd64 -t "minivec:jsonl-$version" "$context/$version"
done
for seed in 38021 7319 19417; do
  case "$seed" in
    38021) order='baseline previous candidate';;
    7319) order='candidate baseline previous';;
    19417) order='previous candidate baseline';;
  esac
  for version in $order; do
    image=minivec:jsonl
    if [ "$version" != candidate ]; then image=minivec:jsonl-$version; fi
    python3 minivec/tools/benchmark_jsonl.py --image "$image" --seed "$seed" \
      --output "protocol-results/benchmark-$version-$seed.json"
  done
done
python3 - <<'PY'
import json
from pathlib import Path
from statistics import median
ratios = []
previous_ratios = []
for seed in (38021, 7319, 19417):
    base = json.loads(Path(f'protocol-results/benchmark-baseline-{seed}.json').read_text())
    candidate = json.loads(Path(f'protocol-results/benchmark-candidate-{seed}.json').read_text())
    previous = json.loads(Path(f'protocol-results/benchmark-previous-{seed}.json').read_text())
    ratios.append(base['query']['exact']['total_seconds'] / candidate['query']['exact']['total_seconds'])
    previous_ratios.append(previous['query']['exact']['total_seconds'] / candidate['query']['exact']['total_seconds'])
summary = {'baseline_commit': 'ff9c6bfbe89e1045e12ac56652165245c834630c',
           'previous_commit': '788b1677bf266877526f01394822bf031ed55a0a',
           'exact_query_speedups': ratios, 'median_exact_query_speedup': median(ratios),
           'previous_query_speedups': previous_ratios, 'median_previous_query_speedup': median(previous_ratios),
           'scope': 'Local synthetic, paired on one runner with identical Docker resource limits; not a platform score.'}
Path('protocol-results/paired-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))
PY
