# Experimental certified lazy exact Top-K, 128 dimensions

Status: experimental candidate integrated after the `9ed5baa1b1363ebd388850e74271b353954902da` runtime. At integration time no native correctness or performance result is claimed. See `validation/lazy128-plan-20261114.json` and subsequent raw CI reports for execution evidence and adoption decisions.

## Scope and canonical value

The suggested patch adds one branch: explicitly exact mode (`!ann_mode`), an immutable resident vector view, no filtering predicates, 128 coordinates, ascending distance, bounded `1 <= K <= 32`, and the existing fan-out admission decision. It changes neither that admission policy nor ANN, descending, filtered, spill, serial, other-dimensional, persistence, or output paths. One-CPU quota/affinity does not imply the existing policy declines fan-out: its hardware/latency policy is inherited unchanged. The isolated heap supports K through 1024 for stress/oracle testing; production admission is intentionally narrower.

For finite binary32 inputs, write

`D = sum_i (a_i - b_i)^2` in exact real arithmetic,

`R = round_binary64_to_nearest_even(D)`.

The original engine's ranking key is `(R, signed int64 primary key)`. Equal rounded distances remain ties even when their unrounded real distances differ. The new materializer calls the existing integer `distance_exact` / `ExactSum`, **not** the certified long-double `distance_squared` shortcut. This deliberately avoids dependence on the x87 precision-control setting and may have a greater cost per exact call. Neither projected distances nor the primary-key tie rule changes. Surviving candidates contain the original canonical `R` before the original merge/output path uses them.

## Floating-point admission and fallback

The floating certificate is admitted only for an x86-64 compiler supported by the existing SIMD header, `FLT_EVAL_METHOD == 0`, no `__FAST_MATH__`, enabled SIMD bound compilation, an available existing bound kernel, nearest rounding in `fegetround` and MXCSR, neither DAZ nor FTZ, and all six MXCSR floating-point exception masks set. The exception-mask test is needed because overflow can trap *before* an `isfinite` fallback can inspect its result. Sticky exception-status bits do not invalidate the certificate. Compile without reassociation/unsafe-math options. Existing kernels have `no-fma` target attributes.

The database's finite-input validation remains a precondition; this prototype does not add a per-coordinate finite scan. Query and row pointers must address exactly 128 live, immutable binary32 coordinates. Floating-point controls must remain unchanged for each half's lifetime, as for the original query-scoped bound context.

The outer SQL gate declines this branch when its calling-thread environment is unsupported, retaining the prior engine path. Each half constructs its own envelope context and checks its own environment. An individually disabled envelope performs no new float-bound rejection and materializes integer exact values. This is not a new claim about the original engine's behavior in arbitrary trapping/nonnearest environments. The prototype does not change thread floating-point settings or assert that unsupported original paths are newly certified.

Within an admitted context, a negative or nonfinite float-sum result yields per-row integer exact fallback. In particular, finite extreme binary32 inputs can make float subtraction, squaring, or reduction overflow even though `R` is finite. Such rows are never rejected from a nonfinite float sum. Zero float sums remain covered by a positive absolute error allowance; zero is not silently treated as exact zero. All interval arithmetic uses binary64 outward rounding. No interval uses NaN as an endpoint.

## A two-sided certificate, including underflow

Let `n = 128`, `u = 2^-24`, `eta = 2^-150`, `m = n + 12`, and let `S` be the result of **one** call to an unchanged existing float-sum kernel. Consider only finite nonnegative `S` under the admitted environment.

