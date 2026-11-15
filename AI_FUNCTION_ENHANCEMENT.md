# 赛题一：AI 函数增强 —— 实现说明

本文说明本分支对 seekdb AI 函数服务所做的增强，对应赛题一的 6 个子题。

- 分支：`ai-func-enhancement`（基于 `origin/master` 的 `c1654f16` 新建，未复用任何历史分支）
- 主要改动集中在 `src/sql/engine/expr/ob_expr_ai/` 与 `src/share/ai_service/`

## 一、AI_PROMPT 类型与模板增强

改动文件：`ob_expr_ai_prompt.cpp/.h`、`ob_ai_func_utils.cpp/.h`

1. **TEXT / LONGTEXT 支持**：`calc_result_typeN` 对 template 与参数同时接受
   `VARCHAR/CHAR/BINARY` 与 `TINYTEXT/TEXT/MEDIUMTEXT/LONGTEXT`；求值时统一走
   `ObTextStringHelper::get_string`，按 LOB 方式读取真实字符串，长文本不会被截断。
   seekdb 的类型系统里没有独立的 blob ObObjType，`tinyblob/mediumblob/longblob` 列
   落在 text 类型类上，因此同样可以作为模板和参数直接使用。
2. **JSON 参数**：模板与参数类型判定中放开 JSON，求值时用
   `ObJsonExprHelper::get_json_doc` 取出 JSON 文档并深拷贝进 `args` 数组，
   Array / Object 参数保持原有结构，不再报 "json type is not supported"。
3. **嵌套 Prompt**：`ObAIFuncPromptObjectUtils::render_args_node` 在渲染占位符时，
   若参数本身是一个合法的 prompt 对象（`{template, args}`），则递归展开成内层
   渲染后的文本（层级上限 `max_prompt_nest_depth = 16`），否则按紧凑 JSON 输出。
   因此 `AI_PROMPT(t2, AI_PROMPT(t1, ...))` 可以正常拼接。
4. **重复占位符**：`replace_all_str_args_in_template` 先把每个参数渲染一次缓存起来，
   再按出现顺序替换，`{0}...{0}` 这类重复占位符每一处都会被替换。
5. **非法下标**：`{n}` 中 n 超出参数个数、或参数为 JSON null 时返回错误；`{}`、`{a}`
   这类不构成下标的写法按普通字符原样保留。
6. **Unicode 与转义**：渲染按字节拷贝，多字节字符不受影响；参数经 JSON 解析后
   转义序列已还原，输出再由 JSON 打印器统一转义。
7. **返回值形态**：与基线一致，`AI_PROMPT` 返回打包后的 prompt JSON 文本
   （`{"args": [...], "template": "..."}`），渲染发生在 `AI_COMPLETE` 阶段。

## 二、AI_EMBED 单行语义

改动文件：`ob_expr_ai_embed.cpp`

- 输入支持 `VARCHAR/CHAR/BINARY` 与 `TEXT/LONGTEXT`（LOB 读取，不截断）。
- 结果类型为 `LONGTEXT`（LOB 结果）：高维向量（如 1536 维）的 JSON 文本可能长达成千
  上万字节，用 varchar 声明会在部分场景被按精度截断，改为 LOB 结果后
  `json_length()`、`json_valid()`、`length()` 均能拿到完整文本。
- `dimension` 参数必须是正整数：`<= 0` 直接报 `OB_INVALID_ARGUMENT`；传 `NULL`
  视为未指定，使用模型默认维度。显式指定时会在请求体中下发 `"dimensions": n`。
- 返回值为 JSON 数组文本，例如 `[0.1, 0.2, 0.3]`。
- 返回结果校验（`ObAIFuncJsonUtils::validate_embedding_array`）：
  必须是 JSON 数组、维度非空、元素必须都是数字；显式指定 `dimension` 时
  返回维度必须与请求维度一致，否则报错（`ai_embed, result dimension is not equal to dimension`）。

## 三、AI_COMPLETE 请求与响应

改动文件：`ob_expr_ai_complete.cpp`、`ob_ai_func_utils.cpp`

- 普通 Prompt、`AI_PROMPT` 生成的 JSON prompt、`TEXT/LONGTEXT` 长文本 prompt 均支持；
  prompt 为 JSON 时不再要求 `args` 全部是字符串，允许 JSON 数组/对象与嵌套 prompt。
