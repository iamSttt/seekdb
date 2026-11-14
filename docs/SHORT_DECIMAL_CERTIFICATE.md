# Short-decimal v1/v2：正确性通过，性能不足，均撤回

当前生产 `sql.cpp` 已逐字节恢复为 `9ed5baa1b1363ebd388850e74271b353954902da`，生产 `src/short_decimal.h` 已移除。v2 原头文件精确归档到 `minivec/tools/short_decimal_experiment/short_decimal.h`；通用的 56 行 JSONL 数值回归继续保留。冻结计划及旧报告是当时的历史记录，不表示当前启用或采用。

v1 提交 `83bc170` 的正确性通过，但对父版本未体现写入 CPU 收益，2 CPU 查询 wall 回退约 7.36%；其旧计划、证书和全部 24 组微测保存在实验目录 `history/`，原始结果仍在 `validation/runs/43544203031/`。

## v2 实测与拒绝原因

本轮测量提交 `f788781b4f163a2410ebd7163ece85cc744c246a`，CI run `43547484521` 成功。完整日志以流式方式下载，命令退出码 0 后才提取，共 54 份报告；日志 SHA256 为 `6555f1c648b8b6ddbade1c2790463e2c37fb0b45d5529acad0d6d1d49fca8f1e`。原始报告位于 `validation/runs/43547484521/`，派生汇总见其中的 `short-decimal-decision-analysis.json`。

三个 native 构建（release flags、strict rounding、ASan/UBSan）均通过 554,561 条独立整数 oracle，结果一致；实际 ELF 协议通过 89 次完整响应比较、100,352 个 float32 坐标位模式回读及提交后重启检查。正确性没有被用于推断性能。

以下四指标均为候选耗时 / 参考耗时，低于 1 更快；每种 CPU 限额 6 对，AB/BA 各 3 对。

| 参考 | CPU 限额 | 查询 wall | 查询 CPU | 首生命周期 wall | 首生命周期 CPU |
|---|---:|---:|---:|---:|---:|
| EE | 1 | 0.876996 | 0.838172 | 0.938700 | 0.891913 |
| EE | 2 | 0.823519 | 0.748433 | 0.918268 | 0.850850 |
| parent9ed | 1 | 1.004574 | 0.999891 | 1.005062 | 1.005005 |
| parent9ed | 2 | 1.021253 | 1.006085 | 0.998674 | 0.992768 |

对 EE 的四项主阈值在两种 CPU 限额下均通过，且每项 6/6 对更快；这是整个候选程序相对 EE 的正面结果，完整 release gate 仍未通过，不能归因给短十进制解析器。对 9ed 的消融中，写入 CPU 比值为 1.012259 / 0.992021：1 CPU 的 6/6 对全部变慢，2 CPU 仅 4/6 对改善。

2 CPU 查询 wall 的顺序分组差异很大：AB 为 1.078767，BA 为 0.971353；生命周期 wall 也为 AB 1.032223、BA 0.965326。查询 CPU 的 AB/BA 则均略慢（1.004331 / 1.007839）。这不支持把 wall 的部分改善当成稳定的解析收益，也不能将两次独立对照块的中位数相除来估计贡献。

## 全部 24 组 parser 微测

下表为 baseline / candidate 的 wall 中位比值，高于 1 更快。每格固定 8 对 AB/BA，所有原始慢样本保留；不能换算数据库倍率或正式分数。

| 输入族 | 1 维 | 16 维 | 128 维 | 768 维 |
|---|---:|---:|---:|---:|
| short_fixed6 | 0.967141 | 1.347642 | 1.208793 | 1.208866 |
| mixed_25pct_exponent | 1.017040 | 0.998266 | 0.957824 | 0.956266 |
| all_exponent | 0.978322 | 0.971422 | 0.981337 | 0.987760 |
| all_17digit_decimal | 1.002257 | 0.904634 | 0.893378 | 0.895043 |
| float32_as_double_repr17 | 0.996506 | 0.876279 | 0.941589 | 0.954583 |
| midpoint_fallback | 1.012945 | 0.948535 | 0.982028 | 0.987799 |

fixed6 的 128/768 维约快 1.209 倍，但全 17 位十进制 128 维只有 0.893378 倍；该组 16,384 个坐标全部走快速路径，说明更多接受并不等于更快。实际 float32 扩展到 double 的 repr17 128 维接受 8,922/16,384 个坐标，仍只有 0.941589 倍；16 维为 0.876279 倍。长十进制的扫描、整数转换、除法及分支成本没有被吞吐收益覆盖。

## 编译结果核验与重现边界

已查看实际 release-flags 微测汇编，计时使用的 `round<true>`（不是带计数的独立预检）在第 3491/3695 行使用 `divsd`，3503 行使用 `cvtsd2ss`；3488 行是普通整数转换，3680–3687 行的高位 uint64 分支为 `shrq/orq/cvtsi2sdq/addsd`，随后回到同一除法点。中点保护实际编译为减去 `2^28-4` 后与 8 比较。所检查分支没有近似倒数替换。汇编文件为 `short-decimal/micro-translation-unit.txt`；这不表示生产 PGO 指令布局与微测完全相同。

要重现当时的完整 CI 和生产绑定，请使用独立 checkout `f788781b4f163a2410ebd7163ece85cc744c246a`。当前 `ci_short_decimal_1114.sh` / `verify_source.py` 会明确拒绝把归档实验当成当前生产绑定。归档快照仍可作为独立数值实验运行，原测试、oracle、54 份报告和 v1 负结果均保留。没有重跑实验、删除慢样本或发起正式评测。

## 冻结时的数学证书（保留历史前提）

以下数学和测试计划在 native 运行前写成；其中“待验证”的时间描述属于冻结时点，当前实测结果以上文为准。
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
