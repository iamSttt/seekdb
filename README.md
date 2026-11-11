# MiniVectorDB — 2026 OceanBase 大赛加赛（轻量向量数据库）

从零实现的单机向量数据库，按赛题要求通过 **stdin/stdout** 提供 SQL 服务，数据落在 `/data`。

## 构建

```bash
docker build -t minivec .
```

多阶段构建：`build` 阶段用 `ubuntu:24.04 + build-essential` 编译，运行阶段只带二进制与 `libstdc++`，运行期无需联网、不监听端口。

## 运行

```bash
docker run -i --rm -v minivec-data:/data minivec
# 或本地
echo "SHOW TABLES;" | ./minivec --data /tmp/mvdb
```

前台运行，从 stdin 读取以 `;` 结尾的 SQL，向 stdout 输出结果，日志写 stderr。

## 支持的 SQL

- `CREATE TABLE [IF NOT EXISTS] t (id BIGINT PRIMARY KEY, name VARCHAR(n), emb VECTOR(d))`
- `INSERT INTO t [(cols)] VALUES (...), (...)`
- `SELECT <cols|*|L2_DISTANCE(emb,'[..]') AS d> FROM t [WHERE ... AND ...] [ORDER BY c|alias [ASC|DESC]] [LIMIT n]`
- `UPDATE t SET c=v [WHERE ...]`、`DELETE FROM t [WHERE ...]`
- `BEGIN` / `COMMIT` / `ROLLBACK`
- `CREATE VECTOR INDEX name ON t(emb)`、`SHOW TABLES`、`DROP TABLE [IF EXISTS]`
- 比较运算符 `= <> != < <= > >=`，仅 `AND`

不支持的能力（JOIN/聚合/子查询/NULL/OR 等）返回明确错误，不静默执行。

## 目录

```
minivec/
  src/      C++17 实现（parser / catalog / storage+WAL / executor / vector kernels）
  tests/    单元测试与 mysqltest 风格用例
  tools/    harness.py（本地对拍驱动）
Dockerfile  多阶段构建
agent/conversation.json  Agent 学习材料
```

## 设计要点

- **存储**：页式行存 + 追加式 WAL（每条记录 CRC32），`fdatasync` 后才返回提交成功；启动时快照 + 重放 WAL；遇到撕裂的尾记录即停止重放。
- **事务**：单写者，立即生效 + undo 链，`ROLLBACK` 逆序恢复；显式事务内读得到自己的修改。
- **向量**：`L2_DISTANCE` 返回欧氏距离；`ORDER BY 距离 LIMIT k` 为精确 TopK（`(dist, pk)` 排序）；IVF-Flat 索引用于近似模式。
- **协议**：默认 mysqltest 风格（表头 + 制表符分隔、无边框、无 `Query OK`），`MVDB_OUT=table` 切换为人读的表格形式。
