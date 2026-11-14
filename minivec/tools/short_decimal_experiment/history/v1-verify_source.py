#!/usr/bin/env python3
"""Bind the numerical test snapshot to the production parser body."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
source = Path(sys.argv[1]).read_text(encoding='utf-8')
snapshot = (root / 'parser_snapshot.h').read_text(encoding='utf-8')
start = source.index('bool parse_float_list(')
production = source[start:source.index('// A quoted string', start)].strip()
candidate = snapshot.split('template<bool Count=false>\n', 1)[1]
candidate = candidate.rsplit('\n} // namespace short_decimal_test', 1)[0].strip()
candidate = candidate.replace(
    'bool candidate_parse(std::string_view body, std::vector<float>* out, Counters* counts=nullptr)',
    'bool parse_float_list(std::string_view body, std::vector<float>* out)')
candidate, count = re.subn(r'      if constexpr \(Count\) \{\n.*?\n      \}\n', '', candidate, flags=re.S)
assert count == 1
candidate = candidate.replace('} else if constexpr (Count) ++counts->bypassed;', '}')
assert candidate == production, 'Test snapshot differs from production parser body'
baseline = snapshot.split('inline bool baseline_parse(', 1)[1].split('\ntemplate<bool Count=false>', 1)[0]
baseline = ('bool parse_float_list(' + baseline).strip()
report = {
    'status': 'identical_after_removing_test_counters_and_renaming',
    'production_parser_normalized_sha256': hashlib.sha256(production.encode()).hexdigest(),
    'baseline_parser_normalized_sha256': hashlib.sha256(baseline.encode()).hexdigest(),
    'snapshot_sha256': hashlib.sha256((root / 'parser_snapshot.h').read_bytes()).hexdigest(),
    'production_header_sha256': hashlib.sha256(Path(sys.argv[1]).with_name('short_decimal.h').read_bytes()).hexdigest(),
    'scope': 'Text binding of parser implementation; native and actual-binary tests remain separate.'
}
print(json.dumps(report, indent=2))