- 参数个数 2~3：第三个参数（JSON config）可省略，也可传 `NULL`，均表示沿用模型
  默认参数，不会因为空字符串去解析 JSON 而报错。
- `parameters`（JSON config）必须是 JSON 对象：`get_json_object_form_str` 增加了
  类型校验，数组或标量不再被强转为 `ObJsonObject`，返回
  `OB_INVALID_ARGUMENT`（`invalid json object, an object is expected`）。
- 响应校验：`call_completion` 解析后若内容为空，返回 `OB_INVALID_DATA` 并回传
  `unexpected http message: response content is empty`；HTTP 非 200、JSON 结构
  不合法等异常响应沿用原有错误路径。
- `temperature` / `max_tokens` 等 config 字段由 `compact_json_object` 原样透传进请求体。

## 四、Ollama Provider

改动文件：`ob_ai_func_utils.cpp`、`ob_ai_func_client.cpp`、`ob_ai_service_struct.cpp`

- `VALID_PROVIDERS` 增加 `OLLAMA`，端点可用任意大小写创建/查询/修改/删除
  （`is_valid_provider` 与 `ob_provider_check` 均使用 `case_compare`），
  因此 `"provider":"ollama"`、`"Ollama"`、`"OLLAMA"` 均可正常工作。
- **免 API Key**：`ObAiModelEndpointInfo::check_valid` 对 `OLLAMA` 允许 `access_key` 为空；
  `ObAIFuncClient::init` 不再强制要求 headers 非空。
- 请求头：`ObOllamaUtils::get_header` 只下发 `Content-Type: application/json`，不带 Authorization。
- 文本生成：`get_complete_provider` 增加 `OLLAMA -> ObOllamaComplete`；
  请求体为 `{"model": ..., "prompt": ..., "stream": false}`（非流式），
  响应解析 `$.response`，并要求其为字符串。
- Embedding：`get_embed_provider` 增加 `OLLAMA -> ObOllamaEmbed`；
  响应同时兼容新版原生协议 `{"embeddings": [[...], ...]}` 与旧版
  `{"embedding": [...]}`，并逐个校验向量元素为数字。

## 五、AI_RERANK 结果正确性

改动文件：`ob_expr_ai_rerank.cpp/.h`

- 字符串数组与对象数组（指定 `doc_key`）两条路径都按 `batch_size = 20` 分批请求。
- `doc_key` 传 `NULL` 等同于三参数形式（文档是字符串数组），不会被当成空 key 报错。
- 每批结果做返回校验（`validate_rerank_result`）：条数必须等于该批文档数、
  每条必须是含 `index` / `relevance_score` 的对象、`index` 为整数且在当前批范围内、
  `index` 不重复。
- 全局索引映射：`batch_result_add_base` 给每批的 `index` 加上批起始偏移，
  保证输出中的 `index` 是原始文档的全局下标。
- 稳定排序：`sort_rerank_result` 按 `relevance_score` 降序、同分按 `index` 升序
  （插入排序），同分文档保持原有相对顺序。
- 对象数组路径下，`attach_score_to_documents` 按排序结果回填原始文档对象
  （原始字段如 `id` 完整保留），并追加 `_model_score` 字段。

## 六、AI_EMBED 跨行批量

改动文件：`ob_expr_ai_embed.cpp/.h`

- 新增向量化路径 `ObExprAIEmbed::eval_ai_embed_batch`，并在 `cg_expr` 中注册
  `rt_expr.eval_batch_func_`：一次 `eval_batch` 拉起整批参数，而不是逐行调用。
- 同一请求内的行满足「同一 model key + 同一 dimension」，单请求上限
  `MAX_EMBED_BATCH_SIZE = 10` 条输入，超过则自动拆成多批次。
- 一次请求携带多条 `input`，响应按 `index` 归位
  （`collect_ordered_embeddings`）：先按请求条数开槽，逐条用响应里的 `index`
  落位（响应缺 `index` 时退化为按数组顺序），缺号/重号/越界均报 `OB_INVALID_DATA`。
- NULL 输入传播：`sql` 为 NULL 的行直接返回 NULL 且不进入请求。
- 结果按行写回：使用 `ObTextStringDatumResult::init_with_batch_idx` 把每行结果
  写进该行对应的 batch 结果内存，保证行序与维度映射一一对应。
