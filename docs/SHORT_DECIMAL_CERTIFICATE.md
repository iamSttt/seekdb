# Short-decimal prototype: scope and correctness argument

This is an experimental candidate integrated over the `9ed5baa` runtime, with native correctness and performance pending at integration time. The Python diagnostic alone does not establish compiled correctness or speed. The authoritative pre-run scope is `validation/short-decimal-plan-20261114.json`. CI uses the actual root GCC13.3 flags for its primary numerical test and microbenchmark; a separate sanitizer build adds `-frounding-math`. `verify_source.py` binds the instrument-free snapshot body to the production parser, and a separate protocol test checks real ELF responses and committed/restarted float32 bits.

## Accepted subset and environment

The fast path recognizes `-?[0-9]+(\.[0-9]+)?`, with at most 15 total digit characters, including leading zeroes. A following byte must be the explicit end, comma, or one of the original six ASCII whitespace characters. It accepts neither a prefix before an exponent/hex/garbage suffix nor `.1`, `1.`, or `+1`. Those cases execute the unchanged `from_chars(float)` and conditional `strtof` block. Whole-vector grammar, partial-output behavior, allocation policy, and finiteness checks remain the original code.

Before native measurements, the scope was further restricted to vector bodies of at least 128 bytes; shorter bodies do not perform even the FP environment check. The first declined short attempt disables further attempts for that vector, including a midpoint decline. Thus a vector containing long decimal or exponent spellings pays at most one unsuccessful short scan, rather than scanning each coordinate twice. This limiter changes only attempts, not parsing results. Mixed vectors can lose most potential acceleration after an early fallback, so that scenario is explicitly measured. The native token oracle also tests `Context.parse` directly, because a one-token body usually does not satisfy the vector activation gate.

Let `m` be the concatenated integer digits and `f` the fraction length. Then `0 <= m < 10^15 < 2^53`, `0 <= f <= 14` in this deliberately strict implementation, and `10^f` is also an exactly representable binary64 integer. Nonzero exact magnitudes lie between `10^-14` and `10^15`; the looser `[10^-15,10^15)` bound also suffices. Thus every nonzero magnitude and its binary64 rounded result are finite, normal binary32-range values, far from both binary32 underflow and overflow.

The prototype requires x86-64/SSE2, IEEE binary32/binary64, `FLT_EVAL_METHOD == 0`, no fast-math build, round-to-nearest in both `fegetround()` and MXCSR, FTZ/DAZ disabled, and all six SIMD exceptions masked. This is checked once per vector. The caller must not change FP control state during parsing. The actual build must preserve a correctly rounded binary64 division and binary32 conversion: no reciprocal approximation, unsafe reassociation, or hidden excess precision. Native validation must inspect the generated division/conversion instructions. `-fno-fast-math -frounding-math` is used for the proposed standalone builds.

## Why checking only double results exactly at float midpoints suffices

Work first with the positive magnitude `x = m / 10^f`, and let `d = RN64(x)`. Both operands are exact; `d` is one round-to-nearest binary64 division.

Every rounding boundary `h` between adjacent finite normal binary32 values in this range is exactly representable as a binary64 number. Round-to-nearest is monotone and fixes representable values. Therefore, if `x < h`, then `RN64(x) <= RN64(h) = h`; similarly, `x > h` implies `RN64(x) >= h`. Consequently, a binary64 rounding step cannot cross `h` strictly. The only way the binary32 rounding decision could differ after binary64 rounding is for `d` to land exactly on a boundary. If `d` is not any boundary, `x` and `d` lie in the same open binary32 rounding cell, so converting `d` to binary32 gives the unique correctly rounded result for `x`.

For a positive normal binary64 value in a binary32-normal binade, the adjacent binary32 lattice points have binary64 fraction fields differing by `2^29`. Their midpoint has low 29 fraction bits equal to `2^28`. This characterization also covers the boundary just below a power of two: that midpoint belongs to the lower binade and the same lower-binade spacing applies. The implementation tests exactly this low-bit pattern and falls back on every such `d`, regardless of which side `x` was on or the even/odd tie direction. It does not attempt to infer a tie from a decimal spelling.

Zero produces `d=0` and positive zero; applying the parsed sign after conversion preserves negative zero. For nonzero negatives, round-to-nearest is sign-symmetric, so converting the positive magnitude and negating is correct. Negation is exact.

## Parsing and memory safety

All character dereferences are guarded by `p != end`. The 15-digit limit is checked before multiplication/addition, bounding the accumulator strictly below `10^15`, so unsigned overflow cannot occur. The fraction length indexes a 16-entry exact table and is at most 14. Midpoint/unsupported/environment outcomes return the original input pointer and do not modify the caller's stored value. The old parser block then sees the identical original range. Delimiter validation prevents silently treating `1e3`, `0x1`, or `1junk` as a successfully parsed short prefix.

The new fast parser itself is bounded by the explicit end; direct guard-page tests cover a physical non-NUL end. The unchanged historical `strtof` fallback is separately tested only under its existing string-storage assumptions. No new guarantee is claimed for that old unbounded library API. Floating-point exception *status* flags are not part of the SQL response contract; the fast division/conversion can set inexact. Control state is not modified, and unmasked environments never enter the arithmetic fast path.

## Independent oracle and differential validation

`make_oracle.py` computes expected binary32 values directly from integer rational numerators/denominators: find the binary exponent, divide to obtain a 24-bit significand, and round from quotient/remainder with tie-to-even. Expected values are never constructed by first converting a `Fraction` to binary64. Directed values cover sign/zero, digit boundaries, exponents around the eligible range, both midpoint parities, binade transitions, and nearby decimal numerators. Random cases are deterministic.

The generated corpus has 243,908 cases. The separately labeled Python double-rounding diagnostic found four real naive double-rounding failures, all caught by midpoint rejection:

- `0.98356494307518`: correct `3f7bcae9`, naive `3f7bcaea`.
- `3.05607807636261`: correct `404396c9`, naive `404396c8`.
- Their negative counterparts have the corresponding sign bits.

There were 237,788 eligible spellings, 12,991 midpoint fallbacks, and no accepted mismatch in that Python diagnostic. These are corpus counts, not production acceptance rates. Exact integer midpoint values such as `16777217` and `16777219` are included and also fall back.

The native test checks that oracle, compares the complete original/candidate parsed float bit patterns including partial output on errors, exercises incompatible syntax and 30,000 deterministic byte-string fuzz cases, checks explicit pointer boundaries/guard pages, and verifies disabled FP environments. Full-vector tests cover dimensions 1/16/128/768 and first-decline positions from the first through the last coordinate. The native microbenchmark includes the full new per-vector environment guard, allocation, scanner, division, cast and fallback costs. It covers those four dimensions and six families: fixed-six-decimal, mixed-exponent, all-exponent, all-17-digit, float32 expanded into double with 17-digit formatting, and midpoint fallback. No parser or end-to-end speed claim is justified before these native measurements.
