这是 I15 PGO 的独立实验工具。方向来自用户报告，具体实现由 assistant 编写。同源隔离实验已在 `502931da5b1e9804212e25efe029f4b9f253bc96` 完成；工具专用 Dockerfile 仍默认 `ENABLE_PGO=0`。根Dockerfile默认启用原PGO；[第二轮记录](../../../docs/ITERATION2_20261112.md)描述当时恢复旧源码的历史选择，当前四宏组合及实际镜像120对见[20项状态](../../../docs/REPORT_20_DIRECTIONS.md)。第三轮六构建矩阵仍保留原PGO、不加入LTO或spill扩展训练；历史隔离数据不能替代当前构建证据。

依赖仅为 Linux、Docker、Git、Bash、Python 3 和常见 coreutils/findutils。编译与训练在现有固定镜像 `gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818` 中完成，C++ helper 只依赖已有 `json.hpp`。无需下载安装包，Docker RUN 网络禁用。Python 仅用于构建外的已有协议验证与基准。

运行示例（Linux，结果目录必须尚不存在）：

```bash
bash /absolute/repo/minivec/tools/pgo/compare.sh /absolute/repo 502931da5b1e9804212e25efe029f4b9f253bc96 /absolute/pgo-results --build-only
# 去掉 --build-only 后还会执行 15 对标准测试与 90 对边界测试。
```

下文的 `[pgo]`/`ci_pgo.sh` 是历史同源隔离入口，须在 `502931d` 的 checkout 中使用。当前根 Dockerfile 已启用 PGO，不满足该旧入口的基线预检，因此当前 HEAD 应使用 `[final-perf]` 或文末的 `MVDB_FINAL_PERFORMANCE=1` 完整验证入口。

`--build-only` 仍会运行两个二进制的 JSONL、精确 TopK（默认/双线程）和官方公开协议检查，只跳过性能测量。脚本按给定 commit 的 Git archive 创建新上下文；使用该提交的源码、测试和基准脚本，不读取未提交工作区。原仓库保持不变。临时上下文、镜像和结果保留，便于诊断；临时容器及其匿名卷用 `docker rm -v` 释放。

仓库 CI 入口为 `bash minivec/tools/ci_pgo.sh`。包含 `[pgo]` 的提交触发独立 50 分钟 job，并排除普通 paired-performance；原 validate job 仍运行。先完成编译、六次训练响应验证、反馈哈希检查、发布镜像检查和八项二进制正确性门禁，再启动 15 对标准与 90 对边界测量。失败立即停止。EXIT trap 始终尝试输出 `MINIVEC_PGO_REPORT_BEGIN ...` / `MINIVEC_PGO_REPORT_END`，其中含 context、hashes、run-status、proof、correctness 和已有配对 JSON，便于从日志下载；输出目录 `pgo-results/` 已忽略。

构建顺序：

1. 对照版使用与当前根 Dockerfile 相同的 GCC 13.3、`-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread`。
2. 实验版先在 `/src` 通过 `src/*.cpp -o /minivec` 加 `-fprofile-generate=/pgo -fprofile-update=atomic` 编译，正常运行训练并验证每条响应。
3. 保持工作目录、源码路径、输入顺序和输出名 `/minivec` 不变，以 `-fprofile-use=/pgo -Werror=missing-profile -Werror=coverage-mismatch` 重编。每个源码翻译单元都必须产生非空 `.gcda`；收集反馈哈希。禁止 `-fprofile-correction`、`-ffast-math`、LTO 和 native CPU 特化。
4. 最终 scratch 镜像只含重编后的 `/minivec` 与空 `/data`。训练 helper、语料、反馈和数据库不会进入发布镜像。两个版本均执行相同 ready/shutdown smoke。

GCC 以输出文件名参与生成 `.gcda` 名称，且 profile 路径可能包含编译路径，因此两个编译阶段使用同一路径。[GCC 13.3 Instrumentation Options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Instrumentation-Options.html) 也说明 atomic 能防止线程计数更新竞争，但未正常结束线程仍可能导致反馈损坏。训练期间所有查询任务已完成、数据库正常 shutdown；仍以反馈诊断和编译错误为准，不修正或忽略损坏反馈。

缺失反馈显式升级为错误；源代码不匹配本来即是错误，再明确要求 `coverage-mismatch` 为错误。PGO 的影响仅来自编译器针对训练反馈的优化，并非运行时识别数据或请求。[GCC 13.3 Optimize Options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Optimize-Options.html)

训练内容：

| 维度 | 行数 | 主查询数 | 建索引前查询 |
| --- | ---: | ---: | ---: |
| 16 | 1,024 | 100 | 10 |
| 128 | 10,000 | 500 | 30 |
| 512 | 1,024 | 100 | 10 |

输入使用独立种子 `2026111201` 和可移植 SplitMix64。每个坐标为十二个 12 位均匀整数相加、减中心值后除以 4096，接近 Gaussian 且多数为非整数，覆盖量化残差。所有坐标为有限 binary32 dyadic 值，没有从现有测试、基准响应或数据库实现读取答案。

查询混合 K=1/10/100、无过滤、标量等值/范围、主键范围和空结果。少量重复向量与查询命中重复向量用于距离相同后主键排序。插入包含批量、列重排、UTF-8 和 SQL 转义。各表执行少量向量与标量 UPDATE、显式 ROLLBACK、COMMIT、重复主键与维度错误，并验证失败记录不存在。

三阶段正常启动/关闭覆盖重启恢复：第一阶段建表、插入、建索引、主查询与更新；第二阶段重启检查已提交值，打开事务修改一行并直接 shutdown；第三阶段重启验证 shutdown 回滚。相同固定输入分别在解析/查询线程为 1 和 2 的新数据库上执行，反馈合并到 `/pgo`。

helper 独立计算所有查询的完整排序，再生成期望响应。坐标为 n/4096 且 |n|<=24570，512 维平方距离分子的上界小于 2^41，double 可准确表示各差值、乘积和累加；距离相同按主键排序。响应逐行检查 ready、id、成功/错误、列与全部行、affected_rows、投影值以及 shutdown 后无多余输出。常规响应采用 JSON 语义相等，错误响应检查 id/ok/error 结构与非空代码，不依赖内部错误文案。

训练用有限 JSONL 文件重定向输入，因此不是交互延迟测试；对照脚本另以既有协议测试覆盖交互行为。确定性保证的是语料与动态计算的期望值，线程调度、启动校准和反馈计数不保证逐位可重现。两个线程配置用于覆盖已存在的执行路径，不将训练时延计入性能收益。

对照记录源码提交、源文件与草稿哈希、镜像与二进制哈希、CPU 信息、语料/反馈哈希、协议结果和性能 JSON。实际测量固定在同一两个可用 CPU 上，标准场景 3 个评估种子各 5 对；评估种子 `7319/8911/13337` 与训练独立。边界覆盖 resident30k、resident50k、low16、filter60k、update10、spill，每场景 15 对。结果只用于本机对照，不换算正式分数。

历史实验按上述门禁先检查构建与正确性，再开展配对。当前最终组合的验证入口为 `MVDB_FINAL_PERFORMANCE=1 bash minivec/tools/ci_score.sh`：使用根 Dockerfile 同一次构建的程序完成正确性与 105 对直接比较。反馈检查或任何协议/数值测试失败都会保留日志并停止，不修正损坏反馈或忽略失败。
