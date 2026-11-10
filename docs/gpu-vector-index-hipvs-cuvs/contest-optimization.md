# AMD 赛题：使用 hipVS 加速 HNSW 索引构建 — 优化与验证说明

本文档说明本仓库相对 AMD 赛题冻结基线
`amd-cagra-contest-baseline-v5`（commit `634888d9b037153fa143d2c913ba4b25503e1d62`）
所做的改动、可复现的测试方法，以及 CPU/GPU 构建性能对比报告的填写方式。

## 1. 基线的构建链路

冻结基线已经完成 GPU 接入，`CREATE VECTOR INDEX ... lib=cuvs` 的离线 DDL 路径为：

```
DDL 扫描基表
  -> ObHNSWIndexAppendBufferOperator::append_row()        每行一次
     -> acquire_adapter_guard()                          每行一次适配器查找 + 加锁
     -> add_snap_index(num = 1)                          每行一次 arena 分配 + 快照锁
        -> obvectorutil::add_index()                     每行一次 marked 集合查找
           -> obcuVS 缓冲区追加                         每行一次 std::vector 扩容
  -> ObHNSWIndexBuildOperator::serialize_vector_index()
     -> ob_cuvs_serialize()                              CAGRA 建图（GPU）+ 快照落盘
  -> 第一条固定查询                                        CAGRA 检索（GPU）
```

其中 CAGRA 建图参数是 `NN-Descent + graph_degree=128 + intermediate_graph_degree=256
+ nn_descent_niter=100`。这组参数是为了让 SIFT(128d) 与 Hidden(256d) 两个数据集同时
满足 Recall 门槛而特意调高的（基线提交 `3d88d5bb`、`ad3253a5`），**降低它几乎必然
触发 `recall@*_baseline_delta` 硬门槛**，所以本题的加速空间主要在建图之外的
CPU 侧数据通路、批次化流程与 GPU 缓冲区生命周期上——这也是赛题第 3 节列出的
优化方向。

## 2. 本仓库的改动

### 2.1 离线 DDL 追加阶段批次化（`src/storage/ddl/`）

`ObHNSWIndexAppendBufferOperator` 原先对每一行调用一次 `add_snap_index(..., num=1)`。
250K 行意味着 25 万次适配器查找、25 万次快照加锁、25 万次 `std::vector` 小批量追加。

改动：

* 稠密向量、无 extra column 的常见路径下，行向量先累积到算子内部的批次缓冲区
  （最多 1024 行或 256K 个 float，约 1 MB），再一次性调用
  `add_snap_index(batch_vectors, batch_vids, nullptr, 0, count)`；
* 适配器 guard 从"每行获取"变为"每批获取"；
* 每个 row file 处理结束时强制 flush，保证进入 `serialize_vector_index()` 之前
  数据已全部写入快照；
* 含 extra column、或向量长度异常的行走原有逐行路径（先 flush 批次以保持追加顺序），
  行为与基线完全一致。

这条路径只改变"把行交给索引"的方式，不改变交给 CAGRA 的向量、ID、顺序，也不改变
CAGRA 的建图参数，因此**建图结果与基线逐点一致**，Recall 不变。

### 2.2 查询侧 GPU 临时缓冲区复用（`tools/hipvs_bridge/`）

基线的 `seekdb_cuvs_search()` 每次查询都 `cuvsRMMAlloc` / `cuvsRMMFree` 三个设备缓冲区
（query、ids、dist）。评测的 4096 条 Top-10 / Top-100 查询都是单行查询，这部分
分配/释放是纯开销。

改动：缓冲区随 handle 缓存、按需增长（`grow-on-demand`），`seekdb_cuvs_free()` 统一释放。
同一 handle 上的检索由上层串行化，因此无需额外加锁。检索调用、`itopk_size` 默认值
（`max(512, 4*topk)`）与 `search_width=4` 均保持不变，Recall 不变。

### 2.3 建图/检索参数可外部标定（不改变默认值）

`tools/hipvs_bridge/seekdb_cuvs_bridge.c` 增加了环境变量入口，**默认值就是冻结基线
的参数**，因此不设置任何变量时建出的图与基线完全相同：

| 环境变量 | 默认值 | 含义 |
| --- | --- | --- |
| `SEEKDB_CUVS_BUILD_ALGO` | `nn_descent` | `nn_descent` / `ivf_pq` / `auto` |
| `SEEKDB_CUVS_GRAPH_DEGREE` | `128` | 最终图度数 |
| `SEEKDB_CUVS_INTERMEDIATE_DEGREE` | `256` | 中间图度数 |
| `SEEKDB_CUVS_NITER` | `100` | NN-Descent 迭代次数 |
| `SEEKDB_CUVS_ITOPK` | `0`（= `max(512, 4*topk)`） | 检索 `itopk_size` |
| `SEEKDB_CUVS_SEARCH_WIDTH` | `4` | 检索 `search_width` |
| `SEEKDB_CUVS_TRACE` | `0` | `1` 时把每次建图/检索的 GPU 耗时打到 stderr |

