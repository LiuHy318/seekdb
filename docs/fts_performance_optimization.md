# seekdb 全文索引性能优化记录（OceanBase 数据库大赛 · 赛题一）

本文记录 seekdb 全文索引（FTS）构建 / 分词 / 查询三条链路的走查结论、已落地的内核改动、
以及已定位但尚未落地（需要基准数据支撑后再推进）的候选优化点，便于后续迭代与复核。

- 赛题页面：`https://open.oceanbase.com/train/detail/21?questionId=600080`
- 压测脚本：`tools/benchmark/`（与本赛区统一基准一致：`fts_large_bench.sh` 为主驱动，
  `fts_large_bench_score.py` 负责按三类指标打分，`fts_large_bench_baseline.json` 为基线）

## 1. 评分口径（与统一基准一致）

| 项目 | 说明 |
| --- | --- |
| 指标类别 | build / tokenize / query 三类等权 |
| build | `build_ik_all_sec`、`build_ik_content_sec`、`build_beng_en_sec` |
| tokenize | `tokenize_ik_avg_ms`、`tokenize_beng_avg_ms` |
| query | `query_cn_avg_ms`、`query_beng_avg_ms`、`query_mixed_avg_ms`、`query_limit_avg_ms` |
| 计分 | `score = clamp(mean_category_improvement, 0, 0.5) / 0.5 * 100`，耗时越低越好 |
| 基准配置 | rows=20000、batch=500、rounds=3000、query_rounds=200、samples=3、warmup=30 |

基线（`fts_large_bench_baseline.json`，五次 CI 运行均值）：

| 指标 | 基线值 |
| --- | --- |
| select1_avg_ms | 0.21032 |
| raw_load_sec | 1.6126 |
| build_ik_all_sec | 35.2836 |
| build_ik_content_sec | 28.3764 |
| build_beng_en_sec | 14.7578 |
| tokenize_ik_avg_ms | 0.76478 |
| tokenize_beng_avg_ms | 0.42262 |
| query_cn_avg_ms（hits 8001） | 16.66282 |
| query_beng_avg_ms（hits 11000） | 24.30424 |
| query_mixed_avg_ms（hits 7332） | 17.55928 |
| query_limit_avg_ms（hits 20） | 16.23338 |

## 2. 三条链路走查结论

### 2.1 构建链路（DML 写全文索引）

- 入口：`ObDASDomainUtils::generate_fulltext_word_rows`（`src/sql/das/ob_das_domain_utils.cpp`），
  由 `ObFTIndexRowCache::segment` 与 `ObFTDMLIterator::generate_ft_word_rows` 逐文档调用。
- 每个文档内的工作：新建一个文档级 `ObFTWordMap` → 逐 token 归一化后累加词频 → 按 map 词数
  一次性分配 `ObDatumRow` 数组 → 逐行填 `[WORD] [DOC_ID] [WORD_COUNT] [DOC_LENGTH]`。
- 关键观察：文档级「分词 + 词频聚合」是构建耗时的主要组成（20000 文档 × 2 个中文列），
  因此**分词热路径的每一笔开销在构建阶段会被放大 40000 次**。

### 2.2 分词链路（TOKENIZE 标量函数 / 索引内的分词）

- 入口：`ObFTParseHelper::segment`（`src/storage/fts/ob_fts_parser_helper.cpp`）→
  `ObIKFTParserDesc::segment` 构造 IK 解析器 → `ObFTParseHelper` 循环 `get_next_token` 后交给
  `ObAddWord::process_word`。
- 每个 token 的固定开销（`src/storage/fts/ob_fts_stop_word.cpp`）：
  1. `casedown_word`：无条件 `ObCharset::tolower`，即使全小写英文 / 纯中文也会走一次
     `ob_write_string` 拷贝 + Unicode casefold 遍历；
  2. `check_stopword`（beng 分词器开启该开关）：每次调用都在栈上构造一个
     `ObArenaAllocator`，并**无条件**做一次 `ObCharset::charset_convert`（源列排序规则与
     停用词表排序规则相同时这次转换纯属空转，仍会分配 + 拷贝一次字符串），随后才做哈希查询；
  3. `groupby_word`：先 `get_refactored` 再 `set_refactored(overwrite=1)`，新词也要走两次哈希。
- IK 词典匹配（`src/storage/fts/dict/ob_ft_range_dict.cpp`）：`find_first_char_range` 对每个字符
  线性扫描 range 数组，且每次比较都调用一次 `ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, ...)`
  的形式化比较函数。该函数位于「每字符 × 每 token」的双重热路径上。

### 2.3 查询链路（MATCH）

