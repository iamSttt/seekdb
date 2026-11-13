# 用户报告 20 项优化：实施、取舍与提测准备

后续状态：用户已要求继续优化已知退化，当前实验基线为交付 `e020fdeb67dd8b64b77f0f8c2905f137f0112bba`；下文120对是上一阶段证据。后续138对等值/DOM实验与84、192对bulk实验尚未得到满足保护要求的默认候选，新宏继续关闭；[退化修复实验](REGRESSION_FOLLOWUP_20261113.md)与[bulk逐轮记录](BULK_TYPED_FOLLOWUP_20261113.md)保留全部正负结果。最后一个显式循环变体待复验，尚未新增正式提测。下文“保留版本”指已验证的311运行源码，不把当前实验HEAD或其他根镜像自动视为同一受测程序。

本轮起点为用户指定的 `a90b2b750e40f020793cddefe01d5b66631afc63`。方向来自用户报告，代码实现、实验和本文件分析由 assistant 完成。用户预测和判断按实际消息保存在两份相同的 `conversation.json`；早先三段文字的 assistant 来源审计继续保留，不把转贴改写为个人原创贡献。

保留版本的运行源码提交为 `311b45ffed1a957f8ebd71d0ead9320b1232298c`，实际根镜像的17项门禁、54种UTF8失败原子性检查及120对配对测量均通过，完整产物另经清单与源码哈希核验。保留原PGO，启用spill标量缓存、投影含至少128维向量时的类型化编码、复合过滤重复使用门控；I06及已测退化方案保持关闭。最后的材料和文档提交仅归档证据，不修改受测构建输入。

20 项均按报告逐项处理，但“实现并测量后不采用”不等于“未做”，也不等于 20 项全部默认开启。I01 的具体失败分支与 T/B/M 仍无法从平台取得，是未解决的外部信息缺口。正式成绩只有旧 ee299adb 的 E=3014.419039；没有新正式提测或6000分证据。

下列性能比值为基线耗时/候选耗时，大于1表示候选更快；它不是正式分数。不同主机、源码或PGO构建之间的倍率不直接相减或连乘。

