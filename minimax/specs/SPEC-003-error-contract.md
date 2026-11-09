# SPEC-003 单条读取与错误契约

- 状态：active
- 归属：reading_list 服务与 HTTP 入口
- 关联任务：TASK-001（必须保持）

## 1. 单条读取

`ReadingListService.get_item(item_id)` 返回匹配条目；不存在时返回 `None`，
不得抛出异常、不得返回空对象代替。

## 2. 错误表达

服务层用 `ValueError` 表达参数错误；HTTP 层把 `ValueError` 映射为
HTTP 400 且响应体形如 `{"error": "<message>"}`。

## 3. HTTP 契约

- `GET /healthz` → `200 {"status": "ok"}`。
- `GET /items` 的查询参数 `page` / `page_size` / `sort` / `status` / `min_rating`
  必须 1:1 透传给服务层。
- 未知路径 → `404 {"error": "not found"}`。

## 4. 验收标准

- `/items` 默认返回 10 条，`total=12`，首条为 `r12`。
- `/items?page=2` 返回 `r02, r01`。
- `/items?page=0` 返回 400。
