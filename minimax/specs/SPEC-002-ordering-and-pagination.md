# SPEC-002 排序与分页语义

- 状态：active
- 归属：reading_list 服务
- 关联任务：TASK-001（必须保持）

## 1. 排序模式

`list_items(sort=...)` 支持三种模式，默认 `added_at_desc`：

- `added_at_desc`：`added_at` 由新到旧；
- `added_at_asc`：`added_at` 由旧到新；
- `title_asc`：`title` 字典序升序。

## 2. 稳定次序

任何排序模式下，若主排序键相同，则以 `id` 升序作为次排序键。同一请求的重复调用
必须返回完全相同的顺序（确定性）。

## 3. 分页参数

- `page` 为 1 起始的正整数；`page < 1` 或非整数时抛出 `ValueError`。
- `page_size` 必须在 1..100 之间，越界或非整数时抛出 `ValueError`。
- 未知的 `sort` 取值抛出 `ValueError`。

## 4. 分页结果的边界

- `total` 是当前结果集（含筛选）的条目总数，与当前页无关。
- `total_pages = ceil(total / page_size)`；当 `total = 0` 时 `total_pages = 0`。
- `page` 超出 `total_pages` 时返回空 `items`，但 `total` 与 `total_pages` 保持真实值。

## 5. 验收标准

- 12 条初始数据、`page_size=5`：第 1 页 5 条、第 3 页 2 条、第 4 页 0 条，
  三者 `total=12`、`total_pages=3`。
- 空集合：`total=0`、`total_pages=0`、`items=[]`。