1. Each input is an integer multiple of `2^-149`. A subnormal difference of two binary32 inputs is therefore representable exactly. A normal finite subtraction has relative error at most `u`; squaring that difference contributes two subtraction factors.
2. Rounded multiplication has relative error at most `u` when its result is normal and absolute error at most `eta` when it underflows. This includes a product rounded to zero.
3. All accumulated terms are nonnegative. A subnormal float addition is exact, and a normal finite float addition contributes at most one relative factor `1 +/- u`. Binary64 reductions, when compiled in the existing reference reduction mode, have smaller error and are also covered by this bound. Initial additions to zero are exact.
4. Every term has at most `n-1` nonzero additions on its route to the result. The unchanged SSE2/AVX2/AVX-512 positive-term reduction trees satisfy this bound. The deliberately larger `m=n+12` covers subtraction twice, multiplication, summation, reduction, and conversion. The 128-dimensional kernels have no scalar remainder.
5. Because all squared terms and reductions are nonnegative, any overflowed term or sum cannot later cancel into a finite `S`; the nonfinite check excludes it. NaN also falls back.

Consequently the usual factor-by-factor propagation gives both

`S <= (D + n*eta) * (1+u)^m`,

`S >= D * (1-u)^m - n*eta`.

For the upper computation inequality, the at-most-`eta` positive errors are conservatively propagated through the larger product of rounding factors. For the lower inequality, each negative `eta` error is followed only by factors at least `1-u` and at most one for this lower-bound construction, so subtracting `n*eta` suffices. Bernoulli/convexity give `(1+u)^(-m) >= 1-m*u` and `(1-u)^m >= 1-m*u`. Thus

`D >= S*(1-m*u) - n*eta`,

`D <= (S+n*eta)/(1-m*u)`.

Every nonzero exact squared distance here is at least `2^-298`. Since each finite input magnitude is less than `2^128`, `D < 128*2^258 = 2^265`. Therefore `D` and its once-rounded binary64 result are either zero or normal binary64; no canonical underflow or overflow occurs. With `u64=2^-53`, `D*(1-u64) <= R <= D*(1+u64)`.

Put `x=m*u`, `r=4*x=(n+12)*2^-22`, and `a=4*n*eta=n*2^-148`. Here `0<x<0.004`; the following elementary bounds have ample slack:

`(1-x)*(1-u64) >= 1-4*x`,

`(1+u64)/(1-x) <= 1+4*x < 4`.

Combining these inequalities, including `D=0`, gives the certificate used by the code:

`S*(1-r)-a <= R <= S*(1+r)+a`.

Both constants `r` and `a` are exactly representable binary64. The code computes `E = up(up(S*r)+a)`, `L = max(0,down(S-E))`, `U = up(S+E)`. Here `up` and `down` each move one representable binary64 neighbor outward after nearest-rounded arithmetic; `down(0)` is clipped to zero. Consequently `L <= R <= U`. An unexpected nonfinite `U` falls back, although the admitted finite-`S`, 128-dimensional bounds are far inside binary64 range. This is a certificate around **canonical RNE distance**, not merely real `D`.

## One sum and the old cached rejection

`EnvelopeContext128::sample` calls the existing float kernel exactly once for every row that reaches it. It first applies the unchanged cached-cutoff arithmetic from `DistanceBoundContext`; only survivors pay for `L,U`. The threshold may be an exact kth key or any certified K-witness ceiling.

Let `T` be finite and nonnegative. Store `denominator=down(1-r)`, compute an upward-rounded numerator `T+a`, then an upward-rounded quotient. Since `R >= S*(1-r)-a`, `S > cutoff` proves `R > T`. Equality is never rejected. Overflow of the numerator or quotient makes the cutoff infinite. The exact source uses the same arithmetic operations and cache invalidation condition as the old context.

The unit test separately obtains an unthresholded interval for every numeric sample, including samples later rejected, and compares it against the integer `ExactSum` oracle. It then checks all rejected samples satisfy `R > T` and compares finite-sum rejection against the original `DistanceBoundContext`. A fallback sample receives no unproved finite enclosure or rejection.

## Heap order, stable slots, and lazy materialization

Each half allocates `K+1` slots once and a vector of at most `K` slot indices. A slot holds a stable row pointer, signed PK, `[L,U]`, and optional cached `R`. There is exactly one scratch slot outside the heap; no row vectors or strings are copied into it.

For two different slot indices `a,b`:

