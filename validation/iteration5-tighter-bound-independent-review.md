# Independent review of the tighter long-double distance certificate

**Decision: no mathematical blocker within the restricted environment below.** Halving the existing error interval is justified; halving it again is not covered. This is an independent proof/source review and exact-rational calculation, not a native validation or a performance result. No repository source, CI configuration or archived evidence was changed. Native builds, database runs and CI runs for this review: **0**.

## Reviewed identity and scope

- `work/tighter-longdouble-bound-review.md`: SHA-256 `54d180e284665ababb335cca9db39fc921f5648ac356b9f1d0e65f720856ea6e`.
- Its JSON companion: SHA-256 `975612675e1865c03dc288cdb2ab86648d25cac7611f6fc11173b7f032691cec`.
- Inspected working-tree `minivec/src/distance.h`: SHA-256 `42affb10aa714ef417cec98b020a5b944c224512122c04a7a4eb68f0b0b19adb`. This identifies the inspected bytes, including their line endings; it must not replace the separately recorded canonical Git-byte identity of a future candidate.

The conclusion concerns finite binary32 inputs, `n <= 2^32`, a 64-bit `size_t`, the existing four lane accumulators and final `(s0+s1)+(s2+s3)` reduction, and actual x87 precision 64 with round-to-nearest/even. The current source has no runtime precision-control guard. An unconditional source replacement is therefore **not** approved by this review.

For a first implementation, restrict the new certificate to x86-64 with binary radix 2, `LDBL_MANT_DIG == 64`, `LDBL_MAX_EXP == 16384`, and `LDBL_MIN_EXP == -16381`. Keep other formats outside this new proof, even though the reviewed text states a more general precision inequality. Floating-point controls must remain unchanged throughout the call, and unsafe reassociation or lowering of long-double arithmetic is excluded.

## Independent derivation

Write `D = sum (a_i-b_i)^2` for the exact real result, `S` for the computed long-double sum, `u = 2^-64`, `m = floor(n/4)`, and `K = m+8`. Here long-double epsilon is **2u**, not u.

For one exact squared contribution, rounded subtraction followed by multiplication gives three relative-error factors: the subtraction factor occurs twice in the square, followed by one multiplication factor. This explicitly permits a rounded subtraction when operand exponents are far apart; finite float32 inputs do not by themselves make every such subtraction exact.

A contribution then passes through at most `m` lane additions, three tail additions to lane zero, and two final reduction additions. Thus its path has at most `3+m+3+2 = K` factors. Counting additions to zero as rounded is conservative. Tail-originating contributions and lengths below four also obey this bound. The contributions are nonnegative, so their summation does not introduce a cancellation condition number.

The standard product-of-rounding-factors estimate gives

```
|S-D| <= gamma_K D,       gamma_K = Ku/(1-Ku).
```

Using `S >= (1-gamma_K)D` yields the valid computed-sum bound

```
|S-D| <= [Ku/(1-2Ku)] S = B.
```

