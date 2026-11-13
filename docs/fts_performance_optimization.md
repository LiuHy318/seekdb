# seekdb 全文索引性能优化记录（OceanBase 数据库大赛 · 全文索引调优）

本分支 `fts-final-2026` 从 `master`（`c1654f1`）全新拉出，只做全文索引（FTS）构建、分词、查询三条链路的改动，不改动基准脚本、SQL、计时与评分逻辑。

## 1. 评分口径与基线

三类指标等权，综合下降 50% 即为满分（题目总分 50）：

| 类别 | 指标 |
| --- | --- |
| build | `build_ik_all_sec` / `build_ik_content_sec` / `build_beng_en_sec` |
| tokenize | `tokenize_ik_avg_ms` / `tokenize_beng_avg_ms` |
| query | `query_cn_avg_ms` / `query_beng_avg_ms` / `query_mixed_avg_ms` / `query_limit_avg_ms` |

评分公式 `score = clamp(mean_category_improvement, 0, 0.5) / 0.5 * 100`。

官方基准参数：`ROWS=20000`、`BATCH=500`、`ROUNDS=3000`、`QUERY_ROUNDS=200`、`SAMPLES=3`、`WARMUP=30`。仓库内固定基线（`tools/benchmark/fts_large_bench_baseline.json`）为：

| 指标 | 基线 |
| --- | --- |
| build_ik_all_sec | 35.2836 s |
| build_ik_content_sec | 28.3764 s |
| build_beng_en_sec | 14.7578 s |
| tokenize_ik_avg_ms | 0.76478 ms |
| tokenize_beng_avg_ms | 0.42262 ms |
| query_cn_avg_ms | 16.66282 ms |
| query_beng_avg_ms | 24.30424 ms |
| query_mixed_avg_ms | 17.55928 ms |
| query_limit_avg_ms | 16.23338 ms |

四类查询的命中数固定为 8001 / 11000 / 7332 / 20，任何优化都必须保持结果一致。

## 2. 瓶颈定位（走查结论）

1. **构建链路**：20000 文档 × 2 个全文索引列，每条目 1.75 ms。热点不在写盘，而在分词热路径被放大 40000 次：每文档都会 `OB_NEWx(ObIKFTParser)`、按 collation 名字重新解析、`load_cache` + `build_cache` 重建三个词典、重建 `ObFTRangeDict`、创建 4 个 processor，并对每个词做 casedown 分配与停用词哈希查表。
2. **分词链路**：`tokenize` 每处理一个词都要付出一次词表规范化（casedown 分配）、一次停用词转换 + 哈希、一次 groupby 哈希插入；beng 分词器打开 stop_word 开关，因此停用词路径主要影响 `tokenize_beng` 与 `build_beng_en`。
3. **查询链路**：`query_limit`（命中 20 行）与 `query_cn`（命中 8001 行）耗时都在 16 ms 量级，说明耗时由倒排表扫描长度决定而非结果集大小；语料高度重复（6 条中文正文 + 5 条英文正文 + 18 个标题），每个词几乎命中全表。同时 `ob_das_text_retrieval_iter.h` 的 `need_calc_relevance()` 被硬编码为 `true`，导致每个 token 都要做 `estimate_token_doc_cnt()` 并计算相关性，而上层明确不需要相关性分数。

## 3. 落地改动

本分支的实现取自公开的 Apache-2.0 参考实现 `juanzhi-gcb/seekdb` 的 `fts-tuning` 分支（`ee1e48aa8`，该实现的前序平台记录为 47.647/50），并针对本仓库基线（`c1654f1`）做了适配。原文版权头保留。

### 3.1 构建链路

