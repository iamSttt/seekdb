#!/usr/bin/env python3
"""Freeze the original parser plus a separate fast-prefix/unchanged-old loop."""
import argparse
import hashlib
import json
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--source', type=Path, required=True)
args = ap.parse_args()
raw = args.source.read_bytes()
source = raw.decode('utf-8')
start = source.index('bool parse_float_list(')
stop = source.index('// A quoted string', start)
original = source[start:stop].strip()
expected_original = '28f4031db532fe1e5f70bdb9e21e10b798e9ec26421925976224cca76655bc97'
assert hashlib.sha256(original.encode()).hexdigest() == expected_original, 'expected unchanged EE/9ed parser, not v1 candidate'
space_start = source.index('bool is_space(unsigned char c)')
space_end = source.index('\n}', space_start) + 2
space = source[space_start:space_end]
assert original.count('const auto parsed = std::from_chars(') == 1
loop_start = original.index('  while (i < n) {')
loop_end = original.index('  return !out->empty();', loop_start)
old_loop = original[loop_start:loop_end]
conversion_start = old_loop.index('    const char* endp = nullptr;')
conversion_end = old_loop.index('    out->push_back(stored);', conversion_start)
fast_conversion = '''    const auto fast = context.parse(begin, body.data() + n);
    if constexpr (Count) {
      if(fast.reason==short_decimal::Reason::accepted)++counts->accepted;
      else if(fast.reason==short_decimal::Reason::midpoint)++counts->midpoint;
      else if(fast.reason==short_decimal::Reason::environment)++counts->environment;
      else ++counts->syntax;
    }
    if(!fast.accepted())break; // i is unchanged at the first non-space byte.
    const char* endp=fast.ptr;
    const float stored=fast.value;
'''
prefix_loop = old_loop[:conversion_start] + fast_conversion + old_loop[conversion_end:]
prefix = '''  const short_decimal::Context context(body.size() >= 128);
  if(context.enabled()) {
''' + ''.join('  ' + line + '\n' for line in prefix_loop.splitlines()) + '  }\n'
candidate = original[:loop_start] + prefix + original[loop_start:]
candidate = candidate.replace('bool parse_float_list(std::string_view body, std::vector<float>* out) {',
    'template<bool Count=false>\nbool candidate_parse(std::string_view body, std::vector<float>* out, Counters* counts=nullptr) {')
assert candidate.count(old_loop) == 1, 'old while loop must remain byte-for-byte'
baseline = original.replace('bool parse_float_list(', 'inline bool baseline_parse(')
header = '''#pragma once
#include "short_decimal.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>
namespace short_decimal_test {
struct Counters { uint64_t accepted=0,midpoint=0,environment=0,syntax=0,bypassed=0; };
'''+space+'\n'+baseline+'\n'+candidate+'\n} // namespace short_decimal_test\n'
root = Path(__file__).resolve().parent
(root/'parser_snapshot.h').write_text(header, encoding='utf-8', newline='\n')
report = {'source': str(args.source.resolve()), 'source_sha256': hashlib.sha256(raw).hexdigest(),
          'original_parse_float_list_sha256': hashlib.sha256(original.encode()).hexdigest(),
          'snapshot_sha256': hashlib.sha256(header.encode()).hexdigest(),
          'original_old_loop_sha256': hashlib.sha256(old_loop.encode()).hexdigest(),
          'candidate_old_loop_verbatim': True,
          'changes': ['one Context per vector, no FP environment check for body shorter than 128 bytes',
                      'fast prefix loop breaks at first decline, old loop resumes at identical i/out state',
                      'old while loop retained byte-for-byte; no per-coordinate fast branch inside old loop',
                      'test-only prefix counters compiled out of timing; old-value counts computed by test callers'],
          'original_function_included_verbatim_except_name': True}
(root/'source-provenance.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(json.dumps(report))