- `ObDASTRMergeIter` / `ObTextRetrievalTokenIter` 按 token 展开倒排表扫描，DAAT 归并出文档号。
- 值得注意的两个既有标记：
  - `src/sql/das/iter/ob_das_text_retrieval_iter.h:109`
    `inline bool need_calc_relevance() { return true; } // TODO: reduce tsc ops if no need to calc relevance`
  - 存储层 `ObTextRetrievalTokenIter::get_next_batch` 在首批数据前会调用
    `estimate_token_doc_cnt()`（`access_service->estimate_row_count`），而该值只服务 BM25 相关性。
  - 基准查询均无相关性排序需求，因此上述开销属于「为不需要的 BM25 付出的代价」，
    是后续 query 侧优化的首选方向（改动跨 SQL/DAS/存储三层，风险相对较高，故本轮未落地）。
- `query_limit_avg_ms`（hits 仅 20）与 `query_cn_avg_ms`（hits 8001）几乎相同，
  说明查询耗时主要由**倒排表扫描长度**（语料高度重复，每个查询词几乎命中全表）决定，
  而不是结果集大小，属于固定开销主导。

## 3. 已落地的优化

改动只做「去掉确定无用的工作」，不改变任何索引内容与匹配结果，可逐条论证等价性。

### O1 停用词检查：长度快速否决 + 同排序规则免转换 + 复用转换缓冲区

- 文件：`src/storage/fts/ob_fts_stop_word.{h,cpp}`
- 原实现问题：每个 token 都新建栈上 arena 并做一次 `charset_convert`。
- 改动：
  1. `ObStopWordChecker::init()` 计算停用词表的最大字节长度 `max_word_len_`；`check_stopword`
     对超长词直接返回「非停用词」，跳过转换与哈希查询（停用词表必不含更长词，等价）；
  2. 当 `word.get_collation_type() == stopword_type_.get_collation_type()` 时直接使用原字符串，
     不再调用 `charset_convert`（同排序规则下转换是恒等变换，等价）；
  3. 需要真正转换时使用 `thread_local` 复用 arena（`reuse()`），避免每词一次的 arena 构造/
     释放；线程本地对象天然并发安全。
- 影响：`tokenize_beng_avg_ms`、`build_beng_en_sec`，以及任何开启停用词开关的分词器。
- 正确性：三条改动都不改变「一个词是否是停用词」的判定结果。

### O2 casedown 的纯小写 ASCII 快路径

- 文件：`src/storage/fts/ob_fts_stop_word.cpp`（`ObAddWord::casedown_word`）
- 原实现问题：无条件 `ObCharset::tolower`，纯小写英文/数字也会分配并拷贝。
- 改动：新增 `is_lower_case_ascii_word`，当所有字节 < 0x80 且不含 `A`–`Z` 时，直接令
  `dst = src`（小写化对这类串是恒等变换）；其余情况（含中文、含大写、含非 ASCII）保持原逻辑。
- 影响：`tokenize_beng_avg_ms`、`build_beng_en_sec` 为主，混合语料同样受益于英文 token 部分。

### O3 词频聚合：新词路径只做一次哈希

- 文件：`src/storage/fts/ob_fts_stop_word.cpp`（`ObAddWord::groupby_word`）
- 原实现问题：先 `get_refactored` 再 `set_refactored`，首见词也要两次哈希查找。
- 改动：先尝试 `set_refactored(word, 1, 0)`（不覆盖）；返回 `OB_HASH_EXIST` 时才读取旧计数并
  覆盖写回 `count + word_freq`。首见词原先写入 1、重复词累加 `word_freq` 的语义保持不变。
- 影响：所有分词链路（构建、TOKENIZE、查询侧查询串分词）。

### O4 词典 range 定位：字节比较 + 二分

- 文件：`src/storage/fts/dict/ob_ft_range_dict.{h,cpp}`
- 原实现问题：`find_first_char_range` 线性扫描，且每次比较都走 `ObCharset::strcmp` 的形式化
  比较回调；该函数处在每字符一次的路径上。
- 改动：
  1. 新增 `binary_compare`：对 `CS_TYPE_UTF8MB4_BIN` 而言其排序等价于「按字节比较 + 长度兜底」，
     因此用 `memcmp` 直接实现，语义与原 `strcmp` 完全一致；
  2. `build_dict_from_cache` 填充 range 时顺带校验 start_ 是否单调递增，记录 `ranges_sorted_`；
  3. `find_first_char_range` 在有序时二分（找最后一个 start ≤ word 的 range，再校验 end），
     无序时退回原线性扫描，保证任何情况下行为不变。
- 影响：所有分词链路（每字符一次）。

### O5 删除 `ObExprTokenize::tokenize_fulltext` 中未使用的 arena

- 文件：`src/sql/engine/expr/ob_expr_tokenize.cpp`
- 原实现问题：声明了 `ObArenaAllocator tmp_parse_alloc` 但从未使用。
- 改动：删除该声明，`TOKENIZE(...)` 每次求值少构造一个对象。

## 4. 已定位、尚未落地的候选优化

以下方向均已定位到具体代码位置，但要么需要端到端基准支撑（本轮没有可用的构建机），
要么跨 SQL / DAS / 存储多层，故留作后续迭代：

