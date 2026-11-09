# 一页技术说明：规有所依的 SDD 上下文管理系统

## 问题

规格（spec）与代码会持续演进，但"这次改动该看哪几条规格、动哪个符号、跑哪些测试"通常靠人记忆。
本系统把这件事做成可重复的机器流程：工程资料落库 → 检索装配上下文 → 生成补丁 → 用测试裁决 → 进展沉淀。

## 架构

- **chunker.py**：规格按 ## 切、代码按 AST 顶层定义切，每块带 path+行号
- **obstore.py**：OceanBase/seekdb mm_doc + ngram 全文索引 + LIKE 兜底
- **contextpack.py**：MATCH 分数 ⊕ LIKE 命中 → 上下文包（含 spec 指纹）
- **minimax_client.py**：OpenAI 兼容 /v1/chat/completions，补丁规范化
- **patch.py**：内置 unified-diff 引擎（-p1、上下文偏移搜索、no-op 拒绝）
- **state.py**：PowerContext journal（SQLite 兜底）：追加事件 → 折叠成当前进度

## 关键设计

1. **可定位**：上下文包里每条规格带 path/line_from/line_to/content_sha，代码带 path/symbol/line_from/line_to
2. **规格指纹**：spec_digest 随规格变化而变化，plan 输出随之变化
3. **状态持久化**：追加式 journal，新进程读取即可恢复任务状态
4. **容错**：无 API Key 时回落到已验证补丁，保证流程可复现

## 规模

- 分块：spec 按 ## 切、code/test 按 AST 切
- 业务工程：12 条初始数据、3 个规格、34 个测试
- 全流程约 1-2 分钟
