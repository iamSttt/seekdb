#!/usr/bin/env python3
"""Integer-only rational -> binary32 oracle plus adversarial decimal corpus.

No decimal -> Python float conversion determines an expected binary32 value.
Python float is used only for a separately reported double-rounding diagnostic.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import random
import struct


def rounded_integer(n, d):
    q, r = divmod(n, d)
    return q + (2*r > d or (2*r == d and q & 1))


def binary32_bits(value, negative_zero=False):
    sign = value < 0 or negative_zero
    value = abs(value)
    if not value:
        return int(sign) << 31
    n, d = value.numerator, value.denominator
    e = n.bit_length() - d.bit_length()
    if (n < d << e) if e >= 0 else (n << -e < d):
        e -= 1
    shift = 23 - max(e, -126)
    if shift >= 0:
        m = rounded_integer(n << shift, d)
    else:
        m = rounded_integer(n, d << -shift)
    if e < -126:
        result = m
    else:
        if m == 1 << 24:
            e += 1
            m >>= 1
        result = 0x7f800000 if e > 127 else ((e + 127) << 23) + m - (1 << 23)
    return result | (int(sign) << 31)


def f32_fraction(bits):
    assert 0 < bits < 0x7f800000
    exponent = (bits >> 23) & 255
    mantissa = bits & 0x7fffff
    if exponent:
        mantissa += 1 << 23
    shift = (exponent - 127 - 23) if exponent else -149
    return Fraction(mantissa << shift, 1) if shift >= 0 else Fraction(mantissa, 1 << -shift)


def decimal(m, f):
    s = str(m)
    if not f:
        return s
    s = s.zfill(f + 1)
    return s[:-f] + '.' + s[-f:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--random', type=int, default=50000)
    args = ap.parse_args()
    rng = random.Random(0xdec1a1114)
    cases = {}

    def add(token):
        cases[token] = binary32_bits(Fraction(token), token.startswith('-') and Fraction(token) == 0)

    for s in ('0', '-0', '00.0000', '-00.0000', '1', '-1', '999999999999999',
              '0.00000000000001', '0.000000000000001', '1000000000000000',
              '16777217', '-16777217', '16777219', '16384.0009765625',
              '16384.0029296875', '.125', '-.125', '1.', '+1', '1e-45',
              '-1e-100', '1e-300', '3.4028234663852886e38', '1e39', '1e300',
              '9007199254740991', '9007199254740992', '9007199254740993',
              '9223372036854775807', '9223372036854775808', '9223372036854775809',
              '9999999999999999999', '10000000000000000000',
              '0.000000000000000001', '0.0000000000000000001',
              '0.98356494307518', '3.05607807636261',
              '37.692861557006836', '-37.692861557006836',
              '927745.5312499999999', '-927745.5312499999999'):
        add(s)
    for digits in range(1, 22):
        for m in (1, 10**(digits-1), 10**digits-1):
            for f in range(0, 22):
                for negative in ('', '-'):
                    add(negative + decimal(m, f))
    for _ in range(args.random):
        digits = rng.randrange(1, 21)
        m = rng.randrange(10**(digits-1), 10**digits)
        f = rng.randrange(0, 20)
        s = decimal(m, f)
        if rng.randrange(2):
            s = '-' + s
        add(s)

    # Decimal neighbors of actual binary32 rounding boundaries. Include both
    # lower-mantissa parities, binade transitions, and every eligible exponent.
    for e in range(-60, 64):
        mantissas = [0, 1, 2, 3, 0x7ffffe, 0x7fffff]
        mantissas += [rng.randrange(0x800000) for _ in range(35)]
        for mantissa in mantissas:
            lo = ((e + 127) << 23) | mantissa
            mid = (f32_fraction(lo) + f32_fraction(lo+1)) / 2
            for f in range(0, 19):
                scaled = mid * 10**f
                center = scaled.numerator // scaled.denominator
                for delta in (-2, -1, 0, 1, 2, 3):
                    m = center + delta
                    if 0 <= m < 10**19:
                        s = decimal(m, f)
                        if sum(c.isdigit() for c in s) <= 19:
                            add(s)
                            add('-' + s)

    # Actual protocol spelling style: float32 expanded into binary64, then the
    # shortest Python repr of that binary64 (same map(str,array('f')) behavior).
    # These are corpus inputs only; their expected values still use Fraction.
    for _ in range(10000):
        b = rng.randrange(0x34000000, 0x4f000000) | (rng.randrange(2) << 31)
        add(str(struct.unpack('<f', struct.pack('<I', b))[0]))
    diagnostic = {'eligible': 0, 'near_midpoint_fallback': 0, 'exact_midpoint': 0,
                  'would_double_round_wrong': 0, 'old_equality_guard_misses': 0,
                  'accepted_mismatch': 0, 'uint64_high_bit_eligible': 0}
    examples = []
    guard_extension_examples = []
    for token, expected in cases.items():
        s = token.lstrip('-')
        if not s or not s[0].isdigit() or not s[-1].isdigit() or 'e' in s or '+' in s:
            continue
        digits = s.replace('.', '')
        if len(digits) > 19:
            continue
        f = len(s) - s.index('.') - 1 if '.' in s else 0
        m = int(digits)
        d = float(m) / float(10**f)
        dbits = struct.unpack('<Q', struct.pack('<d', d))[0]
        midpoint_offset = (dbits & ((1 << 29) - 1)) - (1 << 28)
        near_midpoint = abs(midpoint_offset) <= 4
        got = struct.unpack('<I', struct.pack('<f', -d if token.startswith('-') else d))[0]
        diagnostic['eligible'] += 1
        diagnostic['near_midpoint_fallback'] += near_midpoint
        diagnostic['exact_midpoint'] += midpoint_offset == 0
        diagnostic['uint64_high_bit_eligible'] += m >= 1 << 63
        diagnostic['would_double_round_wrong'] += got != expected
        diagnostic['old_equality_guard_misses'] += got != expected and midpoint_offset != 0
        diagnostic['accepted_mismatch'] += got != expected and not near_midpoint
        example = {'token': token, 'expected': f'{expected:08x}',
                   'naive_double_round': f'{got:08x}', 'midpoint_offset_ulp64': midpoint_offset,
                   'near_midpoint_rejected': near_midpoint}
        if got != expected and len(examples) < 10:
            examples.append(example)
        if got != expected and midpoint_offset != 0 and len(guard_extension_examples) < 10:
            guard_extension_examples.append(example)
    assert diagnostic['accepted_mismatch'] == 0
    assert diagnostic['near_midpoint_fallback'] > 0
    assert diagnostic['old_equality_guard_misses'] > 0, 'corpus must demonstrate why equality-only is insufficient'
    raw = ''.join(f'{t}\t{b:08x}\n' for t, b in cases.items()).encode('ascii')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(raw)
    report = {'scope': 'integer-only exact rational binary32 oracle; Python diagnostic is not native validation',
              'cases': len(cases), 'sha256': hashlib.sha256(raw).hexdigest(),
              'diagnostic': diagnostic, 'double_rounding_examples': examples,
              'guard_extension_examples': guard_extension_examples}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