1. **IK 解析器/词典实例复用**（`src/storage/fts/ob_ik_ft_parser.cpp`）：目前每文档都
   `OB_NEWx(ObIKFTParser)` 并 `init_dict()` 重新走 3 个词典的 `load_cache`、
   重建 `ObFTRangeDict` 与 4 个 processor。可做进程级（或线程级）词典缓存 + 解析器复用，
   直接命中赛题描述里的「加速器实例复用 + 内存池化数据结构」。
2. **文档级 word map 复用**（`ObDASDomainUtils::generate_fulltext_word_rows`）：每文档
   `ObFTWordMap::create()` 分配一次桶数组，可改为复用（`reuse()` 保留桶内存）。
3. **查询侧跳过无用 BM25 开销**：`ObDASTextRetrievalIter::need_calc_relevance()` 目前硬编码
   `true`，导致每查询 token 都会 `estimate_token_doc_cnt()`（memtable 场景还要拆分 range 重复估算）。
   改为读 `ir_ctdef_->need_calc_relevance()`（`ObDASIRScanCtDef` 里已有真实实现
   `nullptr != relevance_expr_`），预计能砍掉 query 的固定开销；风险在于相关性计算路径的连带改动。
4. **倒排扫描列裁剪**：无相关性需求时只输出 `DOC_ID`（现在按 4 列输出），可降低每 posting 行的
   解码成本——`query_*` 的耗时正比于倒排表行数。
5. **CJK 词的 casedown 空转**：中文 token 的小写化恒等但仍会拷贝，可在 UTF-8 解码后按「CJK/全角
   标点等无大小写映射区段」判定后跳过（需保证 Unicode 大小写判定准确，故本轮未做）。

## 5. 验证方式

### 5.1 已做的本地校验（编译前端）

本轮改动逐个做了 clang 17 + C++20 的语法级校验（`-fsyntax-only`，使用 OceanBase 官方
`obdevtools-llvm-17.0.6`）：

```
clang++ -std=gnu++20 -fsyntax-only -w -D_NO_EXCEPTION -DNDEBUG \
  -I. -I./src -I./src/oblib -I./src/oblib/common -I./src/data_plane/api \
  <third-party includes> src/storage/fts/ob_fts_stop_word.cpp
```

通过的文件：`src/storage/fts/ob_fts_stop_word.cpp`、`src/storage/fts/dict/ob_ft_range_dict.cpp`、
`src/sql/engine/expr/ob_expr_tokenize.cpp`。

### 5.2 官方基准（需要在可构建 seekdb 的机器上执行）

```bash
# 1) 构建并启动 seekdb（seekdb 单机模式，端口 2881）
./build.sh --init          # 或按 README 的方式构建
./seekdb --mode mini       # 启动方式以仓库 README 为准

# 2) 基线（改动前，checkout 到 master）
LABEL=before OUTPUT=bench_before.txt ./tools/benchmark/fts_large_bench.sh

# 3) 优化后（本分支）
LABEL=after OUTPUT=bench_after.txt ./tools/benchmark/fts_large_bench.sh

# 4) 打分（对比 before/after 两个结果文件）
python3 tools/benchmark/fts_large_bench_score.py \
  --baseline tools/benchmark/fts_large_bench_baseline.json \
  --result bench_after.txt
```

`SKIP_LOAD=1` 可在不动数据的情况下只复测分词与查询；`ROWS/BATCH/ROUNDS/QUERY_ROUNDS/SAMPLES`
必须固定，否则与基线不可比。

### 5.3 本轮未验证的事项（明确说明）

- 本轮开发环境为 2 核 / 3GB 内存 / 10GB 磁盘的容器，**无法完成 seekdb 全量编译**，因此
  没有产出 before/after 的端到端耗时数字，也没有跑过官方基准。
- 因此本分支的改动均限定在「确定的冗余工作量消除」，并在第 3 节逐条给出等价性论证；
  量化收益（尤其 query 类别）需在评测机上按 5.2 复测确认。
- 排序规则等价性依赖 `CS_TYPE_UTF8MB4_BIN` 的「按字节比较」语义；若后续引入非 BIN 排序规则的
  词典（当前内置词典均为 utf8mb4_bin），`binary_compare` 需要同步改为对应排序规则的比较。

## 6. 变更清单

| 文件 | 说明 |
| --- | --- |
| `src/storage/fts/ob_fts_stop_word.h` | 新增 `max_word_len_`、`is_lower_case_ascii_word` 声明 |
| `src/storage/fts/ob_fts_stop_word.cpp` | O1 停用词快路径、O2 casedown 快路径、O3 单次插入 |
| `src/storage/fts/dict/ob_ft_range_dict.h` | 新增 `binary_compare`、`ranges_sorted_` |
| `src/storage/fts/dict/ob_ft_range_dict.cpp` | O4 字节比较 + 二分查找（无序时回退线性） |
| `src/sql/engine/expr/ob_expr_tokenize.cpp` | O5 删除未使用 arena |
| `tools/benchmark/*` | 统一基准脚本（构建/灌数/分词/查询/打分/基线），便于复测 |
| `docs/fts_performance_optimization.md` | 本文档 |
