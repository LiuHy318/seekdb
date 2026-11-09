# 验证样例

## 任务

为阅读清单服务新增按状态（status）与最低评分（min_rating）的筛选能力。

## 初始数据

`business/reading_list/data.py` 中 12 条确定性阅读记录。

## 操作步骤

```bash
cd minimax
bash scripts/reset.sh              # 回到基线
python -m ctxsdd init              # 导入到 OceanBase
python -m ctxsdd plan              # 检索上下文包
python -m ctxsdd ask              # MiniMax 生成补丁（无 key 时回落）
python -m ctxsdd apply             # 应用补丁
python -m ctxsdd verify           # 跑测试
python -m ctxsdd state-show        # 查看状态
```

## 预期结果

- 目标测试（test_filter_feature.py）：14 passed
- 回归测试（test_regression_baseline.py + test_http_api.py）：20 passed
- state-show 显示 verified=true

## 重置方法

```bash
bash scripts/reset.sh
```
