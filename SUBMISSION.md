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
| 本条提交的分支 | `amd-cagra-submit-v6` |

评测要求参赛 commit 是冻结 commit 的后代，本分支满足该条件，可自行复核：

```bash
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD && echo "descendant: ok"
```

## 2. 改了什么

全部改动都可在这条分支上按 commit 逐个查看。摘要：

1. **离线 DDL 追加阶段批次化**（`src/storage/ddl/ob_ddl_pipeline.cpp`、
   `ob_vector_index_ddl_pipeline.h`）
   基线对每一行调用一次 `add_snap_index(num = 1)`，250K 行意味着 25 万次适配器查找、
   25 万次快照加锁、25 万次 arena 分配和小批量追加。稠密向量、无 extra column 的常见
   路径改为先累积到算子内批次（≤1024 行或 ≤256K float），再一次性
   `add_snap_index(..., num = count)`；含 extra column 或向量长度异常的行走原逐行路径，
   并在切换前先 flush 以保持追加顺序。交给 CAGRA 的向量、ID、顺序都不变，建图结果与
   基线逐点一致，Recall 不受影响。

2. **检索侧 GPU 临时缓冲区复用**（`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）
   基线每次检索都 `cuvsRMMAlloc`/`cuvsRMMFree` 三个设备缓冲区。改为随 handle 缓存、
   按需增长，并在 `seekdb_cuvs_free()` 统一释放；`itopk_size` 默认值
   （`max(512, 4*topk)`）与 `search_width` 保持不变，检索结果不变。

3. **建图/检索参数可外部标定，默认值等于冻结基线**
   `SEEKDB_CUVS_BUILD_ALGO`/`GRAPH_DEGREE`/`INTERMEDIATE_DEGREE`/`NITER`/`ITOPK`/
   `SEARCH_WIDTH`/`TRACE` 七个环境变量，默认值就是 `nn_descent + 128 + 256 + 100`。
   不设置任何变量时，建出的图与基线完全相同；标定流程见
   `docs/gpu-vector-index-hipvs-cuvs/contest-optimization.md` 第 4 节。

4. **可复现的测量与报告工具**（`tools/hipvs_bridge/bench/`）
   `run_build_bench.py` 复现"`CREATE VECTOR INDEX` → 索引可查询 → 第一条固定查询返回"
   这段计时窗口，输出 `results.json` / `results.md` / `observer.log`；`sweep.sh` 做参数
   扫描；`fill_report.py` 把基线 / 候选 / CPU 三组实测结果填进赛题要求的对比报告表格，
   并给出 Recall 门槛的 pass/FAIL 判定。

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

## 4. 复现测试

```bash
python3 tools/hipvs_bridge/bench/run_build_bench.py \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-bench --port 2981 \
  --n 250000 --dim 128 --nq 4096 --lib both \
  --out /tmp/seekdb-bench/sift250k

python3 tools/hipvs_bridge/bench/fill_report.py \
  --dataset SIFT250K \
  --baseline /tmp/seekdb-bench/baseline-sift/results.json \
  --candidate /tmp/seekdb-bench/sift250k/results.json \
  --cpu /tmp/seekdb-bench/sift250k/results.json
```

Hidden250K 用 `--dim 256` 再跑一遍。参数扫描用
`tools/hipvs_bridge/bench/sweep.sh`（`--grid` 每行一个参数点）。

## 5. 硬门槛自检

| 门槛 | 本分支的处理 |
| --- | --- |
| 基于 V5 冻结 commit | 是，`634888d9b` 是 HEAD 的祖先 |
| GPU 真实参与建图与检索 | 建图仍走 `cuvsCagraBuild`，检索仍走 `cuvsCagraSearch`；未引入 CPU 建图或 CPU 检索替代 |
| 结果正确、ID 不重复 | 批次化不改变向量/ID/顺序；检索 `topk` 行为未变 |
| Recall 门槛 | 建图参数与基线逐项相同，图与基线同源；未放宽任何建图参数 |
| 查询吞吐 ≥ 基线 90% | 检索 `itopk_size`/`search_width` 默认值未变，仅复用缓冲区 |
| 显存不超基线 1.25 倍 | 仅复用已释放过的检索缓冲，峰值不高于基线 |
| `lib=vsag` CPU 路径可用 | 未改动 VSAG 路径，批次化在 `lib=vsag` 下同样走批量 `add_index` |
| 无预编译二进制、无跨轮缓存复用 | 全部由源码构建；GPU 快照仍在同一轮内产生并重载 |

## 6. 已知限制（如实说明）

* 本分支的**实测耗时/Rcall 数值不在仓库内**：本机没有 AMD Radeon + ROCm + hipVS 环境，
  无法编译和跑 GPU 测试。报告表格按赛题要求留在
  `docs/gpu-vector-index-hipvs-cuvs/contest-optimization.md` 第 5.3 节，用第 5.1/5.2 节的
  流程在评测同款环境（W7900D / ROCm 7.2.4 / 24 核）跑完后由 `fill_report.py` 自动填充。
  正式成绩以组织方云端统一评测为准。
* `lib=cuvs` 仍只支持稠密 FP32 + L2，与基线一致；余弦/内积在 DDL 阶段拒绝。
* 参数标定旋钮只有在确认 Recall 满足"绝对下限 + 相对同轮基线下降 ≤ 0.005"后才建议写死
  为默认值；本分支没有擅自收紧默认参数。
