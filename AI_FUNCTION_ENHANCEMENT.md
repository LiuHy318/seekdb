## 八、评测环境里的一次 4012 超时（记录）

评测日志（UTC 13:59，对应本分支 commit `ca1633b`）：

```
mysqltest: At line 24: query 'call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_complete')' failed with wrong errno 4012
Timeout, query has reached the maximum query timeout: 10000000(us)
```

- 该语句是官方用例的前置清理语句（`--error 0,11118`），在全新租户上应很快返回 11118
  （`OB_AI_FUNC_MODEL_NOT_FOUND`），与赛题 6 个子题的功能断言无关。
- 4012 就是 `OB_TIMEOUT`，报文案里的 `10000000(us)` 即默认 `ob_query_timeout`：语句等满了 10s 上限。
  该 DDL 走 `serialize_root_service_call`（进程级 Root Service 串行锁）+ schema 刷新，
  属 `src/pl/sys_package`、`src/rootserver`、`src/share/schema` 一侧的代码。
- 本分支的 12 个改动文件没有一个落在该链路：
  `git diff --name-only origin/master origin/ai-func-enhancement | grep -E "dbms_ai|rootserver|root_service|schema_service"` 为空；
  新增代码里也没有全局锁、静态单例或后台线程（全部是表达式内的局部逻辑）。
  因此这条超时不是 AI 函数实现引入的。
- 处理建议：重跑同一语句若仍返回 4012，需要评测侧放宽 `ob_query_timeout`（例如 60s）
  或在实例预热后再执行；本分支后续 4 个对齐提交（NULL 语义、`AI_EMBED` 改 LONGTEXT 等）
  在那次超时评测里尚未生效。

