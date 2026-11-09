# SPEC-001 阅读清单筛选能力

- 状态：active
- 归属：reading_list 服务
- 关联任务：TASK-001

## 1. 能力声明

`ReadingListService.list_items` 必须支持两个可选筛选参数，与既有的 `sort` /
`page` / `page_size` 组合使用：

- `status`：按阅读状态筛选，接受 `Status` 枚举或其字符串值
  （`to_read` / `reading` / `finished`）。
- `min_rating`：按最低评分筛选，只保留 `rating >= min_rating` 的条目。

两个参数同时给出时按逻辑与（AND）组合：条目必须同时满足状态与评分条件。

## 2. 未评分条目的处理

`rating` 为 `None` 表示未评分。`min_rating` 一旦给出，未评分条目必须被排除，
不得把 `None` 当作 0 或当作满分参与比较。

## 3. 参数校验

- `min_rating` 必须是 1..5 的整数；越界或非整数时抛出 `ValueError`。
- `status` 必须能解析为 `Status`；非法取值抛出 `ValueError`。
- 校验失败必须在返回任何结果之前发生，不得返回部分结果。

## 4. 与分页、排序的关系

筛选先于排序与分页：`total` 与 `total_pages` 必须描述筛选之后的全集。
筛选不得改变 SPEC-002 规定的排序规则、稳定次序与分页边界语义。
筛选不得修改服务实例的内部状态；连续调用必须彼此独立。

## 5. 验收标准

- `status="finished"` → 5 条，顺序为 `r11, r07, r04, r02, r01`；
- `min_rating=4` → 6 条，未评分的 `r05 / r06 / r09 / r10` 被排除；
- `status="finished", min_rating=4` → 4 条，顺序为 `r11, r04, r02, r01`；
- `status="to_read", min_rating=5` → 0 条，`total=0`、`total_pages=0`。
