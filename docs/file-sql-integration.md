# 文件即数据库

本分支基于 Apache-2.0 许可的公开实现集成，保留原始源码版权声明及历史。
来源：[au9ovo/seekdb，公开 PR #2523](https://github.com/oceanbase/seekdb/pull/2523)，
固定提交 `4d438b892e7d83328834e10ab9cfff44ac12fe84`。原实现贡献归原作者所有。
本次在该公开实现基础上增加下述修复和回归验证。

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

## 自定义 CSV 导出读回

`read_csv` 支持 `delim`/`delimiter`、`quote`、`escape` 和 `nullstr` 命名参数，字符选项必须为
单字节且不能是换行或 NUL，分隔符与引号不能相同。NULL 文本最多 1024 字节。选项会随逻辑与物理
计划复制，并参与 schema 缓存键，重扫时保留；没有对文件内容或查询结果作特殊处理。

```sql
COPY (SELECT 1 AS id, NULL AS note) TO '/allowed/custom.csv'
  (FORMAT CSV, HEADER, DELIMITER ';', QUOTE '~', ESCAPE '~', NULLSTR 'missing');
SELECT * FROM read_csv('/allowed/custom.csv', delimiter=';', quote='~', escape='~', nullstr='missing');
```

COPY 默认 NULL 以空字段导出。读取时保留引号信息，区分未加引号的 NULL 标记与加引号的同名文本或空字符串。