| 编号 | 报告方向 | 当前状态 | 已完成、取舍和剩余边界 | 证据 |
| --- | --- | --- | --- | --- |
| I01 | 正式分项与 Prompt 失败细项 | 可见核查完成；细项仍未解决 | 已登录复查 7 条记录，最近仍是 ee299adb、E=3014.419039、Prompt 空且只有合并提示；T/B/M、失败分支、评审维度与锁定材料摘要不可见。已列缺失字段，没有代用户联系组织方或发起正式提测。 | [平台复查](<../validation/iteration3-platform-recheck.json>) |
| I02 | 负载矩阵与生命周期 | 已实施并测量 | 已覆盖多规模/维度/K/过滤/写读混合/投影/重启；记录初次构建、首查、写入和COMMIT，计时排除范围明确标注。有固定线程交错、A/A、三种子配对及资源代理；实际根镜像120对已完成。不同轮次的倍率不相乘，未知正式负载权重不作外推。 | [首轮矩阵](../validation/iteration-results-20261112.json)；[固定线程120对](../validation/iteration3-filter-fixed-repeat.json)；[实际镜像120对](../validation/iteration3-retained-final.json) |
| I03 | resident 元数据预算准入 | 已实施、已测、保留 | 按借用向量指针/PK/slot/容量及 posting、量化和构建临时量计预算，保持既有总预算；边界场景收益与构建负面均已记录。不能称总进程内存恒定或全面避免 spill。 | [实现](<../minivec/src/flat_index.cpp>)；[预算测试](<../minivec/tests/test_index_budget.cpp>)；[首轮结果](<../validation/iteration-results-20261112.json>) |
| I04 | 少量 UPDATE 避免整表落盘 | 已实施、已测、保留 | 有界 before-image 保留驻留，超预算再迁移；覆盖语句失败、回滚、多表、恢复与故障。与 I06 的增量索引是不同机制，I06 关闭不撤销本项。 | [实现](<../minivec/src/db.cpp>)；[驻留更新测试](<../minivec/tests/test_resident_update.cpp>)；[首轮结果](<../validation/iteration-results-20261112.json>) |
| I05 | 复合过滤与多列 posting | 有条件保留 | 首个复合过滤扫描使用原单列候选算法；成功非空后才允许同索引版本的多列选路，写入失效/重建重置。30对写查确认与实际四宏120对已过。一次写一次查仍有约0.6%中位负差，第二次缓存升级有代价；不把16次查询定为阈值或声称所有复用均获益。 | [30对](../validation/iteration3-filter-reuse-confirmation-raw.zip)；[实际镜像](../validation/iteration3-retained-final.json) |
| I06 | 稳定索引主体＋小型增量 | 已实施、已测；默认关闭 | 已从 UPDATE 扩至 INSERT/DELETE、exact 与默认 ANN 精确路径；独立 oracle 覆盖混合修改、同 PK 重插、重复 PK 失败、事务/重开、slot 复用、扩容、超过 64 项、同距与 tombstone。9 对完整周期有局部收益，但 warm 查询退步，保留实验宏 MVDB_ENABLE_INDEX_DELTA 为关闭。 | [混合变更测试](<../minivec/tests/test_index_delta_mutations.cpp>)；[故障测试](<../minivec/tests/test_index_delta.cpp>)；[27对中I06九对](<../validation/iteration3-six-finish-raw.zip>) |
| I07 | spill 紧凑过滤列与延迟取行 | 保留 | 有界缓存与最终TopK取行通过正确性、原PGO及写查周期验证；最终低/高命中生命周期约3.203/3.152倍。大倍率来自特定宽行负载；逻辑读取减少不等于物理I/O同比减少，仍有全量向量扫描。 | [写查27对](../validation/iteration3-six-finish-raw.zip)；[实际镜像120对](../validation/iteration3-retained-final.json) |
| I08 | 批量磁盘索引与 payload 迁移写 | 两部分均已实施、测量并保留 | 首轮磁盘 FlatIndex 构建缓冲与本轮 payload 首次迁移批写均已完成；超大记录直接写，失败不发布不完整存储，普通已落盘写入与 COMMIT 语义保留。payload 部分已做同源码四因子与固定线程重复对照；不是仍停留在“尚未实现”。 | [阶段A](<../validation/iteration3-stage-a-performance.json>)；[四因子重复](<../validation/iteration3-filter-fixed-repeat.json>)；[迁移故障](<../minivec/tests/test_payload_batch.cpp>) |
| I09 | 维度/K/剪枝成本的并行策略 | 候选已实施、已测；不采用 | MVDB_ENABLE_WORKLOAD_FANOUT 保持关闭。默认及 high768 K=1 等负面仍保留；4 个 cgroup 配对可解释部分限额现象，单对不能证明普遍改善。继续既有并行策略。 | [96对＋4资源配对](<../validation/iteration3-hotpath-resource-analysis.json>) |
| I10 | 分块安全下界提前拒绝 | 16/32/64候选已实施、已测；不采用 | 严格安全拒绝与 canonical exact/PK 规则保留，但 standard 与 large100k 的三种分块均负，继续原完整整数距离界；不拿超过量化预算的高维场景改善证明整数 early-exit 有效。 | [实现](<../minivec/src/quantized_bound.h>)；[负结果与决策](<../validation/iteration3-hotpath-resource-analysis.json>) |
| I11 | 分块量化尺度与量化预算 | 实际分块方案已实施、已测；不采用 | 不是只做旧 residual-radius 尝试：新增 256 行 power-of-two 分块、最多八尺度查询缓存、统一预算与失败回退，并做 broad/selective 两级验证。105 对＋3 A/A 显示部分离群场景获益，但每行坐标离群及 low16 控制持续退步；两个尺度宏均默认关闭，80%/4×/128 阈值未被证明最优。 | [初筛](<../validation/iteration3-quantization-screen.json>)；[完整确认](<../validation/iteration3-selective-scale.json>)；[数值测试](<../minivec/tests/test_selective_scale.cpp>) |
| I12 | 先导候选收紧 TopK | 32/64/128候选已实施、已测；不采用 | 额外 exact pilot 与现有 norm 排序结合未稳定摊销成本，standard/large100k 三档均负；MVDB_PILOT_COUNT 默认不定义。不是遗漏实现。 | [候选实现](<../minivec/src/exec.cpp>)；[负结果与决策](<../validation/iteration3-hotpath-resource-analysis.json>) |
| I13 | 解析/token 分配 | 首轮复制省略保留；token复用已测不采用 | 跳过执行入口不需要的 Statement.raw 复制已保留；本轮 bounded owning-token/TLS 复用有单测、故障、分配微测和同原 PGO 27 对。默认基本持平且 low16 回退，MVDB_ENABLE_TOKEN_REUSE 保持关闭；不能把微基准分配下降当完整查询收益。 | [解析确认](<../validation/iteration3-parser-pgo-confirmation.json>)；[实现](<../minivec/src/sql.cpp>)；[测试](<../minivec/tests/test_parser_reuse.cpp>) |
| I14 | 减少逐行 JSON DOM | 受限保留 | 全量typed方案曾退化，未通用启用；仅投影含至少128维向量时走typed writer。实际128/768维宽输出含写入计时总和约1.221/1.231倍；16维同类总和0.985，负面保留。128测试只覆盖已测1024行负载，非所有维度的最优阈值证明。 | [63对](../validation/iteration3-output-guard-raw.zip)；[实际镜像120对](../validation/iteration3-retained-final.json) |
| I15 | O3/LTO/PGO | 原PGO保留 | 固定GCC13.3/O3与原构建期训练；LTO和扩展spill训练未取得稳定净收益。当前四宏镜像另经独立120对验证，不能借旧binary SHA证明新构建。未来PGO重建字节可能不同。 | [六构建矩阵](../validation/iteration3-pgo-matrix.json)；[当前构建](../Dockerfile)；[实际镜像](../validation/iteration3-retained-final.json) |
| I16 | 材料语义压缩 | 完成 | 保留真实user聊天及来源；报告明确为逐字摘录，完整原件按原始字节归档。e020交付时两份材料相同、129117字节；后续退化修复阶段无损归档早期assistant指标表，新增记录仍须低于131072上限；原件/摘录与引用哈希校验不等于官方评分。 | [材料](../conversation.json)；[完整原件](../validation/iteration3-user-report-original.md)；[指南](CONVERSATION-GUIDE.md) |
| I17 | 本人实验闭环 | 当前反馈链已记录 | 较新用户对读写摊销的判断推动了首查门控，实现/30对及组合120对由assistant执行并记录，保留全部正负。旧三段已定位assistant来源，不计为独立用户贡献；新消息按实际user提交保存，不由哈希认证原创或承诺学习分。 | [来源审计](../validation/iteration3-source-attribution.json)；[当前记录](../conversation.json)；[门控30对](../validation/iteration3-filter-reuse-confirmation-raw.zip) |
| I18 | 第二场景知识迁移 | 新事前预测→12对实测→用户判断已记录；保留来源限制 | 新的宽字符串spill写查预测、测量后对固定成本摊销的判断已按当前user消息逐字保存；实际实现和测量归assistant。旧spill预测来自早先assistant，不计独立作者证据；新链也不自动证明独立作者认证。收益机制是少做宽行谓词材料化，不能声称物理IO收益或官方涨分。 | [当前预测/反馈](<../conversation.json>)；[写查完整周期](<../validation/iteration3-six-finish-raw.zip>)；[来源审计](<../validation/iteration3-source-attribution.json>) |
| I19 | 历史原始聊天定位 | 来源调查完成 | 三段原始assistant消息及来源线程已定位；保留用户提交的原字句和角色，同时标明内容原作者。未增加未经证实的原创贡献。 | [来源定位记录](../validation/iteration3-source-attribution.json)；[审计字段](../conversation.json) |
| I20 | 材料契约、文档与校验入口 | 完成 | 当前文档与材料指向实际受测四宏版本，旧105对/旧清理报告明确为历史。两副本、容量、角色、引用、附件重建与证据hash机械检查通过；它不证明官方Prompt有效或隐藏用例得分。 | [只读校验器](../minivec/tools/validate_iteration_material.py)；[材料指南](CONVERSATION-GUIDE.md)；[当前验证](../validation/iteration3-retained-final.json) |


