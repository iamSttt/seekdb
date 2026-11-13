# 补偿距离原型独立审查

结论：当前数学推导和源码未发现会错误认证 binary64 舍入值的阻断问题。允许进入独立 native 正确性与指令检查；这不是已编译通过、生产可采用或性能改善的结论。未修改原型、生产文件，也未启动编译、DB 或 CI。

审查对象是 `work/compensated-distance-candidate` 的 PROOF.md、compensated_distance.h、make_fixtures.py、test_compensated_distance.cpp。原 `reference_distance.h` SHA 为 7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9。文件快照 SHA 另见 `compensated-distance-independent-review.json`。

## 数学与实现核对

1. **精确减法准入（header 17–26、51–64）成立。** 非零 finite float32 按有效指数 `max(rawExponent,1)` 对齐，指数差不超过 28 时，最大整数幅度不超过 52 位，异号相加最多 53 位；double 可以精确容纳。零与任意 finite 数单独允许是安全的。该条件保守，不会把一般的 float32→double 减法误当成精确。SIMD 位检查与标量/Python 条件一致，先检查当前四坐标再执行 FP。

2. **FMA 与 TwoSum 对上公式（65–73）。** product 是独立乘法，residual 是 fused d*d−product；TwoSum 六步无 FastTwoSum 的幅值排序假设。有限输入、n≤4096 使全部平方和小于 2^270，非零各项/残差始终是 2^-298 格点的倍数，因而既无 double 溢出，也无残差下溢问题。所引用的 TwoSum 恒等式及 FMA 乘积变换在作者原论文 Theorem 3.4、Algorithm 3.5 中能对应；原型自身误差界仍须独立推导。[作者论文](https://ogilab.w.waseda.jp/ogita/math/doc/2005_OgRuOi.pdf)

3. **补偿误差预算足够。** 对每 lane，E=Σ(|e|+|q|)≤4(m+1)uD_l；t 与 low 累加误差≤(4m+1)uE≤16(m+1)^2u²D_l。四 lane 求和仍以总 D 计。八个 high/low 输入绝对和小于 4D，七次 LD 加法被 gamma_7(v)≤8v 覆盖，故 |F−D|≤[32v+16(m+1)^2u²]D。n≤4096 时 theta<1/2，D≤2F。K=64v+128(n+16)^2u²≥2theta，因此原型选的系数不依赖随机测例碰巧通过。

4. **向外界与中点认证（77–99）成立。** K 的 dyadic 计算在此范围可精确表示，额外 nextafterl 向上取值更保守；F*K 的下一 long-double 值覆盖乘法舍入。非零 D≥2^-298，避免 binary64 subnormal/zero 边界；相邻 double 中点可在 64 位 LD 精确表示，F 到中点的减法满足 Sterbenz 条件。两边严格大于 B 才认证，精确 tie 或不充分误差区间回到原 scalar。F=0⇒D=0 由全局相对界推出，不是依赖经验判断。

5. **FP/形状边界（32–39、107–121）一致。** 同时检查 MXCSR RN、DAZ/FTZ、异常屏蔽及 x87 RN、64-bit precision-control、异常屏蔽；不改变控制字。16≤n≤4096、n%4==0、有效指针、执行期间控制环境不变是明确前提。坐标/环境/形状/认证失败均落回未改的 scalar。原型明确不保证 sticky exception flags 与旧 x87 运算逐位相同，这个限制不能在将来生产声明中丢失。

## 测试审查与剩余条件

- Python oracle 按整数 2^-149 单位构造 D，然后整数 ties-to-even 生成 binary64 位串，没有借用候选 FP 算法。2/6 个 2^(e−27) 项确实分别构成偶/奇低端的精确中点；极小追加项和减一 mantissa 是中点邻近输入。源码已有复制 a/b 列表，避免后续修改污染先前 expected。
- 首次审查发现一个覆盖缺口：environment 测试覆盖 DAZ、FTZ、RC、PC，但未测试 MXCSR/x87 每个 exception-mask 位被清除时的拒绝。已通知实现者添加小型 attempt-only 拒绝检查；应清 sticky、设置/恢复环境后再断言，不在可能陷阱环境调用旧 scalar。
- 当前环境 fallback 数值比较可能被优化器复用计算；直接的 Outcome::environment 与控制字读回才是这部分关键断言。它不代替默认 RN 的独立整数 oracle。
- Native normal、ASan/UBSan、固定编译器实际指令尚待完成。必须确认 product 没有被融合消去、FMA residual 和 TwoSum 操作保留、归约使用 64 位 LD 精度；源级严格 flags/attributes 不能代替该检查。
- 成本诊断和小 kernel 对照仍独立于本数学判断。必须保留 late-coordinate/boundary fallback 的负面成本，不据理论正确就启用。