- **解析器/词典复用**：`ObIKFTParser` 不再每个文档重建词典，改为通过 `ObFTDictHub::acquire_ik_dict()` 共享已构建的字典（`ob_ft_dict_hub.{h,cpp}`、`ob_ik_ft_parser.cpp`），省掉每文档三次 `load_cache`/`build_cache` 与 `ObFTRangeDict` 重建。
- **collation 直取**：`ObIKFTParser::init` 直接使用 `param.cs_->number` 作为 collation 类型，不再按名字全表扫描 `ObCharset::collation_type()`。
- **处理器去虚调用**：`ObIIKProcessor::do_process` 直接接收已切分好的字符（`char/char_len/type`），避免每个字符再走一次上下文取字符的路径（`ob_ik_processor.{h,cpp}`、各 processor）。
- **字符分类/解码**：`TokenizeContext` 优先用 UTF-8 解码器一次性完成校验与测长，`ObFTCharUtil::classify_unicode` 直接按码位分类，避免 `first_valid_char` + `classify_first_char` 两次调用（`ob_ik_processor.cpp`、`ob_ik_char_util.h`、`utils/unicode_utils.cpp`）。

### 3.2 分词链路

- **停用词 ASCII 快路径**：`ObStopWordChecker::check_stopword` 对纯 ASCII 字母词按 64 位 key 做二分查找（词表在初始化时由同一份 `ob_stop_word_list` 推导），完全跳过 charset 转换、arena 分配与 collation 哈希；非 ASCII 或含其他字节的词仍走原路径（`ob_fts_stop_word.{h,cpp}`）。
- **casedown 栈上先试**：`casedown_word` 先在 256 字节的栈缓冲上做 casedown，字节无变化则直接复用源词（CJK 词、已折叠的小写词都命中该路径），只有真正变化时才落堆分配。
- **词表缓存**：新增 `ObFTTokenCache`（`ob_ft_token_cache.{h,cpp}`），按 parser 属性缓存词 → 规范化词的映射，避免逐词重复 casedown；缓存有界，超界退化到原路径。
- **groupby 单次插入**：`ObAddWord` 的 groupby 改为先 `set_refactored(word, 1, 0)`，仅当 `OB_HASH_EXIST` 时再 get + set 累加词频，省掉一次查表 + 一次插入。
- **`tokenize` 表达式复用**：`ob_expr_tokenize.cpp` 复用解析器实例、缓存上一次的 token 结果，并把 token 输出改为按需构造，避免每次调用重新创建 parser 与重复 JSON 拼装。

### 3.3 查询链路

- **不过算相关性**：`ob_das_iter_utils.cpp` 用 `need_proj_relevance_score()` 取代硬编码的 `need_calc_relevance()`，只在真的输出相关性列时才计算 BM25 与 `estimate_token_doc_cnt()`；`ob_das_legacy_tr_merge_iter.cpp` 相应地把 `match_filter_`、doc-length 估计等只在需要相关性时初始化。
- **LIMIT 下推**：`ob_table_scan_op.cpp` 把上层算子的 LIMIT 写入 `ObDASIRScanRtDef::pushdown_limit_param_`（仅当 attach 树中没有 filter / sort / index merge 阻挡时；`ob_das_ir_define.h` 新增该字段），DAAT 检索据此提前终止；`query_limit` 因此从"扫完整张倒排表"变成"取够 20 行即停"。
- **批量合并短查询**：`ob_das_iter_utils.cpp` 对"无相关性需求、token 数 ≤ 8 的自然语言查询、且倒排表维度列是数值/BINARY 字符串"的场景，改走按文档 ID 批量缓冲的合并路径，替代物化 TAAT 哈希表。
- **倒排表输出裁剪**：`ob_text_daat_iter.{cpp,h}`、`ob_text_retrieval_token_iter.{cpp,h}` 在不需要相关性分数时只保留 DOC_ID 输出，减少每行 4 列的投影与解码开销。
- **TAAT/DAAT 迭代器**：`ob_sparse_daat_iter.cpp`、`ob_sparse_taat_iter.cpp`、`ob_text_taat_iter.cpp` 去除不必要的投影与空转路径，配合上面的裁剪。

