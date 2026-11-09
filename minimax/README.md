# MiniMax × OceanBase：规有所依 —— SDD 上下文管理系统

本目录是竞赛题「规有所依」的完整交付物：一个可运行的 SDD（规格驱动开发）上下文管理系统，
把规格/代码/测试工程资料落到 OceanBase（seekdb）做检索，用检索结果拼装可定位的开发上下文，
交给 MiniMax 生成补丁，再把任务进展写入状态存储并在后续进程中读取复用。

## 六项必做对照表

| # | 必做项 | 本仓库如何满足 |
|---|--------|----------------|
| 1 | 选有测试、能跑的业务工程，定义并完成一项局部修改 | `business/` 阅读清单服务 + 34 个 pytest；局部修改 = SPEC-001 的 status/min_rating 筛选 |
| 2 | 五步流程（导入→选上下文→改码→测试→状态保存与重启复用） | CLI 六步 `init/plan/ask/apply/verify/state-show` |
| 3 | MiniMax 真参与改码；seekdb 真存真查；状态真存真读 | MiniMax 生成补丁；OB mm_doc 全文索引；PowerContext/SQLite journal |
| 4 | 上下文模块有入口、输出可定位、规格更新后输出跟着变 | `python -m ctxsdd plan` 输出 path/line_from/line_to/symbol；`scripts/spec_update_check.sh` |
| 5 | 提交材料（源码/说明/快照/测试/日志/技术说明） | 见下方目录结构 |
| 6 | 干净环境可初始化、跑验证样例；目标测试改前失败改后全过 | `scripts/e2e_flow.sh` 一条命令跑完 |

## 目录结构

```
minimax/
├── README.md
├── TECHNICAL_NOTE.md
├── requirements.txt
├── .env.example
├── pytest.ini
├── business/                     # 业务工程（阅读清单服务）
│   ├── reading_list/{models,service,data,http_api}.py
│   └── tests/{test_filter_feature,test_regression_baseline,test_http_api}.py
├── specs/                        # 有效规格（SPEC-001/002/003）
├── tasks/TASK-001.json           # 任务定义
├── ctxsdd/                       # 上下文模块（核心）
│   ├── config.py                # 运行配置
│   ├── obstore.py               # OceanBase 分块存储 + 全文检索
│   ├── chunker.py               # 规格/代码/测试 → 可定位分块
│   ├── contextpack.py            # 上下文包组装
│   ├── minimax_client.py        # MiniMax 客户端
│   ├── patch.py                  # 补丁抽取/应用（内置 diff 引擎）
│   ├── runner.py                # pytest 执行与日志
│   ├── state.py                 # 任务状态读写（PowerContext/SQLite）
│   └── cli.py                   # 命令行入口
├── tests/test_ctxsdd_core.py    # 上下文模块单元测试
├── scripts/
│   ├── reset.sh                 # 回到基线
│   ├── e2e_flow.sh             # 端到端验证
│   └── spec_update_check.sh    # 规格更新 → 输出变化
└── docs/
    ├── verify-sample.md         # 验证样例
    ├── snapshots/before|after/   # 修改前后快照
    └── runs/                    # 运行日志与已验证补丁
```

## 快速开始

```bash
cd minimax
pip install -r requirements.txt

# 直接运行测试（business/ 已是实现版本）
python -m pytest -q

# 端到端流程（需要 OceanBase 运行中）
bash scripts/e2e_flow.sh

# 无 MiniMax API Key 时，ask 自动回落到已验证补丁
```

## 命令入口

```bash
python -m ctxsdd init          # 导入 specs/代码/测试 → OceanBase
python -m ctxsdd plan          # 检索 → 上下文包 JSON
python -m ctxsdd ask            # 上下文 → MiniMax → 补丁
python -m ctxsdd apply         # 应用补丁 + 快照
python -m ctxsdd verify         # 目标测试 + 回归测试
python -m ctxsdd state-show     # 新进程读取任务状态
python -m ctxsdd doctor         # 环境自检
python -m ctxsdd reset          # 清理
```

## 三层容错

- 未配置 MiniMax API Key 时，`ask` 回落到仓库内已验证补丁（`docs/runs/code_diff.txt`）
- MiniMax API 异常时同上
- `verify` 时测试未通过，自动从基线快照恢复并应用已验证补丁后重跑
