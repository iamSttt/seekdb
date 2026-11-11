#!/usr/bin/env bash
# Same-source comparison: only MVDB_DISABLE_PARALLEL_TOPK differs.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
output_dir=${1:-protocol-results}
mkdir -p -- "$output_dir"

python3 - "$output_dir" <<'PY'
import sys
from pathlib import Path

output = Path(sys.argv[1])
expected = [output / f'parallel-{version}-{seed}.json'
            for seed in (38021, 7319, 19417) for version in ('serial', 'parallel')]
expected.append(output / 'parallel-summary.json')
if any(path.exists() for path in expected):
    raise SystemExit('parallel comparison requires new output files; use a fresh directory')
source = Path('Dockerfile').read_text(encoding='utf-8')
needle = 'g++ -O3 '
if source.count(needle) != 1 or 'MVDB_DISABLE_PARALLEL_TOPK' in source:
    raise SystemExit('expected one default g++ -O3 build without the disable macro')
(output / 'Dockerfile-parallel-serial').write_text(
    source.replace(needle, needle + '-DMVDB_DISABLE_PARALLEL_TOPK '), encoding='utf-8')
PY

docker build --platform linux/amd64 -t minivec:parallel-topk-parallel .
docker build --platform linux/amd64 --file "$output_dir/Dockerfile-parallel-serial" \
  -t minivec:parallel-topk-serial .
for seed in 38021 7319 19417; do
  if [ "$seed" = 7319 ]; then order='parallel serial'; else order='serial parallel'; fi
  for version in $order; do
    python3 minivec/tools/benchmark_jsonl.py --image "minivec:parallel-topk-$version" \
      --seed "$seed" --output "$output_dir/parallel-$version-$seed.json"
  done
done

python3 - "$output_dir" <<'PY'
import json
import math
import statistics
import sys
from pathlib import Path

output = Path(sys.argv[1])


def counter(value):
    return value if type(value) is int and value >= 0 else None


def required_time(value, name):
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        raise ValueError(f'invalid required timing: {name}')
    return value


def unique_phase(snapshots, phase):
    matches = [sample for sample in snapshots if sample.get('phase') == phase]
    return matches[0] if len(matches) == 1 else None


def query_cpu_deltas(snapshots):
    before = unique_phase(snapshots, 'after_index')
    after = unique_phase(snapshots, 'after_queries_before_shutdown')
    result = {'usage_usec': None, 'throttled_usec': None}
    if before is None or after is None:
        return result
    before_id = (before.get('container') or {}).get('id')
    after_id = (after.get('container') or {}).get('id')
    if not isinstance(before_id, str) or not before_id or before_id != after_id:
        return result
    for key in result:
        start = counter((before.get('cpu_stat') or {}).get(key))
        end = counter((after.get('cpu_stat') or {}).get(key))
        if start is not None and end is not None and end >= start:
            result[key] = end - start
    return result


def describe(report):
    if (report.get('rows'), report.get('dimension'), report.get('queries_per_mode'),
        report.get('filtered_queries_per_mode'), report.get('k')) != (10000, 128, 1000, 500, 10):
        raise ValueError('expected the standard 10k/128d/1000-query/K10 workload')
    exact = report['query']['exact']
    if exact['count'] != 1000 or report['query']['ann']['count'] != 1000:
        raise ValueError('CPU comparison requires both complete query modes')
    snapshots = (report.get('container_profile') or {}).get('snapshots') or []
    peaks = [counter(sample.get('memory_peak_bytes')) for sample in snapshots]
    available_peaks = [peak for peak in peaks if peak is not None]
    cpu = query_cpu_deltas(snapshots)
    return {
        'exact_seconds': required_time(exact['total_seconds'], 'exact'),
        'unfiltered_seconds': required_time(exact['unfiltered']['total_seconds'], 'unfiltered'),
        'filtered_seconds': required_time(exact['filtered']['total_seconds'], 'filtered'),
        'ingest_seconds': required_time(report['ingest_seconds'], 'ingest'),
        'sampled_peak_bytes': max(available_peaks) if available_peaks else None,
        'query_cpu_usage_usec_both_modes': cpu['usage_usec'],
        'query_cpu_throttled_usec_both_modes': cpu['throttled_usec'],
    }


def ratio(numerator, denominator):
    return numerator / denominator if denominator > 0 else None


samples = []
for seed in (38021, 7319, 19417):
    reports = {}
    for version in ('serial', 'parallel'):
        path = output / f'parallel-{version}-{seed}.json'
        report = json.loads(path.read_text(encoding='utf-8'))
        if report['seed'] != seed:
            raise ValueError(f'seed mismatch in {path.name}')
        reports[version] = describe(report)
    sample = {'seed': seed, **reports}
    for field in ('exact_seconds', 'unfiltered_seconds', 'filtered_seconds'):
        sample[field.removesuffix('_seconds') + '_speedup_serial_over_parallel'] = ratio(
            reports['serial'][field], reports['parallel'][field])
    samples.append(sample)
ratios = [sample['exact_speedup_serial_over_parallel'] for sample in samples]
summary = {
    'kind': 'parallel_topk_same_source_comparison',
    'samples': samples,
    'median_exact_query_speedup_serial_over_parallel':
        statistics.median(ratios) if all(value is not None for value in ratios) else None,
    'scope': 'Only MVDB_DISABLE_PARALLEL_TOPK differs. Both images are built from this working tree '
             'and run sequentially on one host under the unchanged official Session Docker limits. '
             'Image order alternates across the three seeds; this is a local diagnostic, not a formal score.',
    'query_time_scope': 'exact_seconds covers 1000 exact requests; unfiltered_seconds and filtered_seconds '
                        'are the 500-request subsets of that exact mode. ANN timings remain in each raw report.',
    'cpu_window': 'after_index to after_queries_before_shutdown, within the same verified container. '
                  'The window contains BOTH modes: 1000 exact plus 1000 ann requests, mode switches '
                  'and any other container work between those snapshots. CPU usage/throttling deltas '
                  'must not be compared directly with the wall time of only the 1000 exact requests.',
    'missing_metrics': 'Unavailable, ambiguous, different-container or decreasing counters are null, '
                       'never substituted with zero. A measured zero delta remains zero.',
    'memory_scope': 'Maximum available memory.peak snapshot across this report. Each counter is '
                    'cumulative for its container lifetime; no valid snapshot means null. Samples '
                    'do not guarantee final writeback coverage and are not official B/M metrics.',
}
with (output / 'parallel-summary.json').open('x', encoding='utf-8') as stream:
    json.dump(summary, stream, ensure_ascii=False, allow_nan=False, indent=2)
    stream.write('\n')
print(json.dumps(summary, ensure_ascii=False, allow_nan=False, indent=2))
PY