The operation count and this application are independently checked here; the underlying RN error model is standard. [Ogita, Rump and Oishi, Accurate Sum and Dot Product, §2](https://ogilab.w.waseda.jp/ogita/math/doc/2005_OgRuOi.pdf).

For nonzero finite-input distances, `2^-298 <= D < 2^290`. Subtractions, squares, partial sums and the error-bound products are comfortably normal in x87 extended format; no overflow, underflow or gradual-underflow exception to the relative model is needed. In particular `S*K < 2^322`, and the smallest relevant error is above `2^-360`. Zero distances and nonfinite sums do not enter the new certificate.

Now evaluate the proposed error using the actual source operation order:

```
E = fl(fl(S*K) * epsilon_L).
```

The first multiplication can round down, but its result is at least `SK(1-u)`. The second multiplication is exact power-of-two scaling in this range. Hence

```
E >= 2Ku(1-u)S,
E/B >= 2(1-u)(1-2Ku).
```

At the largest permitted K, `K=2^30+8`, an independent exact-rational calculation gives

```
2(1-2^-64)(1-2*(2^30+8)*2^-64)
 = 21267647930082773868290486836505083905
   /10633823966279326983230456482242756608
 = approximately 1.9999999997671694 > 1.
```

This matches the companion JSON exactly. The downward rounding of the error calculation is already covered with almost a factor-of-two margin. No outward `nextafterl` is required for this particular coefficient. A coefficient of epsilon/2 would lose that demonstrated margin and needs its own proof.

## Midpoints and strict comparison

Let `q` be S rounded to binary64. In this domain q and its two neighbors are positive finite normal binary64 values, well away from zero and overflow. Each adjacent midpoint needs at most 54 significand bits, so both the source additions and the subsequent halving compute it exactly in precision 64.

S lies in q's rounding cell. S and either midpoint are within a factor of two, so their differences are exactly representable by the Sterbenz property. Therefore the code's strict comparisons are comparisons of exact margins, rather than rounded estimates that might accidentally pass:

```
S-L > E and U-S > E  =>  L < D < U.
```

This proves q is the canonical binary64 result. Exact midpoint ties cannot be newly certified: their displacement from S is bounded by E, so one strict margin fails. A separate binary64 conversion-error allowance is unnecessary because the test directly bounds D against the exact cell boundaries. This argument also covers binade boundaries, whose lower and upper spacing differ.

## Required guard and compilation conditions

A conservative guard reads controls without changing them:

```
(x87_control_word & 0x0f3f) == 0x033f
(mxcsr           & 0xffc0) == 0x1f80
```

The first expression requires x87 PC64, RN and all six exception masks. The second requires RN, no DAZ/FTZ, and all six SSE exception masks. Read the x87 word with non-waiting `fnstcw`; leave sticky exception flags untouched. Sticky flags are not rounding controls and need not cause rejection. These bit assignments follow Intel's architecture documentation. [Intel Software Developer's Manual, Volume 1](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366521.pdf).

The guard must not be cached for an unconstrained thread lifetime: a caller may change its floating-point environment between calls. Rejecting unsupported controls for the **new** certificate while retaining the original code is a compatibility decision; it does not prove the original certificate correct under PC24/53 or non-RN modes.

Record actual compiler flags and check the generated arithmetic graph. `!__FAST_MATH__` alone does not exclude every individually supplied unsafe option. In particular, reassociation must not change the four-lane path count or midpoint computation; x87 operations must retain the assumed precision. GCC documents that its associative-math, fast-math and excess-precision choices affect these properties. [GCC 13.3 optimization options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Optimize-Options.html).

## Smallest implementation to test

Prefer a two-tier certificate over changing every scalar call:

1. Keep the original sum, original `error = ... * (2*epsilon_L)`, and original strict success test unchanged.
2. Only after that test fails, check the restricted compile-time/runtime environment.
3. Use `tight_error = error * 0.5L` and repeat the same two strict comparisons.
4. Otherwise call the unchanged exact integer fallback.

The final halving is exact in the proven range and equals the smaller expression analyzed above. This preserves the old successful path's source operations and avoids reading controls on those calls. It is a minimal candidate, not an assurance that compiler layout or overall performance remains identical.

The existing aggregate diagnostic reported 35,300 old certificates and 4,800 integer fallbacks among 40,100 scalar calls. It did not record each rounding-cell margin. Consequently it establishes neither how many calls this second tier would rescue nor the time saved; some nonzero distances can still be exact ties. No CPU fraction or score forecast follows from those counts.

## Necessary validation before production evaluation

- Freeze the original function and compare every new certificate against an independent exact integer/Fraction oracle, in optimized and sanitized native builds. Keep old-success, newly rescued and exact-fallback classifications separate from timing builds.
- Cover exact midpoint ties with both even/odd endpoints, values on either side, binade boundaries, signed zero, float32 minimum subnormals, maximum finite values, opposite signs and widely separated exponents. Use finite full-bit random cases and all four remainder lengths, including n=0 through 7 and the common 128/512/65536 dimensions. The `n=2^32` endpoint is justified analytically; no enormous allocation is needed merely to reproduce that inequality.
- Exercise PC24/53/reserved and all rounding controls, DAZ/FTZ and each exception mask. For unmasked-exception cases, invoke only the control guard, restore the environment, and then assert the result; do not execute floating-point distance code that might trap. Confirm sticky flags do not change admission and the guard changes no controls.
- Inspect the native four-lane/reduction operation graph and final conversion. Measure the actual guard plus second-certificate cost, keeping the original integer fallback and all unsupported-environment behavior visible. Do not substitute source-level expectations for an end-to-end measured result.

The reviewed proof is sufficient to proceed with this isolated candidate and these checks. It is not sufficient to adopt it, combine it with the active batch8 experiment, or predict an official score increase.