- If `U_a < L_b`, return `a` better; then `R_a < R_b`.
- If `U_b < L_a`, return `a` not better; then `R_b < R_a`.
- Otherwise materialize/cache both exact values and compare `(R,PK)` by the unchanged rule. Touching intervals count as overlap; overlap is never treated as equality.

Every call therefore returns the result of one fixed, strict total order on unique database primary keys (and a strict weak order even in a helper test with duplicate pairs). Tightening cached intervals cannot change that order. Standard heap algorithms may copy the comparator: each copy references the same private slot pool. The values compared by the algorithms are indices and remain unmodified. Only the external, query-local memoization cache changes. This avoids the risk of caching in moveable heap values or modifying comparator arguments.

On replacement, `pop_heap` completes while all old slot IDs are valid. The scratch ID replaces the evicted ID at the back, then `push_heap` completes. Only after these operations is the evicted slot reused as the next scratch slot. A rejected scratch row never aliases a live heap slot. Filling uses sequential slots; after the Kth insertion, slot K is the scratch slot. The rows and query outlive every comparison.

## K-witness bounds and both halves pruning

When a local heap contains K distinct rows, its root is the worst canonical key. Hence every one of these K actual distances is at most the root's actual distance, itself at most `root.U`. Other entries' own upper bounds may be larger; that does not invalidate the root witness.

The root's interval upper endpoint need **not** decrease when the root changes. Thus the published local ceiling is `min(previous valid local ceiling, current root.U)`. Old witnesses remain real immutable rows even if evicted. A row strictly above any such ceiling already has K other rows with smaller canonical distances and cannot belong to the global Top-K. Primary-key ties remain in play because pruning is strictly `>`.

Every 64 visited rows a half publishes only its own decreasing local witness and reads the peer's atomic ceiling. The effective pruning ceiling is the minimum of valid local and peer history. Receiving a peer ceiling does not make a non-full local heap into a new local witness. If `K > rows_in_half`, that half never publishes; it can nevertheless safely prune from the other half's K distinct actual rows.

Mutual pruning is not circular: each first finite publication requires a concrete full local heap, all of whose row identities are distinct. Each later publication again refers to a full local heap, regardless of earlier pruning. No inference counts a peer's rows twice to invent a K-row witness. A globally eligible row cannot be discarded by distance pruning; within each local heap, any row evicted by K strictly better canonical keys is also globally ineligible. Thus merging both local survivors still contains all global winners, including arbitrary cross-half PK ordering and exact-distance ties. Empty halves and `K > total rows` are covered without a finite witness.

Two separate `alignas(64)` single-writer atomics carry doubles. Relaxed loads need only obtain one valid old/new value, not publish any pointer or row storage. Every finite stored value has a valid witness, so stale values only weaken pruning. Non-lock-free atomic-double platforms use the local-only policy. The existing pool join still waits for both halves even on exceptions before captured query/view state is released.

## Finalization, memory and cost questions

At scan end each half materializes at most K live slots and converts them into unchanged `Candidate` objects. The existing merge sees at most 2K exact candidates and uses the original comparator/output. Earlier overlap materializations are cached, so finalization performs at most K *additional* exact computations per half. The candidate pool, pointers, intervals and witnesses are query-local; no metadata, mutation invalidation, serialization or persisted format changes are introduced.

Extra costs include K+1 slot allocation per half, index indirection in heap operations, two endpoint calculations for every float-bound survivor, overlap-driven integer `ExactSum` calls (potentially slower than old certified long-double calls), and potentially looser published ceilings than exact kth values. The expected benefit is eliminating exact distance computation for many candidates that enter or lose heap comparisons with disjoint certificates. This is a hypothesis, not a speedup result. Large tie groups, narrow distance clusters, tiny K/query batches, integer-exact fallbacks, or all-overflow inputs may lose.

Integration is conditional on native oracle and sanitizer validation followed by the same paired EE benchmark used for the parent candidate, against both the current parent and the formal E=3014 baseline. Counts of avoided exact calls alone do not establish query-wall or lifecycle gains. The original branch remains the control/fallback and no official score is predicted.
