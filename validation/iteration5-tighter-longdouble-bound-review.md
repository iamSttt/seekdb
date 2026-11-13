# Long-double certification: a rigorously smaller bound

Status: mathematical review only. No production edit, native build, database execution or CI was performed for this review. The source under review is `work/seekdb/minivec/src/distance.h`, `distance_scalar`, lines 128–166 in the inspected copy.

**Conclusion.** Under the stated floating-point prerequisites, replacing the current coefficient `2 * epsilon_L` by `epsilon_L` is safe without changing the existing four-lane arithmetic or midpoint certificate. Even after allowing the error-bound multiplication itself to round downward, the proposed error exceeds the derived worst-case error by almost a factor of two. This conclusion is analytic; the 4,800 observed integer fallbacks did not determine its constants. It does not establish how many fallbacks would disappear or whether an actual workload becomes faster.

## 1. Exact domain and necessary prerequisites

Let `n <= 2^32` on the actual 64-bit-size_t target (so `i+4` cannot wrap), let every stored coordinate be a finite binary32 value, and let

`D = sum_i (a_i - b_i)^2`

denote the exact real squared distance. Let the actual arithmetic of `long double` have precision `p >= 64`, round to nearest/even, and the normal exponent range of the existing x86 extended format. Put `u = 2^-p`, so C++ `numeric_limits<long double>::epsilon()` equals `2u`, not `u`.

The proof requires this *actual* precision, rather than just `LDBL_MANT_DIG`. On x86, x87 precision-control must be 64-bit and x87 rounding-control RN. A future implementation must not infer that from the compile-time macro: current `distance_scalar` has no runtime PC/RC guard. A conservative candidate would enable the smaller coefficient only in the verified environment and keep the old function/coefficient otherwise. For a straightforward x86 guard, PC/RC correspond to `(control_word & 0x0f00) == 0x0300`; also retain RN and disabled DAZ/FTZ in MXCSR if any generated load/conversion can use SSE. Masked exceptions and controls unchanged during a call are the ordinary execution contract here. Reading only `fegetround()` would not establish x87 precision-control.

The source's four independent lane accumulators and final `(s0+s1)+(s2+s3)` tree must be preserved. No fast-math reassociation or lowering of long-double operations is covered. Current root flags inspected here are `-O3 -std=c++17 -fno-math-errno ...`, not `-ffast-math`/`-Ofast`. GCC documents the distinction between default exact-sensitive behavior, fast-math, excess precision, and rounding-mode assumptions; build/ELF validation remains necessary for a future candidate. [GCC 13.3 optimization options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Optimize-Options.html).

Finite binary32 values have magnitude below `2^128`, and unequal values differ by at least `2^-149`. Consequently, for `D > 0`:

`2^-298 <= D < n * 2^258 <= 2^290`.

Subtractions, squared terms and nonzero partial sums are far inside normal x87 range. This includes binary32 subnormals and widely separated exponents: **we do not assume their long-double subtraction is exact**. Its rounding is explicitly counted below. The bound's own arithmetic is also normal: `sum*K < 2^322`, while the smallest relevant error on the actual p=64 x87 target is above `2^-360`. Extension to another format would require checking the error's exponent range too, not just its mantissa size. Zero vectors/distances keep the existing exact fallback; NaN/Infinity are outside the finite-input proof and retain existing behavior.

## 2. Count rounding factors along a contribution's path

Write `m = floor(n/4)` and `r = n mod 4`, and define `K = m+8`. Each rounded operation obeys `fl(z) = z(1+delta)` with `|delta| <= u` for the normal results used here. This is the standard round-to-nearest model; the usual gamma bounds are given in the authors' numerical-analysis paper. The following operation count and application to this function are our derivation. [Ogita, Rump and Oishi, Accurate Sum and Dot Product, §2](https://ogilab.w.waseda.jp/ogita/math/doc/2005_OgRuOi.pdf).

For one exact contribution `t_i=(a_i-b_i)^2`, its rounded squared term is

`t_i * (1+delta_sub)^2 * (1+delta_mul)`.

This is **three factors**, including the subtraction factor twice. Adding that term to a lane and then propagating it through subsequent lane additions adds at most `m` factors. Lane zero can receive at most three further tail additions. Finally, each lane passes through at most two additions in the reduction tree. Hence every contribution has at most

`3 + m + 3 + 2 = K`

rounding factors. We deliberately count even additions to zero; no saving from their exactness is needed. Tail-originating terms have no longer path. All contributions are nonnegative, so there is no cancellation condition number in this sum.

Thus, if `S` is the computed long-double sum, each contribution's product of factors is within the standard `gamma_K` bound, and

`|S-D| <= gamma_K * D`, with `gamma_K = K*u/(1-K*u)`.

For completeness, the product estimate follows directly from `(1-u)^K >= 1-K*u` and `(1+u)^K <= 1/(1-K*u)`. The latter follows by bounding each binomial coefficient by `K^j` and the resulting finite sum by the geometric series. Summing the nonnegative contributions preserves those inequalities. No independent-error or random-rounding assumption is used.

## 3. Convert to a bound expressed in computed S

At the worst permitted size and precision,

`K <= 2^30+8 < 2^31`, `u <= 2^-64`, therefore `K*u < 2^-33`.

From the previous estimate, `S >= (1-gamma_K)D`. Using this intentionally conservative conversion gives

