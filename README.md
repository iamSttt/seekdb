# MiniVectorDB — 2026 competition round 2

单进程C++17数据库，通过stdin/stdout JSONL执行SQL子集，支持事务、WAL恢复和向量检索。未使用SQLite、RocksDB、Faiss、VSAG或HNSWlib实现数据库/索引操作。

本轮小算力诊断与候选验证共44对已完成。单CPU启动策略候选改善10k/30k查询，但双CPU默认查询、宽输出及单CPU一写一查出现退化，因此已撤回。当前28项运行构建输入恢复到已验证的 `c3d421ed0c1f92deaef65cf2a8d28aaa9d477042`，保留原四宏与PGO；没有新的净收益或正式涨分证据，暂不建议因本轮实验再次正式提测。全部正负数据与回退证明见[最新记录](docs/RESUME_20261113.md)。

## 最新正式结果

用户于2026-11-13 18:31提交 `e020fdeb67dd8b64b77f0f8c2905f137f0112bba`。已登录核实：MySQL准入通过，性能原始指标 **E=2937.367246**，较上一ee299adb的3014.419039下降约2.56%；Prompt仍无有效分数，只有合并错误提示。T/B/M与具体失败分支未显示。见[本次正式观察](validation/iteration4-formal-score-observation.json)。这不证明全部下降由代码造成，也不支持承诺6000分。

[技术报告](docs/TECHNICAL_REPORT_20261112.md)记录上一ee299adb结果、整体架构与当时的清理复验；[旧正式结果记录](validation/formal-ee299adb-20261112.json)保留脱敏字段。历史成绩不能归给后续候选。

本轮从用户指定的 `a90b2b750e40f020793cddefe01d5b66631afc63` 开始，筛选用户报告中的驻留预算、少量 UPDATE、磁盘索引批写与配对测量方向。按用户本轮要求新建 [agent/conversation.json](agent/conversation.json) 及相同根副本：报告与用户原话标为 `user`，实现与实验标为 `assistant`，不导入旧对话历史。`python3 minivec/tools/validate_iteration_material.py` 检查两份字节、大小与来源引用，不代表官方材料评审通过。

[第一轮优化实测报告](docs/ITERATION_20261112.md)记录 255 对实验、收益和回退决定：35k/50k 及少量 UPDATE 有明显收益，默认 10k 查询仍基本持平；LTO 和追加微调均未保留。**6000分尚未验证；本轮正式结果见页首。**

[第二轮记录](docs/ITERATION2_20261112.md)保存630对追加实验及当时的回退决定；其105对与旧二进制属于历史版本，不代表本轮构建。

本轮逐项处理用户报告中的20个方向，当前实施、关闭理由、来源审计及未解决问题见[20项状态与提测准备](docs/REPORT_20_DIRECTIONS.md)。I06增删改delta通过专项测试但因后续查询代价保持关闭；I07、受限I14与按用户反馈调整的I17门控已通过实际根镜像120对验证并有条件保留；普通查询接近持平，30k/60k和16维负面边界详见该报告。正式提测只由用户操作。随后12对单CPU诊断正确性通过：强制单线程改善30k查询，但spill等保护场景退步，未采用。下一轮12对保留启动校准、只禁止查询分拆，以定位代价；现有默认四宏保留，详细正负结果与限制见[恢复记录](docs/RESUME_20261113.md)。

## 构建和运行

```sh
docker build --platform linux/amd64 -t minivec .
docker run --rm -i --network none --cpus 2 --memory 2g --pids-limit 128 \
  --read-only --cap-drop ALL -v minivec-data:/data minivec
```

