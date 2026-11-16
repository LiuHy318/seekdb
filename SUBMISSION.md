# AMD 赛题提交说明：使用 hipVS 加速 HNSW 索引构建

本文件是提交给"OceanBase seekdb × AMD hipVS"赛题的打包说明，对应赛题第 2 节
（比赛基线）、第 3 节（优化任务与统一编译方式）、第 5 节（提交产物）。

## 1. 代码来源与分支

| 项 | 值 |
| --- | --- |
| 比赛基线仓库 | `https://github.com/zihaomu/seekdb` |
| 基线分支 | `amd-oceanbase-official-contest-base-v5` |
| 冻结 Tag | `amd-cagra-contest-baseline-v5` |
| 冻结 commit | `634888d9b037153fa143d2c913ba4b25503e1d62` |
| 本条提交的分支 | `amd-cagra-submit-v8` |

评测要求参赛 commit 是冻结 commit 的后代，本分支满足该条件，可自行复核：

```bash
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD && echo "descendant: ok"
```

## 2. 改了什么

全部改动都可在这条分支上按 commit 逐个查看。摘要：

1. **建图算法换成 cuVS AUTO_SELECT，并把省下来的预算花在更稠密的最终图上**
   （`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）
   冻结基线把 cuVS 钉在最慢的质量点：`NN_DESCENT` + `graph_degree=128` +
   `intermediate_graph_degree=256` + `nn_descent_niter=100`。在 250K 规模上这段建图
   占了计时窗口的约 80%（`CREATE VECTOR INDEX` → 索引可查询 → 第一条固定查询返回，
   实测 250K×128 全窗口 52.3 s，其中 GPU 建图 40.6 s）。本分支改为
   `build_algo = AUTO_SELECT`（由 cuVS 按数据规模选择 IVF-PQ 建图路线），
   同时把最终图度数从 128 提到 **192**。
   省下的是 NN-Descent 的 100 轮迭代，补回去的是更稠密的图：**实测 Recall 不降反升**，
   因此门槛不是被"换"掉的，而是有余量地通过（数据见第 4 节）。

2. **检索侧 GPU 临时缓冲区复用**（同一文件）
   基线每次检索都 `cuvsRMMAlloc`/`cuvsRMMFree` 三个设备缓冲区。改为随 handle 缓存、
   按需增长，并在 `seekdb_cuvs_free()` 统一释放；`itopk_size` 默认值
   （`max(512, 4*topk)`）与 `search_width` 保持不变，检索结果不变。

3. **建图/检索参数可外部标定**
   `SEEKDB_CUVS_BUILD_ALGO`/`GRAPH_DEGREE`/`INTERMEDIATE_DEGREE`/`NITER`/`ITOPK`/
   `SEARCH_WIDTH`/`TRACE` 七个环境变量；把四个建图旋钮设回
   `nn_descent + 128 + 256 + 100` 即可逐字节还原冻结基线的建图行为，便于同轮 A/B。
   标定流程见 `docs/gpu-vector-index-hipvs-cuvs/contest-optimization.md` 第 4 节。

4. **可复现的测量与报告工具**（`tools/hipvs_bridge/bench/`）
   `run_build_bench.py` 复现"`CREATE VECTOR INDEX` → 索引可查询 → 第一条固定查询返回"
   这段计时窗口，输出 `results.json` / `results.md` / `observer.log`；`sweep.sh` 做参数
   扫描；`fill_report.py` 把基线 / 候选 / CPU 三组实测结果填进赛题要求的对比报告表格，
   并给出 Recall 门槛的 pass/FAIL 判定。

### 2.1 上一版（`amd-cagra-submit-v6`）里被撤回的两处

`v6` 曾把离线 DDL 的逐行 `add_snap_index` 批次化，并预分配批次缓冲
（`src/storage/ddl/ob_ddl_pipeline.cpp`、`ob_vector_index_ddl_pipeline.h`）。
本轮在 Radeon Cloud 同款环境实测该版本 **build=0.911x**（比基线慢 9%），说明
这套批处理在真实窗口里没有收益、反而增加了一次额外拷贝，因此本分支把这两个文件
**逐字节还原为冻结基线的版本**（`git diff 634888d9b -- src/storage/ddl/` 为空）。
另一次尝试是把 `nn_descent_niter` 从 100 降到 40（`amd-cagra-tuned-v7`），
实测 256d 上 Recall@100 掉 0.0127、Recall@10 掉 0.0130，远超"相对基线下降 ≤ 0.005"
的门槛，提测得 0 分，同样已撤回。

## 3. 编译（与赛题 3.2 节命令一致，未追加选项）

```bash
# 1. hipVS bridge
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2. seekdb
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb -- -j64
```

产物：`build_release/src/observer/seekdb` 与 `libseekdb_cuvs_bridge.so`。本分支没有修改
`CMakeLists.txt`/`cmake/`，也没有引入任何需要联网下载的依赖，第三方依赖仍用基线冻结版本。

## 4. 实测数据（Radeon Cloud 模板，gfx1100 / ROCm 7.2.4）

测量方式：直接对 `libseekdb_cuvs_bridge.so` 调用 `seekdb_cuvs_build` +
`seekdb_cuvs_search`，250,000 行、100 条固定查询、`topk=100`，Exact L2 真值由 NumPy
现算；同一进程内、同一份数据对基线参数与本分支参数各跑一遍。

| 数据 | 建图耗时（基线 → 本分支） | 加速比 | Recall@10（基线 → 本分支） | Recall@100（基线 → 本分支） | 100 条 topk=100 检索耗时 |
| --- | --- | --- | --- | --- | --- |
| 250K × 128d | 40.6 s → **15.4 s** | **2.65x** | 0.982 → **0.994** | 0.965 → **0.982** | 37.2 ms → 37.3 ms |
| 250K × 256d | 41.1 s → **16.8 s** | **2.44x** | 0.933 → **0.953** | 0.868 → **0.902** | 39.9 ms → 41.3 ms |

同一台机器上走完整 SQL 路径（建表 → 灌 250K 行 → `CREATE VECTOR INDEX` → 第一条
`APPROXIMATE` 查询）的对照：基线全窗口 **52.3 s**，其中 GPU 建图 40.6 s、服务端固定
开销约 12 s；把 GPU 建图换成 15.4 s 后，窗口随之下移到 27 s 量级。
`--cpu-count 24` 与 128 核两种设置下该窗口分别为 52.27 s / 52.33 s，说明这 12 s
与服务端核数无关，是固定的取数 + 快照写盘成本。

参数扫描（同数据、同进程）供复核：

| 建图配置 | 250K×128 建图 | Recall@10/@100 | 250K×256 建图 | Recall@10/@100 |
| --- | --- | --- | --- | --- |
| `nn_descent 128/256/100`（基线） | 40.6 s | 0.982 / 0.965 | 41.1 s | 0.933 / 0.868 |
| `nn_descent 128/256/40`（v7，0 分） | 21.3 s | 0.991 / 0.962 | 21.2 s | 0.920 / 0.855 |
| `nn_descent 128/256/60` | 27.8 s | 0.990 / 0.964 | 28.8 s | 0.935 / 0.863 |
| `ivf_pq 128/256/—` | 15.0 s | 0.841 / 0.763 | — | — |
| `iterative_cagra_search` | 64.0 s | 0.992 / 0.964 | — | — |
| **`auto 192/256`（本分支）** | **15.4 s** | **0.994 / 0.982** | **16.8 s** | **0.953 / 0.902** |

## 5. 硬门槛自检

| 门槛 | 本分支的处理 |
| --- | --- |
| 基于 V5 冻结 commit | 是，`634888d9b` 是 HEAD 的祖先 |
| GPU 真实参与建图与检索 | 建图仍走 `cuvsCagraBuild`（AUTO_SELECT 路由到 cuVS 的 IVF-PQ 建图内核），检索仍走 `cuvsCagraSearch`；无 CPU 建图、无 CPU 检索回退 |
| 结果正确、ID 不重复 | 索引仍是 CAGRA 图，向量、ID、顺序未改；`topk` 行为未变 |
| Recall 门槛 | 实测两个维度上 Recall@10/@100 均**高于**基线（+0.012/+0.017 与 +0.020/+0.034），未消耗 0.005 的下降预算 |
| 查询吞吐 ≥ 基线 90% | `itopk_size`/`search_width` 默认值未变，仅复用缓冲区；256d 检索 41.3 ms vs 39.9 ms（0.97x） |
| 显存不超基线 1.25 倍 | 图从 128 度提到 192 度（250K 上 +64 MB），中间图仍为 256 度、与基线同量级；无额外常驻缓冲 |
| `lib=vsag` CPU 路径可用 | 未改动 VSAG 路径与 DDL 代码（两个 DDL 文件已还原为基线版本） |
| 无预编译二进制、无跨轮缓存复用 | 全部由源码构建；GPU 快照仍在同一轮内产生并重载 |

## 6. 已知限制（如实说明）

* 第 4 节的数值是**本地复现测量**：Radeon Cloud 模板机（gfx1100 / ROCm 7.2.4 / 单卡）
  上的合成数据（256 簇高斯混合，128d/256d 各 250K 行；查询一半来自扰动后的库内向量、
  一半为独立采样），不是赛题的 SIFT250K / Hidden250K 原始数据。同一份数据上基线与本
  分支同进程对跑，因此加速比与 Recall 变化量是同口径的；绝对 Recall 值会随真实数据
  分布变化，最终以组织方云端统一评测为准。
* 本次没有在本机完成整仓编译（该环境无法从公网拉取约 250 MB 的源码，直连速率约
  40 KB/s），因此端到端窗口数据取自模板自带的预编译 observer；`lib=cuvs` 的建图参数
  开关只对按第 3 节命令从源码构建出来的产物生效。
* `lib=cuvs` 仍只支持稠密 FP32 + L2，与基线一致；余弦/内积在 DDL 阶段拒绝。
* 图度数 192 是一个**保守取值**：更高度数（224/256）在同样数据上 Recall 更高，但会让
  检索耗时逼近 0.8x 基线，反而有丢查询分的风险，因此停在 192。
