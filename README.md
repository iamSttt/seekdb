# MiniVectorDB — 2026 competition round 2

单进程C++17数据库，通过stdin/stdout JSONL执行SQL子集，支持事务、WAL恢复和向量检索。未使用SQLite、RocksDB、Faiss、VSAG或HNSWlib实现数据库/索引操作。

## 当前分支状态与正式结果

本分支是从最高分版本重新验证的研究候选，**尚未通过完整性能采用条件，不作为已验证的正式提分版推荐**。运行组合760f已通过真实根镜像、公开20用例和数值/事务/协议回归；同机EE默认查询初筛通过，但相对保留专项版本的宽表、落盘和部分导入指标仍有退步。完整结果及所有未通过实验见[2026-11-14报告](docs/ROUND2_RESULT_20261114.md)。

新增全局分配失败注入还在760f及两个宽表实验中复现基础JSON库noexcept析构的终止路径，发生在宽表新状态创建之前；该资源异常限制尚未修复，不能将上述普通回归通过表述为所有OOM均可恢复。

2026-11-14重新核对六次正式结果，最高仍为2026-11-12 19:52（北京时间）提交 `ee299adb296a2922d01982aa6c5707d37e767b41` 的 **E=3014.419039**。后两次e020fde/7ca3baa为2937.367246/2567.716075；六次均通过MySQL，Agent Prompt均未参与排名。本轮没有提交正式评测，当前分支没有新正式成绩。

[技术报告](docs/TECHNICAL_REPORT_20261112.md)记录本次结果、整体架构、正确性边界、性能瓶颈与清理复验；[正式结果记录](validation/formal-ee299adb-20261112.json)保存脱敏字段。该成绩属于已评测的ee299adb，不能视为清理后提交的新正式成绩。两份conversation.json及专属材料工具已按仓库所有者要求移除，当前版本不包含Agent Prompt对话材料。

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

历史清理候选通过[清理CI](https://github.com/iamSttt/seekdb/actions/runs/43219423574)，其程序字节与EE相同，见[清理复验](validation/cleanup-ci-20261112.json)；这不是当前已修改运行代码的程序身份。当前组合的[实际根镜像与配对验证](https://github.com/iamSttt/seekdb/actions/runs/43548698762)完成正确性与EE初筛，但R专项保护未通过；数值结论以原报告为准，不能只看工作流颜色。工作流支持手动dispatch，提交分支push不会自动触发。

旧SQL文本适配器仅供引擎回归：`--protocol sql`或`MVDB_PROTOCOL=sql`；`minivec/run.sh`与harness显式选该模式，不能替代JSONL准入。

## 后续测量和仓库结构

`minivec/tools/benchmark_jsonl.py`与`profile_jsonl.py`用于同环境成对测量，可用`--help`查看参数；cgroup采样由`profile_container.py`提供。测量代理和样本不能换算正式E或榜单名次。

| 目录 | 内容 |
| --- | --- |
| minivec/src、third_party | 必要运行源文件与JSON依赖/许可证 |
| minivec/tests、tools | 回归、JSONL测量工具及明确标记的未采用历史原型 |
| mysqltest、protocol_check | 原样保留的官方公开fixtures与驱动，已去除pyc缓存 |
| docs | 本轮技术报告与实验取舍 |
| validation | 六次正式结果、配对原始证据与历史清理记录 |

旧清理记录见[cleanup-manifest-20261112.json](validation/cleanup-manifest-20261112.json)，清理前代码可从[固定提交](https://github.com/iamSttt/seekdb/tree/ee299adb296a2922d01982aa6c5707d37e767b41)找回。本轮原始结果及拒绝采用的理由保留在当前validation/docs中；正式评测由仓库所有者提交。
