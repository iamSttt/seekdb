# Independent review: quarter-plus-margin long-double certificate

**Numeric conclusion: safe under the same restricted format/control/arithmetic assumptions as the existing half-error proof.** The exact proposed factor is `c = 1/4 + 2^-30`, not plain `1/4`. The additional multiplication's worst downward rounding is included below. This review does not change the current 19-line candidate, run a database, compile native code, start CI, or establish a performance gain.

The frozen candidate inspected is `work/tighter-bound-candidate/candidate_distance.h`, SHA-256 `bdcc9e587677b91c0e57a0da8dd63eda7160c62fb9121e22cf5db19d243bf7c7`. The previous independent proof is `work/tighter-bound-independent-review.md`. The new exact-rational arithmetic receipt is `work/tighter-quarter-plus-margin-proof.json`; old reports and source are unchanged.

## Domain and original arithmetic

Retain finite binary32 coordinates; 64-bit size_t; `n <= 2^32`; x86-64 extended long double with radix 2, precision 64, exponent limits 16384 / -16381; stable controls throughout the call. Require the existing conservative masks:

```
(x87_cw & 0x0f3f) == 0x033f
(mxcsr  & 0xffc0) == 0x1f80
```

Thus RN/even, x87 PC64, all exceptions masked, no DAZ/FTZ. No unsafe reassociation, fast-math, altered four-lane reduction, or lowered precision is authorized. Unsupported controls retain the old branch/fallback behavior; this proof does not retroactively prove the old certificate under unsupported controls.

Write `u = 2^-64`, `m = floor(n/4)`, `K = m+8`, exact real squared distance `D`, and computed long-double sum `S`. The existing operation-count proof gives, for `S > 0`:

```
|S-D| <= B = [Ku/(1-2Ku)] S.
```

This permits rounded subtraction: three factors per squared contribution (subtraction twice and product once), at most m lane additions, three tail additions, and two reduction additions. All contributions are nonnegative. Preserve the original sum and original error operation order:

```
A = RN(S*K)
old_error = A * (2*epsilon_L) = A * 4u.
```

K is exactly represented. Scaling A by 4u is exact, normal power-of-two scaling. Neither this range nor the new range approaches x87 underflow/overflow: positive distances are at least `2^-298` and below `2^290`; S*K is below `2^322`, and the smallest relevant new error remains above `2^-360`. Zero and nonfinite values keep the existing fallback.

## Include coefficient representation and both downward roundings

Let `delta = 2^-28`. Then

```
c = 1/4 + 2^-30 = (1+delta)/4 = 268435457 / 1073741824.
```

This requires only 29 significant binary bits. It is represented exactly in binary64 and precision-64 long double. An unambiguous C++ constant would be `0x1.0000001p-2L`, equivalently the exactly representable sum `(0.25L + 0x1p-30L)`. No coefficient rounding term is missing. An arbitrary rounded decimal approximation or pow evaluation is not the same specified constant.

The prospective second-tier value would be:

```
E = RN(old_error*c).
```

Unlike halving, that last multiplication may round. The complete lower bound is

```
A >= SK(1-u)
E >= old_error*c*(1-u)
  >= Ku(1+delta)(1-u)^2 S.
```

It suffices to show

```
E/B >= (1+delta)(1-u)^2(1-2Ku) > 1.
```

All factors are positive. The right side decreases as K increases. At the maximum allowed length,

```
Kmax = 2^30 + 8 = 1073741832.
```

The exact rational check gives the lower ratio approximately `1.000000003608875`, with positive excess approximately `3.6088749752255164e-9`. The exact numerator/denominator and excess are in the JSON; the displayed decimals are not used to decide the inequality.

A short all-domain inequality also suffices without a large rational:

```
(1-u)^2(1-2Ku) >= (1-2u)(1-2Ku)
                    > 1-2u-2Ku
                    > 1-2^-32,
```

since `2K+2 <= 2^31+18 < 2^32`. Therefore

```
E/B > (1+2^-28)(1-2^-32)
    = 1 + 15*2^-32 - 2^-60
    > 1.
```

This explicitly covers both worst downward multiplications, including the extreme n endpoint; it is not a fit to measured rescue rates. Plain `old_error*.25L` loses the `(1+delta)` margin and is **not authorized by this bound**.

## Strict midpoints remain valid

Let q be the existing conversion of S to binary64 and L/U its adjacent midpoints. The positive finite binary32 input domain keeps q and its neighbors normal, finite and far from binary64 underflow/overflow. Each midpoint is exactly representable with at most 54 significant bits, and the source's sum then halving computes it exactly. S lies in q's rounding cell; S and either midpoint are within a factor two, so the margin subtractions are exact by Sterbenz.

Consequently the unchanged strict comparisons

```
S-L > E && U-S > E
```

imply `L < D < U`. Returning q is correct once-rounded binary64 squared distance. Exact ties, zero margins and uncertain cases still fall back. Projection sqrt semantics are unchanged; this proof is about canonical squared distance, not about replacing the later sqrt or ranking on unrounded real D.

Under the admitted controls E is also strictly smaller than old_error/2: `c(1+u) < 1/2`. Thus numeric acceptance can only expand relative to the current half-error tier for the same S/old_error; this is not a claim that a changed binary runs faster or has identical instruction layout.

## Implementation and observable-state caveats

- The original success branch must stay first, with its original calculation/comparisons verbatim. Evaluate the proposed second tier only after old failure and the same controls/format guard. Do not run the current half-error test and then add a third test unless that extra cost is separately intended.
- Do not replace `old_error*c` with `old_error/4 + 2^-30`: the latter adds an absolute quantity rather than the required relative margin. Do not move the factor into the old error expression or re-associate the accumulation merely because the real-number formula looks equivalent. This proof pins the two rounded products above, while preserving the old-success path is also an explicit compatibility objective.
- The new multiplication can set **FE_INEXACT** when the current exact `*.5L` step would not. All masked exceptions prevent a new trap, and controls need not change, but identical sticky FP flags are **not established**. If complete fenv status equivalence is required by the API, that is an additional constraint, not something this numeric proof silently waives. SQL result equality and unchanged controls are distinct from fenv flag identity.
- No enormous native allocation is needed to test n=2^32; the analytic maximum-K check handles that endpoint. Native correctness should cover realistic dimensions and all four tail lengths, subnormal/extreme float coordinates, equal vectors, exact/near midpoints and binade boundaries, plus the current guard tests. The new coefficient and both rounded arithmetic steps must be visible in code-generation review.
- A shadow-only classification can distinguish old success, current-half rescue, new-only rescue, and remaining integer fallback. It must remain outside timing. A paired whole-call measurement must include the actual guard, new multiplication, comparisons and remaining fallback. More rescues alone do not establish a speedup or clear official-score expectation.

**Scope:** rigorous numeric certification opportunity, with the fenv caveat above. Production edits 0; native builds 0; database runs 0; CI runs 0. The active tighter-only experiment remains unchanged.
