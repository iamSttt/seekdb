# Nineteen-digit decimal prototype: correctness certificate

This v2 certificate was frozen in the standalone work directory before integration. The candidate is now integrated for validation; v2 native correctness and performance remain unmeasured. The Python diagnostic is separate evidence, not proof of the compiled operation graph or speed. V1 raw reports remain under validation/runs/43544203031; its prior plan, certificate, oracle and complete micro summaries are preserved in minivec/tools/short_decimal_experiment/history. V1 is not the performance control: EE and parent 9ed remain the references.

## Scope and arithmetic assumptions

The fast subset is `-?[0-9]+(\.[0-9]+)?` with at most **19 total digit characters**, counting leading zeroes, and at least one integer digit. The fraction length is at most 18. An explicit end, comma or one of the original six ASCII whitespace characters must follow. Exponents, leading plus/dot, trailing dot, hexadecimal, special values, unsupported suffixes and longer spellings go through the unchanged original `from_chars(float)` / conditional `strtof` path.

The concatenated mantissa satisfies `0 <= m < 10^19 < 2^64`. The check precedes every multiply/add, so no integer overflow occurs. Every denominator `10^f`, `0 <= f <= 18`, is exactly binary64-representable: `10^f = 2^f * 5^f` and `5^18 < 2^42`. In contrast to v1, `m` need not be exactly representable in binary64. Its conversion and the division each round once to nearest:

```
a = RN64(m)
d = RN64(a / 10^f)
```

For nonzero input the exact magnitude is in `[10^-18,10^19)`. All intermediate results and relevant binary32 rounding boundaries are finite and normal; binary32 subnormal/overflow boundaries are outside this scope. Zero and negative zero are handled separately by the parsed sign applied after conversion.

The existing per-vector environment gate requires x86-64/SSE2, IEEE binary32/binary64, `FLT_EVAL_METHOD == 0`, no fast-math build, round-to-nearest in `fegetround()` and MXCSR, FTZ/DAZ off, and all SIMD exceptions masked. Bodies shorter than 128 bytes skip the environment check and use the old loop. The caller must keep FP control state unchanged while parsing the vector. The build must preserve correctly rounded integer-to-double conversion, division and double-to-float conversion, with no reciprocal-math transform or excess precision. Proposed release micro flags match the actual Docker flags: `-O3 -fno-math-errno`, with no added `-frounding-math`. Separate strict correctness/ASan builds may add `-frounding-math` but their timings are not release comparisons.

## Error bound for integer conversion plus division

Let `x=m/10^f > 0` and `u=2^-53`. Correct round-to-nearest arithmetic gives

```
d = x (1+delta1)(1+delta2),       |delta1|, |delta2| <= u.
```

Therefore the larger relative error when expressing `x` in terms of `d` is

```
|x-d| / d <= (1-u)^(-2) - 1
          = (2u-u^2)/(1-u)^2 < 3u.
```

For `d` in binade `[2^e,2^(e+1))`, let `q=2^(e-52)` be that binade's binary64 grid spacing. Since `u*d < q`, **`|x-d| < 3q`**.

Every midpoint `h` between adjacent normal binary32 numbers is an exactly representable binary64 grid point. If rounding `x` directly to binary32 differs from converting `d`, there must be such a midpoint between them, possibly including an endpoint. A midpoint outside `d`'s binade is at least `2^27*q` away from its endpoint (the lower neighboring binade has half-width double steps), so an error of less than `3q` cannot cross an outside midpoint. Any relevant `h` is in the same binade as `d` and satisfies `|h-d| < 3q`.

Within that binade, adjacent binary32 values are `2^29` binary64 grid positions apart and their midpoint has low 29 fraction bits equal to `2^28`. Thus the check

```
low = bits(d) & (2^29-1)
2^28-4 <= low <= 2^28+4
```

rejects every possible boundary-crossing result. It is deliberately conservative: the sufficient proof bound is below three positions. If the check does not reject, `x` and `d` occupy the same open binary32 rounding cell; converting `d` returns the unique correctly rounded result for `x`. Absolute-magnitude computation followed by exact negation preserves RN symmetry and negative zero. Floating-point status flags can differ, as in v1; they are not SQL response fields. Control state is unchanged.

## Why v1's equality-only midpoint check is insufficient here

The independent reviewer found concrete counterexamples, also included in the oracle:

| Decimal | Correct binary32 | Naive two-round result | d relative to midpoint |
|---|---|---|---|
| `37.692861557006836` | `4216c57e` | `4216c57d` | one binary64 position below |
| `927745.5312499999999` | `49628018` | `49628019` | one position above |

The second mantissa is above `2^63`, so it also exercises the unsigned-conversion high-bit path. Both negative counterparts are tested. Equality-only detection would accept these wrong results; the four-position guard rejects them.

On an x86 compiler lowering high `uint64_t` conversion by shift/jam, the expected sequence is `j=(m>>1)|(m&1)`, convert `j` to double, then multiply/add by two. For even `m` the halving is exact. For odd `m`, `j` is an odd integer within one half-integer of `m/2`; in this range double midpoints are even integers separated by at least 1024. The jammed odd value and `m/2` cannot be on opposite sides of one, so the RN result is unchanged. Doubling is exact. This explains that known lowering, but the actual build still requires native oracle coverage and disassembly inspection; no uninspected compiler sequence is certified by this text.

## Two-loop parser equivalence

The candidate contains a fast prefix loop followed by the **byte-for-byte original old while loop**. The source function hash is pinned to unchanged EE/9ed parsing, never to the slower v1 candidate. `source-provenance.json` records both the original function and old-loop hashes.

Before each prefix attempt, `i` points at the first non-space byte and `out` contains exactly the successfully parsed preceding coordinates. A fast success has the same value and end pointer as old parsing and uses the same subsequent push/whitespace/comma/trailing-comma code. On the first decline, the prefix breaks without advancing `i` or appending a value; the old loop starts with exactly the state it would have reached. Leading whitespace has already been consumed, but consuming it again is a no-op. All remaining coordinates run the original loop without a per-coordinate short-path branch or end-pointer fallback branch. Invalid suffixes, partial output, trailing commas, allocation policy and whole-vector validation retain the old behavior.

Test-only counters exist in the prefix and compile out of timing. Remaining old-loop value counts are computed by the test caller, so the old loop remains literally unchanged even in the snapshot source.

## Planned evidence and limitations

The independent integer-rational oracle directly constructs binary32 bits from exponent, quotient/remainder and ties-to-even. It never uses `float(Fraction)` as expected output. The corpus additionally includes real `str(float32)` spellings; their values still use the integer oracle.

The generated corpus contains **554,561** tokens. In the separately labeled Python arithmetic diagnostic, 546,902 are eligible, 128,872 are conservatively guarded, and all 37,092 naive double-rounding errors are rejected. Equality-only checking would miss 1,114 errors; the new accepted set has zero mismatches. There are 1,271 eligible unsigned-high-bit mantissas. Counts reflect a boundary-heavy diagnostic corpus, not production rates.

Native tests cover the integer oracle, full-parser differential results and partial outputs, first-decline positions, whitespace/delimiters, 30,000 deterministic byte strings, non-NUL end/guard pages, FP environments and signed zero. The 24 original micro fixtures and generated data remain unchanged: dimensions 1/16/128/768 crossed with fixed6, mixed scientific, all scientific, all17-digit, float32 expanded to double repr17, and midpoint-heavy families. All negative samples remain. Actual release Docker and EE/9ed paired comparisons determine adoption; neither the v1 regression nor a favorable micro result is a valid substitute baseline.
