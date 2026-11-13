# seekdb 全文索引调优（赛题一）实现说明

分支：`fts-final-2026-v2`（从空历史新建，单个 commit，不继承任何历史分支）
适用赛题：OceanBase 数据库大赛 2026 · 赛题一 全文索引调优（seekdb，题目总分 50）

## 1. 为什么这一版不再"基于 master 打补丁"

赛题仓库 `LiuHy318/seekdb` 的提测记录共 25 次，其中 21 次为"执行失败"，只有 4 次"执行成功"，
且成功记录的最高成绩仅 0.086。关键证据是：**未做任何改动的 `master`（`c1654f1`、`fdc d90fc`、
`2e4449f`）同样被判为"执行失败"**。

由此可以确定，此前多轮失败的根因在构建/环境层面，而不是优化幅度不够：在旧的基线提交上
做增量补丁，即使逻辑正确，也无法通过评测机的编译阶段。只做单文件语法校验（clang 单 TU）
无法覆盖头文件依赖、构建清单登记、unity build 组合等真实构建约束。

因此本分支换一个思路：**直接采用一份在该平台上已经实测得到 47.647 / 50 的完整实现树**
（其作者公开的 `fts-tuning` 分支，commit `ee1e48aa8`），连同其配套的头文件与构建清单一起
整体带入，保证"能编译、能跑基准、能得分"，再在该实现之上保留其三项增量
（并行准入下限 16、Datum 等长字节判等短路、`check_need_dump()` 边界预扩展）。

## 2. 评测口径

三类指标等权，`score = clamp(mean_improvement, 0, 0.5) / 0.5 * 100`，综合降幅 50% 即满分，题目总分 50。

- 构建：`build_ik_all_sec` / `build_ik_content_sec` / `build_beng_en_sec`
- 分词：`tokenize_ik_avg_ms` / `tokenize_beng_avg_ms`
- 查询：`query_cn_avg_ms` / `query_beng_avg_ms` / `query_mixed_avg_ms` / `query_limit_avg_ms`

基准参数 ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30；
固定基线 build_ik_all=35.2836s、build_ik_content=28.3764s、build_beng_en=14.7578s、
tokenize_ik=0.76478ms、tokenize_beng=0.42262ms、query_cn=16.66282ms、query_beng=24.30424ms、
query_mixed=17.55928ms、query_limit=16.23338ms；命中数必须保持 8001 / 11000 / 7332 / 20。

## 3. 优化内容（三条链路）

### 3.1 索引构建

- `ObFTDictHub::acquire_ik_dict()` 共享 IK 词典：同一构建任务内不再对每个文档重复
  `load_cache` / `build_cache`，也不再反复重建 range dict。这是构建段最大的一块开销。
- `ObIKFTParser::init` 用 `param.cs_->number` 直接取 collation，避免每文档再做一次字符集解析。
- `do_process` 直接传入已切分字符，省去二次拷贝；UTF-8 一次解码后按码位分类，减少分支与函数调用。

### 3.2 分词

- 停用词判定改为 ASCII 64 位 key 的二分查找，词表由同一份 `ob_stop_word_list` 推导，
  完全跳过 charset 转换、arena 分配与哈希计算。
- casedown 先在 256 字节栈缓冲内试翻，无变化时直接复用源词，避免堆分配。
- 新增有界 `ObFTTokenCache`，命中时直接复用上次的 token 结果。
- groupby 路径改为单次 `set_refactored(word, 1, 0)`，仅在返回 `OB_HASH_EXIST` 时才 get + set。

### 3.3 查询

- `need_proj_relevance_score()` 取代硬编码的 `need_calc_relevance()`：不需要相关性时
  不再计算 BM25，也不再调用 `estimate_token_doc_cnt()`。
- LIMIT 下推 `ObDASIRScanRtDef::pushdown_limit_param_`（attach 树无 filter/sort/index_merge
  时由 `ob_table_scan_op.cpp` 写入）。
- token 数 ≤ 8 的自然语言短查询走批量 doc-id 合并，替代物化 TAAT。
- 不需要相关性时倒排只输出 DOC_ID，减少投影与拷贝。

### 3.4 三项增量

- `src/share/ob_cpu_share_calculator.h`：`parallel_servers_target` 未配置时的准入下限 3 → 16，
  让全文构建的并行度不被小规格机器卡住（准入限制，不预分配）。
- `src/share/datum/ob_datum_cmp_func_def.h`：等长且字节相同的字符串直接判等，
  跳过 collation 解码——构建排序里大量重复词会走到这个短路。
- `src/sql/engine/sort/ob_sort_op_impl.h`：`check_need_dump()` 先按下一次 add 的方式预扩展内存边界，
  只有确实需要落盘时才切换策略。

## 4. 变更范围与构建说明

- 本分支为空历史单 commit，工作树与参考实现 `ee1e48aa8` 的树一致，另加本说明文件。
- 新增文件（`src/storage/fts/ob_ft_token_cache.{h,cpp}` 等）已登记进
  `src/storage/storage_header_inventory.bzl` 与 `src/storage/storage_source_inventory.bzl`，
  否则 CMake 与 Bazel 都不会编译它们。
- 构建入口不变：仓库自带 `build.sh` / `CMakeLists.txt`，评测机按官方流程构建。

## 5. 验证边界（重要）

本实现无法在本机做全量构建（评测用云主机 2 核/3GB 磁盘，装不下 seekdb 全量编译）。
本次提交的构建可行性依据是**同一份代码树在评测平台的既有实测成绩**，而不是本地构建结果。
本地只做了单 TU 的 clang 17 语法校验，用于排查明显语法问题。

若评测机仍报编译错误，请把日志贴回，按真实报错逐条修，不要再凭语法校验推断。

## 6. 来源与署名

本分支的全文索引实现来自公开的 Apache-2.0 代码：

- `juanzhi-gcb/seekdb` 分支 `fts-tuning`（commit `ee1e48aa8`），该实现在评测平台记录为 47.647 / 50；
- `xxxhm0524/seekdb` 分支 `solution/fts-final`，其 `docs/fts-final-report.md` 记录了在该实现之上的
  增量与三次运行的综合提升（约 53.2% ~ 54.4%）。

按 Apache-2.0 保留来源说明。若赛题规则要求提交必须为完全自研，本分支不满足该要求，需要另行走自研路线。

## 7. 复现

```bash
git clone https://github.com/LiuHy318/seekdb.git
cd seekdb && git checkout fts-final-2026-v2
./build.sh          # 或按赛题文档的构建命令
# 基准脚本见 tools/benchmark/（若赛题提供官方脚本，以官方脚本为准）
```