`|S-D| <= [gamma_K/(1-gamma_K)] * S`

`          = [K*u/(1-2*K*u)] * S = B`.

The denominator is positive with a very large margin. A slightly tighter conversion can be obtained directly from the product inequalities, but it is not needed and is not used in the decision here.

## 4. The proposed error remains outward despite downward rounding

The minimal expression change under review is

`E = fl(fl(S*K) * epsilon_L)` where `epsilon_L = 2u`.

`K` is an exactly represented small integer. The first multiplication can round downward, but its normal result `A` obeys `A >= S*K*(1-u)`. Multiplication by `epsilon_L` is scaling by a power of two; it is exact here because neither intermediate nor result overflows or underflows. Therefore the actual value compared by the code obeys

`E >= 2*K*u*(1-u)*S`.

It is sufficient that `2*(1-u)*(1-2*K*u) >= 1`. Using the size bounds above,

`2*(1-u)*(1-2*K*u) > 2*(1-2^-64)*(1-2^-32) > 1`.

So **E > B >= |S-D|**, including the worst possible downward rounding of the first product. No added `nextafterl` is necessary for this particular half-size coefficient and these limits. This proof does not authorize dropping the coefficient again to `epsilon_L/2`; that would need a separate argument. The attached exact-rational check evaluates the worst endpoint rather than fitting a floating-point constant to examples.

For fixed S, both the old and proposed formulas differ only in final power-of-two scaling, so the new E is exactly half the old E in this normal domain. Mathematically the existing successful certificates remain successful; some previously uncertain cases can become certified. Compiler/profile effects on execution time are not covered by that monotonicity statement.

## 5. Existing strict midpoint test still proves canonical binary64 rounding

Let `q = RN_binary64(S)`, and let `q-` and `q+` be adjacent binary64 values. For this domain q and its neighbors are finite positive normal numbers, far from binary64 zero/overflow. Their midpoints

`L=(q+q-)/2`, `U=(q+q+)/2`

need at most 54 significand bits and are exactly represented at p>=64. The additions and power-of-two halvings used by the current code therefore compute these midpoints exactly.

S lies in q's rounding cell. S and either midpoint differ by far less than a factor of two; their subtraction is exact by the Sterbenz property. Equivalently, scale the two nearby binary floating-point values to a common grid: the difference needs no more precision than the original format. A midpoint equality produces zero margin and does not pass the strict comparison.

Accordingly, if the actual code passes both `S-L > E` and `U-S > E`, the previous section implies

`L < S-E <= D <= S+E < U`.

D is strictly inside q's rounding cell, so q is the correctly rounded binary64 answer. Exact midpoint ties remain on the integer fallback path; the tie-to-even rule is not guessed from the approximation. No extra half-ULP conversion allowance is needed because the certificate surrounds exact D and checks its position directly against exact rounding boundaries.

## 6. Evidence limits and narrowly scoped next step

The existing diagnostic has 40,100 scalar calls, 35,300 long-double certifications and 4,800 integer fallbacks; a separate audit established at least 4,786 nonzero-distance fallbacks. Those records do not contain each S/midpoint margin, so they cannot tell us the rescue count for a half-size interval. Nonzero distances can still be exact ties or lie too close to a midpoint. Call counts do not establish CPU share or an end-to-end gain.

The proof supports a standalone candidate limited to the coefficient, with environment gating for actual x87 PC64/RN and unchanged arithmetic. Before adopting it, independently review this proof; verify the native operation graph/rounding environment; compare the certified result against the existing integer oracle over ties, adjacent midpoint distances, float32 exponent extremes, every tail length and both normal/sanitized builds. Any mechanism timing must include the new guard's cost. Other FP environments must keep the original behavior rather than silently using this proof. This review neither implements that candidate nor combines it with the active batch8 experiment.

## 7. Narrow implementation chosen after independent review

The independent reviewer agreed with the proof. The prepared, still-unexecuted work-only candidate keeps the original error expression and original successful certificate verbatim. **Only after that certificate fails**, it checks the environment and tries the same strict midpoint comparisons with `tighter_error = old_error * 0.5L`. Thus the ordinary previously certified path does not read x87/MXCSR controls. Unsupported environments proceed through the original integer fallback they would already have taken.

This is exactly the same mathematical error interval: the original product is `A * 4u`, where `A=RN(S*K)`. Both that scaling and its subsequent halving are exact normal power-of-two scalings, so `old_error*0.5L = A*2u = E` from §4. No new rounding allowance is missing. The guard is restricted to x86_64, 64-bit size_t, radix 2, `LDBL_MANT_DIG==64`, `LDBL_MAX_EXP==16384`, `LDBL_MIN_EXP==-16381`, `(cw & 0x0f3f)==0x033f` and `(mxcsr & 0xffc0)==0x1f80`. Those masks require full precision/RN and masked exceptions, and disable SSE DAZ/FTZ; sticky status bits are ignored, never cleared. The format/fast-math macros are necessary safeguards, not proof that separately supplied unsafe compiler flags are absent.

The canonical Git reference was subsequently pinned independently to `HEAD:minivec/src/distance.h`, SHA256 `7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9`. The initial source SHA in the JSON review identified the Windows CRLF worktree copy; it must not be called canonical Git bytes. The separate candidate package records both its actual source commit and the canonical reference hash.