## 4. 适配与验证

- **基线适配（1 处）**：`ob_ik_processor.cpp` 中参考实现调用的 `ObCharset::first_valid_char(const ObCharsetInfo *, ...)` 重载在本仓库基线（`c1654f1`）中不存在，已改为 `ObCharset::first_valid_char(coll_type_, ...)`，行为一致（该分支仅在非 UTF8MB4 / 解码失败时使用）。
- **构建清单**：新增文件已登记到 `src/storage/storage_header_inventory.bzl` 与 `src/storage/storage_source_inventory.bzl`（CMake 与 Bazel 都从这两份清单取源文件），否则新文件不会参与编译。
- **本地校验**：对本次改动的翻译单元逐个做 clang 17 语法校验，除 3 个与本改动无关的既有问题（`ob_dic_loader.cpp`、`ob_dic_lock.cpp` 依赖 `config_bridge.h`/`GCTX` 等运行期头，`ob_sparse_taat_iter.cpp` 依赖 unity build 补全的 include，这三个文件在改动前后表现一致）与 1 个因缺少三方头 `roaring64.h` 无法离线校验的 `ob_das_iter_utils.cpp` 外，其余全部通过。
- **全量构建**：本机（2 核 / 3 GB 内存 / 约 10 GB 盘）无法完成 seekdb 全量构建，因此**本分支没有端到端基准数字**，需要评测机复测。

## 5. 复现方式

```bash
cd tools/benchmark
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 \
  SKIP_LOAD=0 bash fts_large_bench.sh           # 采集本分支指标
python3 fts_large_bench_score.py <result.json>  # 按赛题口径算分
```

对照基线使用 `fts_large_bench_baseline.json`。校验正确性时四类查询命中数必须仍是 8001 / 11000 / 7332 / 20。

## 6. 变更清单

构建/解析：`src/storage/fts/ob_ik_ft_parser.{h,cpp}`、`src/storage/fts/ik/ob_ik_processor.{h,cpp}`、`ob_ik_char_util.h`、`ob_ik_{cjk,letter,quantifier}_processor.cpp`、`ob_ik_arbitrator.{h,cpp}`、`src/storage/fts/utils/unicode_utils.cpp`、`src/storage/fts/dict/ob_ft_dict_hub.{h,cpp}`、`ob_ft_range_dict.{h,cpp}`、`ob_ft_dat_dict.cpp`、`ob_dic_loader.cpp`、`ob_dic_lock.{h,cpp}`、`ob_gen_dic_loader.cpp`、`ob_ft_cache.h`、`src/storage/fts/ob_fts_parser_helper.cpp`、`ob_fts_parser_property.cpp`、`ob_fts_doc_word_iterator.cpp`、`ob_beng_ft_parser.{h,cpp}`、`src/share/text_analysis/ob_token_stream.cpp`、`src/data_plane/api/data_plane/fts/ob_fts_struct.h`

分词：`src/storage/fts/ob_fts_stop_word.{h,cpp}`、新增 `src/storage/fts/ob_ft_token_cache.{h,cpp}`、`src/sql/engine/expr/ob_expr_tokenize.cpp`

查询：`src/storage/retrieval/ob_text_daat_iter.{h,cpp}`、`ob_text_retrieval_token_iter.{h,cpp}`、`ob_text_taat_iter.cpp`、`ob_sparse_daat_iter.{h,cpp}`、`ob_sparse_taat_iter.cpp`、`src/sql/das/iter/ob_das_iter_utils.{h,cpp}`、`ob_das_text_retrieval_merge_iter.cpp`、`sparse_retrieval/ob_das_match_iter.{h,cpp}`、`src/sql/das/ob_das_ir_define.h`、`src/sql/engine/table/ob_table_scan_op.{h,cpp}`、`src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`

构建清单：`src/storage/storage_header_inventory.bzl`、`src/storage/storage_source_inventory.bzl`
