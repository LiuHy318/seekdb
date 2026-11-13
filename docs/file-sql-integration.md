# 文件即数据库

本分支基于 Apache-2.0 许可的公开实现集成，保留原始源码版权声明及历史。
来源：[au9ovo/seekdb，公开 PR #2523](https://github.com/oceanbase/seekdb/pull/2523)，
固定提交 `4d438b892e7d83328834e10ab9cfff44ac12fe84`。原实现贡献归原作者所有。
这不是独立原创的竞赛实现；提测前应自行确认赛事对复用公开实现的要求。

支持通过 seekdb 原生 SQL 引擎查询本地 CSV、JSONL、Parquet，及文件发现、动态列结构、聚合、
跨格式 JOIN 和 CSV 导出。没有使用其他查询引擎代执行。

```sql
SET SESSION enable_file_sql = ON;
SELECT * FROM read_csv('/allowed/data.csv') LIMIT 10;
SELECT * FROM read_ndjson('/allowed/events.jsonl');
SELECT count(*) FROM read_parquet('/allowed/data.parquet');
DESCRIBE SELECT * FROM read_csv('/allowed/data.csv');
SELECT file FROM glob('/allowed/*.csv');
COPY (SELECT * FROM read_csv('/allowed/data.csv'))
  TO '/allowed/export.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
```

功能默认关闭；启用后仍受 `secure_file_priv` 限制。CSV/JSONL 使用全量 schema 推断，
`sample_size` 只接受 `-1`。扫描保留文件指纹，发生替换或修改后报错。已有 COPY 目标不会被覆盖。

本次额外收紧 JSONL 的读取预算：推断和执行阶段均在读取过程中限制单行长度，避免先通过
`std::getline` 分配整条超长记录后才检查上限。CSV 同时限制整条记录及列数，避免大量短字段累积超出预算。增加独立的原生服务器集成回归测试。

验证工作流：`.github/workflows/file-sql-acceptance.yml`。它从当前提交编译 seekdb，启动真实服务，
运行官方 `file_sql_basic.test` 和新增的端到端断言；失败时保存日志，不能将未通过或未运行的步骤视为通过。
