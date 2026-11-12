# CI 43132750770：三版本配对性能复核

来源：integration/validation/raw/verify-20261112-parallel；AMD EPYC 9V74，seeds 7319/7320/7321。CDF、upstream72、fixed385均在同一个CI job中测量。下表比率始终为分子版本耗时/分母版本耗时，>1表示分母较快；中位数是三个配对比率的中位数，不是两组中位耗时相除。未换算或预测正式E。

## Bench：10k×128、100行批写、每模式1000查询（半过滤）、K10

| 配对 | 指标 | 三seed比率 | 中位 | 范围 |
|---|---|---|---|---|
| cdf-control_over_upstream72 | ingest_s | 5.099892, 2.008235, 0.557236 | 2.008235 | 0.557236–5.099892 |
| cdf-control_over_upstream72 | index_s | 0.939573, 0.983937, 1.069698 | 0.983937 | 0.939573–1.069698 |
| cdf-control_over_upstream72 | query_both_s | 1.041885, 1.049738, 1.054874 | 1.049738 | 1.041885–1.054874 |
| cdf-control_over_upstream72 | filtered_both_s | 1.065081, 1.081072, 1.076672 | 1.076672 | 1.065081–1.081072 |
| cdf-control_over_upstream72 | unfiltered_both_s | 1.023738, 1.025273, 1.037748 | 1.025273 | 1.023738–1.037748 |
| cdf-control_over_upstream72 | query_cpu_usec | 0.813880, 0.813193, 0.835513 | 0.813880 | 0.813193–0.835513 |
| cdf-control_over_upstream72 | peak_sampled_bytes | 1.036792, 1.081975, 1.050754 | 1.050754 | 1.036792–1.081975 |
| cdf-control_over_upstream72 | restart_s | 0.631711, 0.802464, 0.812481 | 0.802464 | 0.631711–0.812481 |
| cdf-control_over_fixed385-20261112 | ingest_s | 4.277439, 2.230763, 0.442488 | 2.230763 | 0.442488–4.277439 |
| cdf-control_over_fixed385-20261112 | index_s | 0.921372, 0.908278, 1.071074 | 0.921372 | 0.908278–1.071074 |
| cdf-control_over_fixed385-20261112 | query_both_s | 1.045632, 1.031938, 1.047194 | 1.045632 | 1.031938–1.047194 |
| cdf-control_over_fixed385-20261112 | filtered_both_s | 1.065428, 1.065363, 1.075808 | 1.065428 | 1.065363–1.075808 |
| cdf-control_over_fixed385-20261112 | unfiltered_both_s | 1.030056, 1.005955, 1.024973 | 1.024973 | 1.005955–1.030056 |
| cdf-control_over_fixed385-20261112 | query_cpu_usec | 0.826251, 0.781124, 0.830469 | 0.826251 | 0.781124–0.830469 |
| cdf-control_over_fixed385-20261112 | peak_sampled_bytes | 1.070628, 1.064874, 1.051761 | 1.064874 | 1.051761–1.070628 |
| cdf-control_over_fixed385-20261112 | restart_s | 0.823832, 0.797392, 0.750784 | 0.797392 | 0.750784–0.823832 |
| upstream72_over_fixed385-20261112 | ingest_s | 0.838731, 1.110807, 0.794078 | 0.838731 | 0.794078–1.110807 |
| upstream72_over_fixed385-20261112 | index_s | 0.980629, 0.923106, 1.001286 | 0.980629 | 0.923106–1.001286 |
| upstream72_over_fixed385-20261112 | query_both_s | 1.003597, 0.983043, 0.992720 | 0.992720 | 0.983043–1.003597 |
| upstream72_over_fixed385-20261112 | filtered_both_s | 1.000325, 0.985469, 0.999197 | 0.999197 | 0.985469–1.000325 |
| upstream72_over_fixed385-20261112 | unfiltered_both_s | 1.006171, 0.981158, 0.987690 | 0.987690 | 0.981158–1.006171 |
| upstream72_over_fixed385-20261112 | query_cpu_usec | 1.015200, 0.960564, 0.993963 | 0.993963 | 0.960564–1.015200 |
| upstream72_over_fixed385-20261112 | peak_sampled_bytes | 1.032635, 0.984194, 1.000959 | 1.000959 | 0.984194–1.032635 |
| upstream72_over_fixed385-20261112 | restart_s | 1.304128, 0.993679, 0.924063 | 0.993679 | 0.924063–1.304128 |

## 原始量与资源