- 批次请求失败时整条表达式返回错误，不会把上一批结果串到当前行。

## 七、对齐官方更新版用例（`ai_prompt.test` / `ai_function_enhancement.test`）

官方下发的两份更新版用例是可获得的唯一硬指标（所有公开参与者仓库都没有配套的
`.result`），据此对齐了以下行为：

1. `AI_PROMPT(NULL)`、`AI_PROMPT('a', NULL)` 报 `OB_ERR_INVALID_TYPE_FOR_OP`（客户端 5083），
   与用例中的 `--error 5083` 一致；模板为空字符串仍报 `OB_INVALID_ARGUMENT`（客户端 1210）。
2. 参数个数少于占位符个数时（如 `{0}+{1}={2}` 只给一个参数）仍成功返回：
   `AI_PROMPT` 只负责打包 prompt JSON，渲染在 `AI_COMPLETE` 里发生，打包阶段不报错。
3. `tinyblob/mediumblob/text/longtext` 列作为模板必须成功（原先的 5083 改为返回结果）。
4. `AI_EMBED` 结果类型改为 `LONGTEXT`，`json_length(ai_embed(...))` 能拿到完整向量长度。
5. `ai_complete(model, prompt, NULL)` 与 `ai_complete(model, prompt)` 均可正常发起请求。
6. `ai_rerank(model, query, '[{"id":"a",...}]', NULL)` 走字符串文档路径，
   与四参数形式传 `NULL` 的语义一致。

## 验证方式与已知限制

- 已用 OceanBase 官方 devtools 内的 clang 对全部改动文件做过
  `-fsyntax-only` 语法检查（`src/sql/engine/expr/ob_expr_ai/*`、
  `src/share/ai_service/ob_ai_service_struct.cpp`），全部通过。
- 本机未构建完整 seekdb（依赖体量大、磁盘不足），因此**未运行**
  `ob_expr_ai_prompt_test` / `test_ob_openai_utils` 单测与 mysqltest，
  SQL 层行为建议在具备 Mock AI Service（0.0.0.0:8000）的环境下按下列用例复跑。
- 官方用例只提供了 `.test`，没有 `.result`，因此错误文案无法逐字核对，本分支对齐的是
  错误码（5083 / 1210）与“必须成功/必须失败”的断言。
- 语义变化提示：仓库内旧版 `ai_prompt.test` 与 `ai_prompt.result` 未在本分支更新，
  其中的「JSON 参数应报错」「text/longtext 应报错」断言与赛题要求相反；
  评测应使用官方更新的 `ai_prompt.test`。

### 自测用例（需要 Mock AI Service）

```sql
-- 1. AI_PROMPT：JSON 参数、嵌套、重复占位符
SELECT AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1, true), JSON_OBJECT('k', 2));
SELECT AI_PROMPT('{0} / {0}', AI_PROMPT('inner {0}', 'x'));
SELECT AI_PROMPT('{0}', 'a') = '{"args": ["a"], "template": "{0}"}';

-- 2. AI_EMBED 单行 / 维度校验
SELECT JSON_LENGTH(AI_EMBED('mock_embed', 'hello'));
SELECT JSON_LENGTH(AI_EMBED('mock_embed', 'hello', 768));
SELECT JSON_LENGTH(AI_EMBED('mock_embed', 'hello', 3));  -- mock 返回 768 维，应报维度不一致

-- 3. AI_EMBED 跨行批量（超过 10 行会拆成多请求）
SELECT id, JSON_LENGTH(AI_EMBED('mock_embed', content)) FROM t_embed ORDER BY id;
SELECT id, JSON_LENGTH(AI_EMBED('mock_embed', content)) FROM t_embed WHERE content IS NOT NULL LIMIT 3;

-- 4. AI_RERANK 分批与全局索引
SELECT AI_RERANK('mock_rerank', 'apple', (SELECT JSON_ARRAYAGG(txt) FROM t_doc), NULL);
SELECT AI_RERANK('mock_rerank', 'apple', doc, 'text') FROM t_doc_obj;

-- 5. Ollama（access_key 可空，provider 大小写不敏感）
CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_ollama',
  '{"ai_model_name":"ob_complete","url":"http://127.0.0.1:11434/api/generate",
    "provider":"ollama","access_key":""}');
SELECT AI_COMPLETE('ob_complete', 'hello');
```
