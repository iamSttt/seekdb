#!/usr/bin/env bash
# Same source: isolate code-vector and winner-row prefetch hints.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
output_dir=${1:-protocol-results}
mkdir -p -- "$output_dir"

python3 - "$output_dir" <<'PY'
import sys
from pathlib import Path

output = Path(sys.argv[1])
variants = ('off', 'code', 'row', 'both')
expected = [output / f'prefetch-{variant}-{seed}.json'
            for seed in (38021, 7319, 19417) for variant in variants]
expected.append(output / 'prefetch-summary.json')
if any(path.exists() for path in expected):
    raise SystemExit('prefetch comparison requires new output files; use a fresh directory')
source = Path('Dockerfile').read_text(encoding='utf-8')
needle = 'g++ -O3 '
if source.count(needle) != 1 or any(name in source for name in
        ('MVDB_DISABLE_CODE_PREFETCH', 'MVDB_DISABLE_ROW_PREFETCH')):
    raise SystemExit('expected one default g++ -O3 build without either prefetch disable macro')
for variant in variants:
    flags = []
    if variant not in ('code', 'both'):
        flags.append('-DMVDB_DISABLE_CODE_PREFETCH')
    if variant not in ('row', 'both'):
        flags.append('-DMVDB_DISABLE_ROW_PREFETCH')
    extra = ' '.join(flags) + ' ' if flags else ''
    (output / f'Dockerfile-prefetch-{variant}').write_text(
        source.replace(needle, needle + extra), encoding='utf-8')
PY

for variant in off code row both; do
  docker build --platform linux/amd64 --file "$output_dir/Dockerfile-prefetch-$variant" \
    -t "minivec:prefetch-$variant" .
done
for seed in 38021 7319 19417; do
  case "$seed" in
    38021) order='off code row both';;
    7319) order='code row both off';;
    19417) order='row both off code';;
  esac
  for variant in $order; do
    python3 minivec/tools/benchmark_jsonl.py --image "minivec:prefetch-$variant" \
      --seed "$seed" --output "$output_dir/prefetch-$variant-$seed.json"
  done
done

python3 - "$output_dir" <<'PY'
import json
import math
import statistics
import sys
from pathlib import Path

output = Path(sys.argv[1])
variants = ('off', 'code', 'row', 'both')
orders = {38021: ['off', 'code', 'row', 'both'],
          7319: ['code', 'row', 'both', 'off'],
          19417: ['row', 'both', 'off', 'code']}


def counter(value):
    return value if type(value) is int and value >= 0 else None


def timing(value, name):
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        raise ValueError(f'invalid required timing: {name}')
    return value


def unique_phase(snapshots, phase):
    found = [sample for sample in snapshots if sample.get('phase') == phase]
    return found[0] if len(found) == 1 else None


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
        'exact_seconds': timing(exact['total_seconds'], 'exact'),
        'unfiltered_seconds': timing(exact['unfiltered']['total_seconds'], 'unfiltered'),
        'filtered_seconds': timing(exact['filtered']['total_seconds'], 'filtered'),
        'ingest_seconds': timing(report['ingest_seconds'], 'ingest'),
        'sampled_peak_bytes': max(available_peaks) if available_peaks else None,
        'query_cpu_usage_usec_both_modes': cpu['usage_usec'],
        'query_cpu_throttled_usec_both_modes': cpu['throttled_usec'],
    }


def ratio(numerator, denominator):
    return numerator / denominator if denominator > 0 else None


samples = []
for seed in (38021, 7319, 19417):
    reports = {}
    for variant in variants:
        path = output / f'prefetch-{variant}-{seed}.json'
        report = json.loads(path.read_text(encoding='utf-8'))
        if report['seed'] != seed:
            raise ValueError(f'seed mismatch in {path.name}')
        reports[variant] = describe(report)
    speedups = {}
    for variant in variants:
        speedups[variant] = {
            name: ratio(reports['off'][name + '_seconds'], reports[variant][name + '_seconds'])
            for name in ('exact', 'unfiltered', 'filtered')}
    samples.append({'seed': seed, 'execution_order': orders[seed],
                    'variants': reports, 'speedups_off_over_variant': speedups})
medians = {}
for variant in variants:
    medians[variant] = {}
    for name in ('exact', 'unfiltered', 'filtered'):
        values = [sample['speedups_off_over_variant'][variant][name] for sample in samples]
        medians[variant][name] = (
            statistics.median(values) if all(value is not None for value in values) else None)
summary = {
    'kind': 'prefetch_same_source_comparison',
    'variant_settings': {variant: {
        'code_prefetch_enabled': variant in ('code', 'both'),
        'row_prefetch_enabled': variant in ('row', 'both')} for variant in variants},
    'samples': samples,
    'median_speedups_off_over_variant': medians,
    'scope': 'Only MVDB_DISABLE_CODE_PREFETCH and MVDB_DISABLE_ROW_PREFETCH differ. '
             'Both hints are enabled by default; off disables both, code disables row only, '
             'row disables code only. All four images use the same working-tree source and '
             'unchanged official Session Docker limits. Their order rotates across three seeds. '
             'Ratios are paired within this run; this local diagnostic is not a formal score.',
    'query_time_scope': 'Each exact_seconds value covers 1000 exact requests; unfiltered_seconds '
                        'and filtered_seconds cover its 500-request subsets. ANN timings remain '
                        'in the raw reports. The medians are of three per-seed ratios.',
    'cpu_window': 'after_index to after_queries_before_shutdown in one verified container. '
                  'This covers BOTH modes (1000 exact plus 1000 ann requests), mode switches '
                  'and other container work inside the boundaries, not only the exact timing window.',
    'missing_metrics': 'Unavailable, ambiguous, different-container or decreasing CPU counters '
                       'produce null deltas. All-missing memory samples produce null. Missing '
                       'metrics are never substituted with zero; a measured zero remains zero.',
    'memory_scope': 'Maximum valid memory.peak snapshot in each report; cumulative per container '
                    'lifetime, not guaranteed final writeback coverage or an official B/M metric.',
}
with (output / 'prefetch-summary.json').open('x', encoding='utf-8') as stream:
    json.dump(summary, stream, ensure_ascii=False, allow_nan=False, indent=2)
    stream.write('\n')
print(json.dumps(summary, ensure_ascii=False, allow_nan=False, indent=2))
PY