| 版本/seed | 写入s | 查询2000条s | 查询CPU µs | 查询容器peak B | 含重启最大已采样peak B |
|---|---|---|---|---|---|
| cdf-control/7319 | 1.737054 | 0.698150 | 276405 | 18006016 | 18006016 |
| cdf-control/7320 | 0.771158 | 0.695965 | 270611 | 18489344 | 18489344 |
| cdf-control/7321 | 0.575583 | 0.702825 | 283797 | 17977344 | 17977344 |
| upstream72/7319 | 0.340606 | 0.670084 | 339614 | 13410304 | 17367040 |
| upstream72/7320 | 0.383998 | 0.662990 | 332776 | 13291520 | 17088512 |
| upstream72/7321 | 1.032926 | 0.666264 | 339668 | 13660160 | 17108992 |
| fixed385-20261112/7319 | 0.406097 | 0.667682 | 334529 | 13377536 | 16818176 |
| fixed385-20261112/7320 | 0.345692 | 0.674426 | 346438 | 13742080 | 17362944 |
| fixed385-20261112/7321 | 1.300788 | 0.671150 | 341731 | 13520896 | 17092608 |

CDF→fixed查询中位1.045632×（约耗时下降4.36%），主要增益在过滤组1.065428×；无过滤仅1.024973×。fixed相对upstream72中位0.992720×（约0.73%耗时增加），范围0.983043–1.003597，不能将小幅变化单独归因于安全修复。

查询CPU并未同方向改善：fixed 334529–346438µs，CDF 270611–283797µs；配对fixed/CDF增幅约20.41%–28.02%，中位21.03%。这与新增并行策略相容，但本轮没有单变量关闭并行，不能据此完成因果归因。未发现这些查询快照的CPU节流，但不能据此宣称无调度噪声。

内存指标是容器生命周期内memory.peak快照；fixed查询阶段13.38–13.74MB，含重启后最大采样16.82–17.36MB，CDF约17.98–18.49MB。应报告两种范围，不能只取查询较低值充当完整M。容器销毁后的指标为null，未保证覆盖最终写回。

after_index的io.stat均只有设备259:0：三seed CDF wbytes=6070272,wios=208,rbytes=0；upstream72/fixed wbytes同为6070272,wios=153，但rbytes=413696,rios=101。减少写请求次数并未减少该时点写字节，同时出现读取；不能简单宣称B全面改善。这些是可见设备计数，不是官方全过程B。

## Profile：独立诊断负载