根Dockerfile是唯一镜像入口：固定GCC13.3.0镜像digest，静态链接，scratch运行层；默认 `/minivec --data /data`，不监听网络端口。启动完成恢复后输出 `{"type":"ready"}`，每个请求和响应各占一行并flush，诊断仅写stderr。示例请求：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE items(id BIGINT PRIMARY KEY, v VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO items VALUES (1,'[1,2]');"}
{"id":3,"op":"execute","sql":"SELECT id FROM items ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
```

请求须逐条等待响应。官方契约见[STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)。公开驱动限制启动120秒、请求60秒、用例600秒、最大响应16MiB、CPU2、内存2GiB。

## 当前实现

- SQL：建表、单列整数主键、BIGINT/INT/INTEGER、VARCHAR、VECTOR、多行INSERT、SELECT/UPDATE/DELETE、AND过滤、单键ORDER BY、LIMIT、向量距离、CREATE VECTOR INDEX、BEGIN/COMMIT/ROLLBACK。不是通用MySQL实现。
- 事务：语句失败回退本语句；已确认COMMIT先同步WAL；checksummed快照与WAL恢复。派生索引/临时payload可重建。
- 检索：默认exact与ann均使用精确Flat候选扫描，int8和SIMD只做经保守界证明的拒绝；精确距离与主键规则决定结果。默认关闭的部分簇ANN尚有实验缺口，不推荐开启冲分。
- 资源：resident向量借用行载荷；按预算spill、外部归并、可选双线程计算。元数据、巨大SQL和完整JSON响应仍增长，预算不等于整个进程内存恒定。

## 验证

完整门禁（Linux，需GCC、Python3、Docker；输出目录必须未存在）：

```sh
bash minivec/tools/ci_score.sh
```

单独运行：

```sh
make -C minivec -j2 test
python3 protocol_check/run.py --image minivec --output protocol-results/public
python3 minivec/tests/image_interactive.py --image minivec --output protocol-results/interactive
```

CI会构建根镜像，测试源码和镜像的公开用例、保持stdin打开的交互、退出/恢复、COMMIT后SIGKILL、精确TopK、线程池/跨表spill/WAL故障及sanitizer。公开用例与本地回归不计算正式E，也不包含隐藏用例。

当前受测运行源码 `311b45ffed1a957f8ebd71d0ead9320b1232298c` 已通过[实际根镜像门禁与120对验证](https://github.com/iamSttt/seekdb/actions/runs/43350596233)，准确镜像/程序SHA与负面样本见[当前摘要](validation/iteration3-retained-final.json)。最后的文档和材料提交不改变受测构建输入；不能将其称为又跑了一次120对。工作流支持手动dispatch及优化分支push验证。

第一轮历史门禁：运行核心 `4d8de37` 通过[当时验证](https://github.com/iamSttt/seekdb/actions/runs/43226783666)，公开用例各20/20；回退后交付树 `20eac01` 通过[当时最终门禁](https://github.com/iamSttt/seekdb/actions/runs/43232155090)。这些旧二进制结论不代表当前四宏版本。

旧SQL文本适配器仅供引擎回归：`--protocol sql`或`MVDB_PROTOCOL=sql`；`minivec/run.sh`与harness显式选该模式，不能替代JSONL准入。

## 后续测量和仓库结构

`minivec/tools/benchmark_jsonl.py`与`profile_jsonl.py`用于同环境成对测量，可用`--help`查看参数；cgroup采样由`profile_container.py`提供。测量代理和样本不能换算正式E或榜单名次。

| 目录 | 内容 |
| --- | --- |
| minivec/src、third_party | 必要运行源文件与JSON依赖/许可证 |
| minivec/tests、tools | 有效回归与JSONL测量工具 |
| mysqltest、protocol_check | 原样保留的官方公开fixtures与驱动，已去除pyc缓存 |
| docs | 本轮技术报告 |
| validation | 最新正式结果、本轮复验与清理记录 |

旧日志和实验可从[清理前固定提交](https://github.com/iamSttt/seekdb/tree/ee299adb296a2922d01982aa6c5707d37e767b41)找回。清单见[cleanup-manifest-20261112.json](validation/cleanup-manifest-20261112.json)。本次清理不宣称新的性能提升；正式评测由仓库所有者提交。
