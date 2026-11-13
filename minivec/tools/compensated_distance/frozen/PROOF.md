# AVX2/FMA 补偿距离原型：准入与保守认证

这是独立原型，不是已采用实现。原 `reference_distance.h` 是 EE/R 相同的 distance.h 原字节，SHA256 `7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9`。任何拒绝都调用未改动的 `mvdb::distance_scalar`。未做 C++ 编译、DB 运行或性能测量。

引用范围：Knuth TwoSum 的误差无损恒等式、TwoProductFMA 和普通求和的 gamma 界依据 [Ogita–Rump–Oishi, Accurate Sum and Dot Product, Algorithms 3.1/3.5, Theorem 3.4 and equation 2.4](https://ogilab.w.waseda.jp/ogita/math/doc/2005_OgRuOi.pdf)。下述输入准入、四 lane 误差预算、按计算值认证和舍入单元证明是本原型的独立推导，不是该论文直接给出的本算法结论；不据论文的速度数字预测本程序速度。

## 1. 精确目标与准入

目标 D 是所有有限 float32 输入在实数域的 Σ(a_i−b_i)²，再一次按 nearest-even 舍入到 binary64；不同 D 舍入相同仍按原 PK tie 语义处理。

仅尝试 x86-64 GNU/Clang、AVX2+FMA、LDBL_MANT_DIG=64，16≤n≤4096 且 n 为 4 的倍数。MXCSR 必须 RN、DAZ/FTZ 关闭且异常 masked；x87 必须 RN **且 precision-control=64 位**，异常 masked。检查 x87 RN 而不检查 precision-control 不够：53/24 位运算不能沿用 64 位 long-double epsilon 界。本原型不修改控制字。调用中途控制环境不得变化。

每组四个坐标先用整数位检查 finite。写 float32 为 ±M·2^(E'−150)，E'=max(rawExponent,1)，M 最多 24 位（subnormal 无隐含位）。非零 a、b 只在 |E'_a−E'_b|≤28 时准入；零与任意 finite 值的差也准入。对齐最小指数后，每个整数幅度最多 24+28=52 位，减法的最坏幅度不超过 53 位，因此该精确差可由 binary64 表示。实际 SIMD `double(a)-double(b)` 无舍入损失。这是充分条件，拒绝更多本可精确处理的输入是允许的。

所有 float32 都是 2^-149 的整数倍。因此 d² 是 2^-298 的整数倍；非零平方至少 2^-298，全部 D<4096·2^258=2^270。后续误差无损的积残差、TwoSum 残差及累加同在该格点上，不会出现 binary64 下溢；高量级也远离 binary64 溢出。坐标 subnormal 不等于计算平方在 double 下溢。

## 2. 四个独立补偿 lane

以下 u=2^-53 是 binary64 unit roundoff，v=2^-64 是 64 位 long-double unit roundoff。C++ epsilon 分别是 2u、2v。每 lane 有 m=n/4≤1024 项；先对一 lane 证明，最后按其非负 D_l 求和。

对每个已精确计算的 d：p=RN64(d·d)，q=FMA(d,d,−p)。因没有溢出/下溢，p+q=d² 精确，|q|≤u d²。

Knuth TwoSum(h,p) 返回新的 h'=RN64(h+p) 及 e，使 h'+e=h+p 精确。代码逐个 intrinsic 保留以下次序：s=h+p；z=s−h；e=(h−(s−z))+(p−z)。不能把它替换成有排序前提的 FastTwoSum，也不能重结合成零。

低部采用 t=RN64(e+q)，l'=RN64(l+t)。最后精确恒等式 D_l=h_m+Σ(e_i+q_i)；只有低部的两层普通求和存在补偿误差。

## 3. 独立保守误差预算

令 P=Σp_i。因为 d² 非负，P≤(1+u)D_l。普通非负高部累加满足 h_j≤(1+u)^j P_j，而 m≤1024，(1+u)^m<2。故每次 |e_i|≤u(h_{i−1}+p_i)≤2uP≤3uD_l。于是

E=Σ(|e_i|+|q_i|)≤(3m+1)uD_l≤4(m+1)uD_l。

单次 t 舍入总误差至多 uE，且 Σ|t_i|≤(1+u)E<2E。低部的 m 项普通求和误差至多 gamma_(m−1)·Σ|t_i|，由 gamma_m=mu/(1−mu)≤2mu，得到至多 4muE。因此

|l_m−Σ(e_i+q_i)|≤(4m+1)uE≤16(m+1)²u²D_l。

四 lane 相加后，同样得到精确八项实数和 H 与目标 D 的差：

|H−D|≤16(m+1)²u²D。

各高部非负，Σh_l<3D；由 Σ|t|<2E 与低部误差，Σ|l_l|<3ΣE_l≤12(m+1)uD<D。故八个 double 高/低项的绝对和 <4D。

代码先四 high 树形相加（3 次 long-double 加法），四 low 同样（3 次），最后相加（1 次），总 7 次。double→long double 精确；任何固定求和树的绝对误差被 gamma_7(v)·Σ|inputs| 覆盖，gamma_7(v)≤8v。因此计算出的 long-double F 满足

|F−D|≤θD，θ=32v+16(m+1)²u²。

在限定 m 下 θ<1/2。D>0 时 F≥(1−θ)D>0，故 D≤2F，进而 |F−D|≤2θF。F=0 则必然 D=0，可返回 +0；这不依赖“浮点看起来为零”猜测。

## 4. 从公式到可执行上界

采用 root 提议的系数之前已按上述推导核验：

K=32 epsilon_L +32(n+16)² epsilon_D²
 =64v+128(n+16)²u² ≥64v+32(m+1)²u² =2θ。

因此 B≥F·K 就足以包含真实 D。n≤4096 时整数 (n+16)²、系数 32 及二进制幂的乘积可精确表示在 64 位 long double；K 的两个 dyadic 项相加也不超过该精度（把 K 乘 2^104 后为 2^46+32(n+16)²，少于 48 位）。实现仍将 K 和 F·K 各用 `nextafterl(...,+infinity)` 向上取一格，显式保护非精确最后乘法。所有 B 运算都远离 long-double 下溢/溢出。

这证明 root 提议的 32 系数在**本实现和额外 x87 精度准入**下足够；不是通过有限随机实验确认系数。

## 5. 正确舍入认证

r=RN64(F)。r 与相邻 double 的两个中点 L/U 最多需要 54 位，因此在这里的 64 位 long double 可精确表示。r>0 且有限；D 的限定范围保证没有 zero/subnormal/infinity 的特殊舍入边界。F 在 r 的舍入单元内，与 L/U 的比值接近 1，因此中点差的 long-double 减法由 Sterbenz 精确，实际比较不会向上抬高距离而误认证。

仅当 F−L>B 且 U−F>B 两个**严格**条件都满足，D 才被证明严格位于 r 的舍入单元内部，可返回 r。边界、exact tie、认证不充分均进入原 scalar，不能猜 tie 的方向。即使输入域满足准入，也允许大量 boundary fallback；正确性与认证率是两个问题。

## 6. 编译与接口约束

- 不允许 fast-math、associative-math 或 contraction 改写 TwoSum；测试命令明确 `-ffp-contract=off`，GCC 内核另用 no-fast-math / fp-contract=off 属性。p 的独立乘法必须存在，q 是显式 FMA residual intrinsic。将来 Linux 还需看实际反汇编确认没有重结合或漏掉残差，源码审查不等于指令验证。
- `kernel` 是实现内部，前置条件由 `attempt` 检查；外部只能调用 attempt/distance。传入指针在 n 个 float 范围内有效且不同时修改。
- 非标准 FP 环境走原 scalar，**不声称原 scalar 在这些环境下等于正常 RN 数学值**，只要求本包装保持原行为。极端 exponent gap / NaN / Inf 同样回退。
- sticky exception flags 可能不同：补偿 double 算法与原 x87 运算序列不一致。本原型仅主张数值返回和控制字保持，未主张逐 exception flag 等价；若生产约束要求异常 flags 完全等价，该方向须停下或补证明/实现，不可忽略。
- 返回 Attempt 的诊断字段、每四坐标检查、FMA/TwoSum 的额外指令也可能抵消 SIMD 收益；当前不能称更快。

## 7. 测试计划与现状

`make_fixtures.py` 独立把 float 位串解成单位 2^-149 的有符号大整数，先精确平方求和，再按整数位和 ties-to-even 一次生成 double。已生成 7842 案例，含全部有限 float exponent、mantissa 角点、subnormal、±0、相反号、gap28/29/30、维度边界、精确中点及邻近值、随机动态范围、4096 维。此 oracle 不调用候选或参考距离函数；3.82 MB 二进制 fixture SHA 在 oracle.json。

`test_compensated_distance.cpp` 会同时核对每个已认证值、包装结果、原 integer exact 和 scalar 与该独立结果；还验证 shape/coordinate/boundary 分支都真正触发，非标准 MXCSR/x87 控制回退、控制字未改、NaN/Inf fallback。无 AVX2/FMA 返回 77/UNSUPPORTED，不能伪称通过。

当前仅已运行 Python 预检，**未运行 C++**。生成器一次边界循环误造负 exponent 后已修正（high 最小值改为 31）；随后 Fraction 重放发现中点测试保留可变数组别名，已改为每例复制并重新生成全部 fixture。现每个存储输入的 expected bits 经 Python Fraction 独立转换交叉核对，7842 个全部通过；247014 个纯整数减法准入角点也通过。两项生成器失败/修复记录在 preflight.json，不作为 native 成败证据。后续需普通 O3、ASan/UBSan、真实指令审计及成本占比诊断；只有成本足够且正确性通过才有资格进入独立性能实验。

建议的 Linux 正确性命令（未执行）：

```sh
python3 make_fixtures.py --output oracle.bin
g++ -O3 -std=c++17 -ffp-contract=off -fno-math-errno test_compensated_distance.cpp -o normal
./normal oracle.bin
g++ -O1 -g -std=c++17 -ffp-contract=off -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie test_compensated_distance.cpp -o sanitized
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./sanitized oracle.bin
```
