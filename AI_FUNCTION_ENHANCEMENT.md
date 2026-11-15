# 赛题一：AI 函数增强 —— 实现说明

本文说明本分支对 seekdb AI 函数服务所做的增强，对应赛题一的 6 个子题。

- 分支：`ai-func-enhancement`（基于 `origin/master` 新建，未复用任何历史分支）
- 主要改动集中在 `src/sql/engine/expr/ob_expr_ai/` 与 `src/share/ai_service/`

## 一、AI_PROMPT 类型与模板增强

改动文件：`ob_expr_ai_prompt.cpp/.h`、`ob_ai_func_utils.cpp/.h`

1. **TEXT / LONGTEXT 支持**：`calc_result_typeN` 对 template 与参数同时接受
   `VARCHAR/CHAR/BINARY` 与 `TINYTEXT/TEXT/MEDIUMTEXT/LONGTEXT`；求值时统一走
   `ObTextStringHelper::get_string`，按 LOB 方式读取真实字符串，长文本不会被截断。
   BLOB 类型（tinyblob/mediumblob/longblob）仍然报 `OB_ERR_INVALID_TYPE_FOR_OP`。
2. **JSON 参数**：模板与参数类型判定中放开 JSON，求值时用
   `ObJsonExprHelper::get_json_doc` 取出 JSON 文档并深拷贝进 `args` 数组，
   Array / Object 参数保持原有结构，不再报 "json type is not supported"。
3. **嵌套 Prompt**：`ObAIFuncPromptObjectUtils::render_args_node` 在渲染占位符时，
   若参数本身是一个合法的 prompt 对象（`{template, args}`），则递归展开成内层
   渲染后的文本（层级上限 `max_prompt_nest_depth = 16`），否则按紧凑 JSON 输出。
   因此 `AI_PROMPT(t2, AI_PROMPT(t1, ...))` 可以正常拼接。
4. **重复占位符**：`replace_all_str_args_in_template` 先把每个参数渲染一次缓存起来，
   再按出现顺序替换，`{0}...{0}` 这类重复占位符每一处都会被替换。
5. **非法下标**：`{n}` 中 n 超出参数个数、或参数为 JSON null 时返回
   `OB_INVALID_ARGUMENT`（`ai_prompt: invalid placeholder index`）；`{}`、`{a}`
   这类不构成下标的写法按普通字符原样保留。
6. **Unicode 与转义**：渲染按字节拷贝，多字节字符不受影响；参数经 JSON 解析后
   转义序列已还原，输出再由 JSON 打印器统一转义。

## 二、AI_EMBED 单行语义

改动文件：`ob_expr_ai_embed.cpp`

- 输入支持 `VARCHAR/CHAR/BINARY` 与 `TEXT/LONGTEXT`（LOB 读取，不截断）。
- `dimension` 参数必须是正整数：`<= 0` 直接报 `OB_INVALID_ARGUMENT`；传 `NULL`
  视为未指定，使用模型默认维度。
- 返回值为 JSON 数组文本，例如 `[0.1, 0.2, 0.3]`。
- 返回结果校验（`ObAIFuncJsonUtils::validate_embedding_array`）：
  必须是 JSON 数组、维度非空、元素必须都是数字；显式指定 `dimension` 时
  返回维度必须与请求维度一致，否则报错。

## 三、AI_COMPLETE 请求与响应

改动文件：`ob_expr_ai_complete.cpp`、`ob_ai_func_utils.cpp`

- 普通 Prompt、`AI_PROMPT` 生成的 JSON prompt、`TEXT/LONGTEXT` 长文本 prompt 均支持；
  prompt 为 JSON 时不再要求 `args` 全部是字符串，允许 JSON 数组/对象与嵌套 prompt。
- `parameters`（JSON config）必须是 JSON 对象，数组或标量不再被强转为对象。
- 响应校验：`call_completion` 解析后若内容为空，返回 `OB_INVALID_DATA`。

## 四、Ollama Provider

改动文件：`ob_ai_func_utils.cpp`、`ob_ai_func_client.cpp`、`ob_ai_service_struct.cpp`

- `VALID_PROVIDERS` 增加 `OLLAMA`，端点可用任意大小写创建/查询/修改/删除。
- **免 API Key**：`check_valid` 对 `OLLAMA` 允许 `access_key` 为空；
  `ObAIFuncClient::init` 不再强制要求 headers 非空。
- 请求头只下发 `Content-Type: application/json`，不带 Authorization。
- 文本生成：非流式请求体 `{"model": ..., "prompt": ..., "stream": false}`，解析 `$.response`。
- Embedding：兼容 `{"embeddings": [[...]]}` 与旧版 `{"embedding": [...]}`。

## 五、AI_RERANK 结果正确性

改动文件：`ob_expr_ai_rerank.cpp/.h`

- 字符串数组与对象数组（指定 `doc_key`）两条路径都按 `batch_size = 20` 分批请求。
- 每批结果做返回校验：条数等于该批文档数、`index` 为整数且在当前批范围内不重复。
- 全局索引映射：每批 `index` 加上批起始偏移，输出中 `index` 是原始文档全局下标。
- 稳定排序：按 `relevance_score` 降序、同分按 `index` 升序。
- 对象数组路径回填原始文档对象并追加 `_model_score` 字段。

## 六、AI_EMBED 跨行批量

改动文件：`ob_expr_ai_embed.cpp/.h`

- 新增向量化路径 `eval_ai_embed_batch`，在 `cg_expr` 中注册 `eval_batch_func_`。
- 同一请求内各行需同一 model key 与同一 dimension，单请求上限 10 条输入，超过自动拆批。
- 一次请求携带多条 `input`，响应按 `index` 归位，乱序返回也能还原顺序。
- NULL 输入传播：`sql` 为 NULL 的行返回 NULL 且不进入请求。
- 结果按行写回（`init_with_batch_idx`），行序与维度映射一一对应。

## 验证方式与已知限制

- 已用 OceanBase 官方 devtools 内的 clang 对全部改动文件做过 `-fsyntax-only` 语法检查，全部通过。
- 本机未构建完整 seekdb（依赖体量大、磁盘不足），因此未运行单测与 mysqltest。
- 语义变化提示：旧版 `ai_prompt.test` 中「JSON 参数应报错」「text/longtext 应报错」
  的断言与赛题要求相反，复跑前需按新语义更新。
