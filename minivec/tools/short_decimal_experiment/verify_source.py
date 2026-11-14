#!/usr/bin/env python3
"""Bind the numerical test snapshot to the production parser body."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
# read_text uses universal-newline normalization: original SQL Git blobs may
# be CRLF while frozen numerical snapshots are LF. Binding ignores only that.
source = Path(sys.argv[1]).read_text(encoding='utf-8')
snapshot = (root / 'parser_snapshot.h').read_text(encoding='utf-8')
freeze = json.loads((root / 'freeze.json').read_text(encoding='utf-8'))
provenance = json.loads((root / 'source-provenance.json').read_text(encoding='utf-8'))
assert not (root / 'short_decimal.h').exists(), 'Snapshot must include the production header, not a shadow copy'
start = source.index('bool parse_float_list(')
production = source[start:source.index('// A quoted string', start)].strip()
candidate = snapshot.split('template<bool Count=false>\n', 1)[1]
candidate = candidate.rsplit('\n} // namespace short_decimal_test', 1)[0].strip()
candidate = candidate.replace(
    'bool candidate_parse(std::string_view body, std::vector<float>* out, Counters* counts=nullptr)',
    'bool parse_float_list(std::string_view body, std::vector<float>* out)')
candidate, count = re.subn(r'      if constexpr \(Count\) \{\n.*?\n      \}\n', '', candidate, flags=re.S)
assert count == 1
assert candidate == production, 'Test snapshot differs from production parser body'
baseline = snapshot.split('inline bool baseline_parse(', 1)[1].split('\ntemplate<bool Count=false>', 1)[0]
baseline = ('bool parse_float_list(' + baseline).strip()
baseline_sha = hashlib.sha256(baseline.encode()).hexdigest()
assert baseline_sha == provenance['original_parse_float_list_normalized_sha256']
assert baseline_sha == '229669aba4294fff3a26d5a70e5fe06ec548699a866d890ed9bed0a791226afd', 'Must use EE/9ed, not v1, as original parser'
old_start = baseline.index('  while (i < n) {')
old_loop = baseline[old_start:baseline.index('  return !out->empty();', old_start)]
assert production.count(old_loop) == 1, 'Original remainder loop changed'
assert hashlib.sha256(old_loop.encode()).hexdigest() == provenance['original_old_loop_normalized_sha256']
assert 'try_short' not in production and 'if(!endp)' not in production, 'v1 per-coordinate decision remained'
header_path = Path(sys.argv[1]).with_name('short_decimal.h')
bound_files = {'short_decimal.h': header_path}
for name in ('parser_snapshot.h', 'make_oracle.py', 'test_short_decimal.cpp', 'bench_short_decimal.cpp'):
    bound_files[name] = root / name
for name, path in bound_files.items():
    normalized_sha = hashlib.sha256(path.read_text(encoding='utf-8').encode()).hexdigest()
    expected = freeze['files'][name].get('normalized_sha256', freeze['files'][name]['sha256'])
    assert normalized_sha == expected, f'Frozen v2 source changed: {name}'
oracle = json.loads((root / 'oracle.json').read_text(encoding='utf-8'))
assert oracle['sha256'] == freeze['files']['oracle.tsv']['sha256']
report = {
    'status': 'identical_after_removing_test_counters_and_renaming',
    'production_parser_normalized_sha256': hashlib.sha256(production.encode()).hexdigest(),
    'baseline_parser_normalized_sha256': baseline_sha,
    'original_remainder_loop_normalized_sha256': hashlib.sha256(old_loop.encode()).hexdigest(),
    'original_remainder_loop_verbatim_after_newline_normalization': True,
    'snapshot_sha256': hashlib.sha256((root / 'parser_snapshot.h').read_bytes()).hexdigest(),
    'production_header_sha256': hashlib.sha256(header_path.read_bytes()).hexdigest(),
    'production_header_normalized_sha256': hashlib.sha256(header_path.read_text().encode()).hexdigest(),
    'oracle_sha256': oracle['sha256'],
    'oracle_cases': oracle['cases'],
    'candidate': 'v2 integrated for validation, not adopted; header work-only comment preserved to retain frozen identity',
    'scope': 'Full parser binding after CRLF normalization and removal of test-only counters; actual production header and frozen v2 fixture binding. Native and actual-binary tests remain separate.'
}
print(json.dumps(report, indent=2))
