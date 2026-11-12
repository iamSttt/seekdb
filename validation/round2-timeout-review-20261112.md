# 2026-11-12：JSON 超时定位与运行版本验证

本记录保存本轮源码检查、修复与实际 CI 结果。正式评测由用户提交；公开协议用例与本地回归不等于正式隐藏用例，也不构成性能 E 或 Prompt 分数预测。

**失败入口与版本关系**

用户报告的提交为 `ff6a3bdbf6f10a1b17908229b170aa85579f5be9`。其 `minivec/src/main.cpp` 只有以分号结束的 SQL 控制台循环：启动时把 `minivec ready ...` 写到 stderr，然后阻塞读取 stdin。stdout 没有官方协议要求的 `{"type":"ready"}` JSON 行。官方 Session 在发送第一条请求前等待这条 ready，双方因此都在等待输入。旧 Docker 构建内的 smoke 使用 SQL 文件输入和文本输出，不能检查这个 JSONL 契约。

`cdfdb7cfe48910e4ef2506a5ffcccd143722587f` 已恢复 JSONL 入口和固定编译器的根 Dockerfile。本轮实际构建该提交，而非仅根据源码相同作推断；[CI 43129998304](https://github.com/iamSttt/seekdb/actions/runs/43129998304) 成功，镜像 `sha256:77f7056adb7c5bf627a04448ba8547c1c3220495dc45a68d80e2f85a11c2013b`，官方公开用例 20/20 通过。原始报告位于 `raw/verify-20261112-cdf/`。

验证期间提交分支前进到 `72bda06d7bedcf2e5f79cb838922f8ffdcd609a4`。本轮保留其性能实现和两份材料，重新验证新运行树。未用 CDF 的测试结果替代新版本测试。

**在新运行树上修复的问题**

- JSON 快速路径：只让 ASCII 简单字符串直接通过；控制字节及非 ASCII 字节交给严格 JSON 解析，避免非法 UTF-8 的请求仍执行 SQL。修正数值 id 边界累积和 INT64_MIN 的有符号取负溢出。
- 查询线程池：调用方或工作线程抛异常时，等待两半完成、清理任务引用，然后在调用方重抛；创建线程失败时保留两半的执行与异常语义。
- 查询/解析线程：创建前屏蔽 SIGTERM/SIGINT，由工作线程继承，恢复主线程掩码；增加启动握手和解析池部分构造失败时的停止、join 清理。
- 跨表落盘：完整创建 payload 文件后、释放原始行向量前，清除仍借用这些向量地址的索引。
- SIMD 派发：AVX512 路径的短维度分支依赖 AVX2，派发时补查该能力。
- WAL 错误：保留现有 1 MiB 缓冲和逻辑/物理位置设计；UPDATE/DELETE 改为检查 `append_wal()` 返回值。逻辑字节数递增不能证明 flush 成功。失败语句回退，事务此前成功语句继续保留。

这些后续问题与 `ff6a3bdb` 缺少 JSON ready 分开记录；它们不被冒充为该次正式评测的额外已知失败原因。

**修复前后对照**

[CI 43132750770](https://github.com/iamSttt/seekdb/actions/runs/43132750770) 固定构建 `385766a2251595867a546840fe8f41c5db6b1c9e`，镜像 `sha256:1c858ef7f44a222ff9a745292b323745bdf8da6be7844104a8fdd7848a3255f0`。它通过：

- 真实镜像握手和 20 个公开协议用例；源码二进制也通过同一组用例。
- stdin 保持开启时的正常请求、错误后继续请求、拆分 UTF-8 请求、shutdown 回复后退出；事务回退及 COMMIT 后 SIGKILL/重启。
- 镜像二进制的 7 组 JSON 协议测试，以及普通和强制双线程 TopK 精确 oracle。
- 查询池异常、创建失败、空闲信号测试及真实 64 MiB 全局预算的跨表 spill、回滚和重启测试；对应 ASan/UBSan 测试通过。ASan 关闭 leak detection 以适配有意存活到进程结束的线程池，仍检查非法内存访问。
- 完整源码回归和 UBSan 二进制 JSON 测试。

相同新测试作用于未修改的 `72bda06` 时，观察到非法 UTF-8 请求被执行并返回非法 UTF-8 响应（退出码 1）；查询池普通/创建失败分支均在两半完成断言失败（各 134）；跨表 spill 后索引未失效断言失败（134）。这些是具体错误复现，未把超时或任意非零返回当作复现证据。`raw/verify-20261112-parallel/` 保存 46 份提取报告、完整压缩日志和文件摘要。

**最终运行提交与 WAL 实测**

最终运行提交为 `e8534416dcef2af64847330ffedb12831e45885d`。相比 385，只增加两处 WAL 错误检查及默认回归入口、测试；WAL 缓冲大小和并行策略未替换。[CI 43134391008](https://github.com/iamSttt/seekdb/actions/runs/43134391008) 于北京时间 09:29 完成且成功，实际镜像为 `sha256:76d581ff6f4e21835a9f0778a030d33704d31e6e8afbb9001247b18baa0e5e82`。

该最终镜像重新通过官方握手和 20/20 公开用例、stdin 开启时的交互和恢复测试、镜像 JSON 七组测试及强制双线程 TopK；`make test` 完整通过。新增八个 WAL 专项在普通构建与 ASan/UBSan 下均通过，UBSan 整个程序的 JSON 回归也通过。这里的最终运行结果独立于先前 385 的结果。

故障注入允许文件多写 32,768 bytes，再触发短写。原 385 的严格测试在 UPDATE `accepted=1` 后断言失败（134）；观察模式显示 UPDATE、DELETE 均误报成功，恢复写入空间后 COMMIT 与重新打开仍能恢复一致数据。最终版 INSERT、UPDATE、DELETE 均 `accepted=0`，回退失败语句，并保留事务前一条仍在缓冲中的成功语句。这证明了漏报写入错误及修正，未据此声称已确认 COMMIT 的数据丢失。

新增覆盖还包括四种 COMMIT payload/header 部分写入与重试/close-reopen 组合、跨 flush 后重复主键回退。未直接注入 `fdatasync` 返回失败或内存分配失败。SIGKILL 恢复由另一组已有测试和镜像交互测试覆盖，不把 close/reopen 冒充 SIGKILL。

本次还原生编译了 `ff6a3bdb` 的确切源码：stdin 保持开启、进程仍活着、stdout 为零字节时，官方 Session 的等待函数报告 `JSON response timeout`。这项历史定位只把配置副本的启动等待缩为两秒，清楚标注为诊断；当前镜像的官方用例继续使用原始 120 秒启动、60 秒请求上限，未改官方限额文件。详见 `raw/verify-20261112-final/legacy-no-ready-diagnostic.json`。

最终轮保存 38 份报告、完整压缩日志、CI 提交和输入 SHA256，目录为 `raw/verify-20261112-final/`。发布前按 Linux 行尾核对 73 个构建/协议输入文件与该 CI 一致；附加报告提交不改变这些运行输入。

**性能对照的范围**

上述 CI 在同一 AMD EPYC 9V74 runner、官方 Session 的 2 CPU/2 GiB 容器限制下，交替运行 CDF、原 72、修复 385 三版本的三个 seed。每份 benchmark 是 10,000 行、128 维、exact/ann 各 1,000 次查询、K=10，包含过滤/无过滤、主键平局、10 个独立全扫描 oracle 抽样，以及重启后 10,000 行和 10 个向量核对。

385 相对原 72 的查询耗时基本持平，配对加速比中位 0.9927，范围 0.9830–1.0036；相对 CDF 中位 1.0456。CDF 到 385 的查询 CPU 用量增加约 21%，内存采样下降。写入有较大落盘尾延迟波动，保留全部慢样本，不宣称稳定 2.23 倍。完整分析见 `verify-20261112-performance-analysis.md` 和同名 JSON。

未启用部分簇 ANN；本轮 default ann 与 exact 一致。未对显式开启近似模式的召回做认证。容器资源是采样结果，未换算未知正式 E。

最终轮在另一个同机配对 job 中比较 385 与 e853，三个 seed 的查询配对比率中位为 0.994693（e853 耗时约增加 0.53%），范围 0.981155–0.998023。写入 benchmark 的 e853 耗时中位增加 11.22%，不能隐去该结果；独立 profile 的写入路径表现方向不一致，尚不能认定稳定代码回退。六份 benchmark 的已记录正确性检查通过，I/O 与内存采样近似持平。完整原始量见 `verify-final-20261112-performance-analysis.md` 及同名 JSON。最终版解决错误处理，不宣称这两处修复本身提速。

**材料保护**

`agent/conversation.json` 和根 `conversation.json` 均保持 `72bda06` 原字节：各 112,461 bytes，SHA256 均为 `b630b54444c28be61fdb3efc148909762d193e889a01eb7090e63f6ebaaf317b`。本轮记录独立保存，不改历史发言。该版本本地材料结构校验退出 0；这不是正式 Prompt 评分或来源真实性认证。

**复验命令（Linux + Docker）**

```bash
docker build --platform linux/amd64 -t minivec:verify .
python3 protocol_check/run.py --image minivec:verify --handshake-only
python3 protocol_check/run.py --image minivec:verify
make -C minivec -j2 test
./minivec/test_recovery --wal-staging
```

新的 JSON、双线程查询、线程池故障和跨表 spill 测试已纳入 `make test`，降低后续入口改动漏测的风险。正式协议、公开用例及资源限额文件未修改。