## 准备度与正式提交边界

结论：可作为由用户决定是否提交的候选版本，未发现正确性、资源失控或已定位的严重机制阻断。该结论包含已知性能取舍，不表示全面提速。19项完成实施验证、取舍或真实反馈记录；I01可见核查已完成，但外部细项仍缺，不能写成20项全部取得预期结果。新门控第二次查询的缓存升级有成本，重复查询不保证每一种短生命周期都获益；128维守卫也只在实际测量覆盖范围内解释。

用户已授权评估后推送仓库并交付分支及完整提交号。**正式平台提测只由用户决定并操作，助手不点击提测或消耗次数。** 可提交候选不代表已达到6000分，也不代表旧Prompt失败已经解决。

## 本次实际镜像的关键结果

本次比较基线为 `969ecca98f817ecdef9b0c378fc99a0d6cef510d` 的原PGO构建。比值来自各配对样本，不是两个独立中位数之比。标准/边界各15对，spill过滤与投影各3对；后者仍是有限覆盖。

| 场景 | 首轮查询比值 | 重启后查询比值 | 已计时生命周期或阶段总和比值 |
| --- | ---: | ---: | ---: |
| standard10k | 1.0023 | 0.9959 | 1.0084 |
| resident30k | 0.9192 | 1.0738 | 1.0063 |
| resident50k | 0.9988 | 0.9934 | 0.9949 |
| low16 | 1.0830 | 1.0493 | 1.0254 |
| filter60k | 1.0239 | 0.9792 | 1.0080 |
| update10 | 0.9818 | 0.9985 | 1.0049 |
| spill | 1.0607 | 1.0182 | 1.0573 |
| spill_compound | 47.1318 | 46.4408 | 3.2027 |
| spill_high | 31.9709 | 30.6991 | 3.1518 |
| wide768（投影） | — | — | 1.2306 |
| many16（投影） | — | — | 0.9848 |
| wide128（投影） | — | — | 1.2213 |

普通查询接近持平。30k首轮64条查询的耗时约增加8.79%，配对中位多1.805ms，10/15对慢且双方均双线程；60k过滤重启后约增加2.12%，13/15对慢且三个seed一致。many16计时总和0.9848，2/3对慢。原因未定位，不将其称为噪声。默认重启filtered ANN子组0.9727；排除唯一线程数不匹配配对的敏感性结果仍负，主分析120对没有删除样本。

投影总和包含写入和七种测量阶段，**不是完整端到端wall time**，没有CPU/RSS/IO采样；其他生命周期不含重启oracle wall。资源快照不能代替正式M/B。没有按未知正式负载权重将倍率平均、连乘或换算为6000分。

运行：[GitHub Actions 43350596233](https://github.com/iamSttt/seekdb/actions/runs/43350596233)。[摘要](../validation/iteration3-retained-final.json)、[完整原始产物](../validation/iteration3-retained-final-raw.zip)、[分析与校验脚本](../validation/iteration3-retained-final-analysis.zip)均已归档。实际镜像ID与程序SHA以摘要和材料selected_runtime为准；没有正式平台提测。