这些开关用于在评测同款 GPU 上做参数标定：先测量基线参数下的构建时间与 Recall，
再逐点收紧参数，只有当 Recall 仍满足"绝对下限 + 相对基线下降 ≤ 0.005"时才采用。
标定完成后把选中的值写死为默认值即可（不需要在评测环境设置环境变量）。

## 3. 统一编译方式（与赛题一致）

```bash
# 1. hipVS bridge
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh <输出目录>/libseekdb_cuvs_bridge.so

# 2. seekdb
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=<输出目录>/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb -- -j64
```

产物：`build_release/src/observer/seekdb` 与 bridge 动态库。

## 4. 可复现的测试方法

评测的计时窗口是"执行 `CREATE VECTOR INDEX` -> 索引可查询 -> 第一条固定查询返回"，
且索引参数、SQL、数据集固定。`tools/hipvs_bridge/bench/run_build_bench.py` 在单机上
复现这一窗口：

```bash
python3 tools/hipvs_bridge/bench/run_build_bench.py \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-bench --port 2981 \
  --n 250000 --dim 128 --nq 256 \
  --lib both \
  --out /tmp/seekdb-bench/sift250k
```

脚本会：

1. 用独立 `base-dir` / 端口拉起一个 seekdb observer；
2. 建 `items(id BIGINT PRIMARY KEY, v VECTOR(dim))` 并导入数据（默认生成聚类合成数据，
   可用 `--fvecs` 指定 SIFT 的 `.fvecs` 文件）；
3. 在同一个 mysql 会话里执行
   `CREATE VECTOR INDEX ... WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64)`
   与第一条 `ORDER BY l2_distance(...) APPROXIMATE` 查询，记录总耗时；
4. 用 `nq` 条固定查询测 QPS / 平均延迟，并用 NumPy 精确 kNN 计算 Recall@10 / Recall@100；
5. `--lib both` 时同时对 `lib=vsag`（CPU）重复构建，得到 CPU/GPU 对比；
6. 输出 `results.json`、`results.md`（报告表格）与 `observer.log`
   （其中 `[OBVSAG][cuVS] serialized GPU CAGRA snapshot ... build_us=... graph_bytes=...`
   一行可以直接读出 GPU 建图耗时）。

参数扫描：

```bash
cat > grid.txt <<'EOF'
nn_descent 128 256 100
nn_descent 128 256  60
nn_descent 128 192  40
nn_descent  96 192  40
nn_descent 128 256  20
auto auto auto auto
EOF

tools/hipvs_bridge/bench/sweep.sh \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-bench --out /tmp/seekdb-bench/sweep \
  --n 250000 --dim 128 --nq 256 --lib cuvs --grid grid.txt
```

`sweep.sh` 每行参数重启一次 observer 并输出一行 Markdown 表格，可直接贴进第 5 节。
`sweep.sh` 会把 `SEEKDB_CUVS_*` 变量注入 observer 进程，因此不需要改代码。

## 5. CPU/GPU 构建性能对比报告

> 下表按赛题要求给出"冻结基线 / 候选版本"、"GPU / CPU"的构建耗时与 Recall 对比。
> 本仓库只提供测量工具与候选实现，**具体数值必须在评测同款环境（Radeon PRO W7900D、
> ROCm 7.2.4、24 核 CPU 配额）上跑 `run_build_bench.py --lib both` 后填写**，不要用
> 其它机器或其它数据集的自测数字代替。

| 数据集 | 后端 | 构建耗时（含第一条查询, s） | 加速比 | Recall@10 | Recall@100 |
| --- | --- | ---: | ---: | ---: | ---: |
| SIFT250K (250K×128) | 冻结基线 `lib=cuvs` | 待填 | 1.00x | 待填 | 待填 |
| SIFT250K (250K×128) | 本仓库 `lib=cuvs` | 待填 | 待填 | 待填 | 待填 |
| SIFT250K (250K×128) | `lib=vsag`（CPU 参考） | 待填 | 待填 | 待填 | 待填 |
| Hidden250K (250K×256) | 冻结基线 `lib=cuvs` | 待填 | 1.00x | 待填 | 待填 |
| Hidden250K (250K×256) | 本仓库 `lib=cuvs` | 待填 | 待填 | 待填 | 待填 |
| Hidden250K (250K×256) | `lib=vsag`（CPU 参考） | 待填 | 待填 | 待填 | 待填 |

赛题公布的 V5 冻结基线参考值（W7900D，10 次运行）：SIFT250K 20～27 s（中位数约 23 s），
Hidden250K 29～35 s（中位数约 30 s）。

Recall 门槛（必须同时满足，否则本次计 0 分）：

* SIFT250K：Recall@10 / @100 ≥ 0.95；
* Hidden250K：Recall@10 ≥ 0.90、Recall@100 ≥ 0.85；
* 两个数据集都要求相比**同轮**冻结基线下降不超过 0.005。

## 6. 注意事项

* `lib=vsag` 的 CPU 路径未被改动，SQL 建表/建索引/查询仍可用；
* 本改动不改变 `lib=cuvs` 的建图算法与默认参数，GPU 仍然实质参与建图与检索；
* 快照（`OBCUVS02`）格式未变，跨进程重载逻辑未变；
* 若后续采用标定后的更快参数，必须在报告中同时给出 Recall，并确认满足第 5 节门槛。
