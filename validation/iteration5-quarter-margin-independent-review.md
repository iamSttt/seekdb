# Guarded quarter-size certificate: mathematical review only

**Conclusion:** the proposed `RN(old_error * (0.25L + 0x1p-30L))` is conservative under the current strict x87 guard, finite binary32 inputs, `n <= 2^32`, unchanged four-lane arithmetic, normal intermediates and RN-even throughout the call. The existing strict midpoint certificate remains valid. No production edit, native build, DB run or performance experiment was performed; the active half-size candidate is unchanged.

Source inspected: `7bab1e96c6783ee5398028d5dc8843dad739b425:minivec/src/distance.h` (candidate header SHA-256 `bdcc9e587677b91c0e57a0da8dd63eda7160c62fb9121e22cf5db19d243bf7c7`). This review rechecks the bound for its existing operation graph; it does not certify different compiler reassociation or FP controls.

Let `D` be the exact squared distance, `S > 0` the computed long-double sum, `u=2^-64`, and `K=floor(n/4)+8`. Each contribution has at most K rounding factors: subtraction twice in its square, one square multiplication, at most `floor(n/4)+3` lane additions and two reduction additions. Nonnegative contributions give `|S-D| <= gamma_K D`; converting to computed S conservatively yields

`|S-D| <= B = [Ku/(1-2Ku)] S`.

The source uses `epsilon_L=2u`, so `2*epsilon_L=4u`. With `A=RN(SK)`, the existing error is exactly `A*4u`: this normal power-of-two scaling does not round. The new coefficient is

`c = 1/4 + 2^-30 = (1+delta)/4`, where **`delta=2^-28`**.

Both literals and their sum are exact in p=64: c has only 29 significant binary digits. Thus there is no coefficient-rounding factor. The new multiplication can round downward, as can the earlier SK product:

`E = RN((A*4u)c) >= Ku(1+delta)(1-u)^2 S`.

All these operations are normal in the stated domain. Finite binary32 differences/squares and their sums lie many thousands of exponent steps inside x87 range (`2^-298 <= D < 2^290` when D is nonzero); multiplication by K and the error coefficients cannot approach x87 overflow/underflow. Zero and nonfinite cases retain the existing fallback and are not admitted by this argument.

Consequently

`E/B >= R = (1+delta)(1-u)^2(1-2Ku)`.

For the entire range, `K <= 2^30+8` and `2u+2Ku <= 2^-33+18*2^-64 < 2^-32`. Since `(1-u)^2 >= 1-2u`, and the omitted cross product is positive,

`R >= (1+delta)(1-2u-2Ku) > 1+delta-2^-31 = 1+7*2^-31 > 1`.

This symbolic lower bound establishes the full domain, without fitting examples. A separate exact-Fraction evaluation at the worst endpoint gives `R-1 ≈ 3.6088749752255164e-9`, consistent with the proof. There are exactly two potentially downward rounded products in the displayed lower bound; neither a coefficient error nor a third rounding factor is silently omitted.

For `q=RN_binary64(S)`, its adjacent-double midpoints L/U are exactly representable in extended precision. S and these nearby midpoints satisfy Sterbenz's condition, so the computed margins `S-L` and `U-S` are exact. With `E > B >= |S-D|`, both strict comparisons exceeding E imply `L < D < U`; q is therefore the correctly rounded squared distance. A true midpoint tie cannot pass and still uses the exact fallback. No additional `nextafterl` inflation is needed for this exact coefficient and domain.

The guard must describe the arithmetic throughout the call, not merely a control word changed after accumulation. The conclusion covers the current no-fast-math operation graph and does not establish rescue frequency, runtime improvement, native-code validation or formal score improvement.