| 配对 | 路径 | 三seed比率 | 中位 | 范围 |
|---|---|---|---|---|
| cdf-control_over_upstream72 | batch_statement_batch_commit_s | 1.624343, 0.637357, 5.392684 | 1.624343 | 0.637357–5.392684 |
| cdf-control_over_upstream72 | single_statement_batch_commit_s | 0.951675, 1.208931, 1.651899 | 1.208931 | 0.951675–1.651899 |
| cdf-control_over_upstream72 | single_statement_single_commit_s | 0.743973, 0.189763, 3.515157 | 0.743973 | 0.189763–3.515157 |
| cdf-control_over_upstream72 | batch_statement_batch_commit_insert_s | 1.957494, 1.734003, 1.938503 | 1.938503 | 1.734003–1.957494 |
| cdf-control_over_upstream72 | batch_statement_batch_commit_commit_s | 0.749757, 0.234374, 16.144725 | 0.749757 | 0.234374–16.144725 |
| cdf-control_over_upstream72 | empty_result_ms_per_request | 1.082414, 1.141106, 0.916209 | 1.082414 | 0.916209–1.141106 |
| cdf-control_over_upstream72 | primary_key_ms_per_request | 1.086080, 1.149265, 0.923702 | 1.086080 | 0.923702–1.149265 |
| cdf-control_over_upstream72 | vector_unfiltered_ms_per_request | 1.070445, 1.101141, 0.930271 | 1.070445 | 0.930271–1.101141 |
| cdf-control_over_upstream72 | vector_filtered_ms_per_request | 1.126263, 1.159429, 0.978502 | 1.126263 | 0.978502–1.159429 |
| cdf-control_over_fixed385-20261112 | batch_statement_batch_commit_s | 1.652091, 1.867357, 5.094712 | 1.867357 | 1.652091–5.094712 |
| cdf-control_over_fixed385-20261112 | single_statement_batch_commit_s | 1.097753, 1.220027, 1.639923 | 1.220027 | 1.097753–1.639923 |
| cdf-control_over_fixed385-20261112 | single_statement_single_commit_s | 0.624347, 1.054981, 2.481009 | 1.054981 | 0.624347–2.481009 |
| cdf-control_over_fixed385-20261112 | batch_statement_batch_commit_insert_s | 1.993907, 1.733164, 1.884647 | 1.884647 | 1.733164–1.993907 |
| cdf-control_over_fixed385-20261112 | batch_statement_batch_commit_commit_s | 0.764959, 2.689510, 13.997719 | 2.689510 | 0.764959–13.997719 |
| cdf-control_over_fixed385-20261112 | empty_result_ms_per_request | 1.083806, 1.050845, 1.004909 | 1.050845 | 1.004909–1.083806 |
| cdf-control_over_fixed385-20261112 | primary_key_ms_per_request | 1.090921, 1.079739, 1.008000 | 1.079739 | 1.008000–1.090921 |
| cdf-control_over_fixed385-20261112 | vector_unfiltered_ms_per_request | 1.074551, 0.990718, 1.026270 | 1.026270 | 0.990718–1.074551 |
| cdf-control_over_fixed385-20261112 | vector_filtered_ms_per_request | 1.131896, 1.048349, 1.084537 | 1.084537 | 1.048349–1.131896 |
| upstream72_over_fixed385-20261112 | batch_statement_batch_commit_s | 1.017083, 2.929843, 0.944745 | 1.017083 | 0.944745–2.929843 |
| upstream72_over_fixed385-20261112 | single_statement_batch_commit_s | 1.153495, 1.009178, 0.992750 | 1.009178 | 0.992750–1.153495 |
| upstream72_over_fixed385-20261112 | single_statement_single_commit_s | 0.839206, 5.559467, 0.705803 | 0.839206 | 0.705803–5.559467 |
| upstream72_over_fixed385-20261112 | batch_statement_batch_commit_insert_s | 1.018602, 0.999516, 0.972218 | 0.999516 | 0.972218–1.018602 |
| upstream72_over_fixed385-20261112 | batch_statement_batch_commit_commit_s | 1.020276, 11.475290, 0.867015 | 1.020276 | 0.867015–11.475290 |
| upstream72_over_fixed385-20261112 | empty_result_ms_per_request | 1.001286, 0.920900, 1.096812 | 1.001286 | 0.920900–1.096812 |
| upstream72_over_fixed385-20261112 | primary_key_ms_per_request | 1.004457, 0.939504, 1.091261 | 1.004457 | 0.939504–1.091261 |
| upstream72_over_fixed385-20261112 | vector_unfiltered_ms_per_request | 1.003836, 0.899719, 1.103194 | 1.003836 | 0.899719–1.103194 |
| upstream72_over_fixed385-20261112 | vector_filtered_ms_per_request | 1.005001, 0.904194, 1.108365 | 1.005001 | 0.904194–1.108365 |

fixed的空结果/主键请求中位约0.194/0.195ms，无过滤向量约0.372ms、过滤约0.288ms。往返与固定请求成本不可忽略，但不同SQL/输出负载不可直接相减充当解析/距离精确占比。

写入必须保留慢样本：bench fixed写入0.406/0.346/1.301s，而CDF为1.737/0.771/0.576s。配对中位虽2.230763×，范围0.442488–4.277439跨越明显回退；因此不能稳定宣称2.23倍。profile每100行一条INSERT且每100行提交的配对CDF/fixed均>1（中位1.86736×），保持提交数相同的单行INSERT批提交中位1.22003×；单行单提交则范围0.62435–2.48101，commit尾延迟波动大，不能把多种工作负载的中位耗时减法作为准确归因。

## 正确性与适用边界

全部9个bench报告ann_matches_exact=true、primary_key_tie=true，每份full_scan_oracle_query_indices为10项；每份重启检查10000记录、10向量。它是抽样全扫描oracle加两模式一致性，不是全部1000查询独立exact oracle。默认ANN探测比例100，本次不验证显式开启部分簇ANN时的召回。门槛/ASan/原72缺陷复现日志由主线程另行复核，不由性能JSON替代。

本轮支持保留安全修复：它修复已知生命周期/异常/协议风险且性能与upstream72接近。它不支持目标E>=6000已达成，也不支持将吞吐增益、内存下降和CPU上涨合成为未知官方分数。下一轮应分别稳定复测durable写入尾延迟、并行开关CPU/延迟与完整生命周期资源；不能按不同runner最好值拼接。

完整逐seed数据和所有配对比率见同目录verify-20261112-performance-analysis.json。
