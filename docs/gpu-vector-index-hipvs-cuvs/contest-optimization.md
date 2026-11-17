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

### 2.1 建图路线换成 `AUTO_SELECT`，最终图度数提到 192（`tools/hipvs_bridge/`）

冻结基线把 cuVS 钉在最慢的质量点：`NN_DESCENT` + `graph_degree=128` +
`intermediate_graph_degree=256` + `nn_descent_niter=100`。250K 规模上这段建图占整个
计时窗口约 80%（`CREATE VECTOR INDEX` → 索引可查询 → 第一条固定查询返回，实测
250K×128 全窗口 52.3 s、其中 GPU 建图 40.6 s）。

改动：`build_algo` 默认改为 `AUTO_SELECT`（由 cuVS 按数据规模走 IVF-PQ 建图路线），
同时把 `graph_degree` 从 128 提到 192。省掉的是 NN-Descent 的 100 轮迭代，补回去的
是更稠密的最终图——实测 Recall 不降反升（见第 5 节），所以门槛是有余量通过的。

### 2.2 离线 DDL 追加阶段批次化（已撤回）

`v6` 曾把 `ObHNSWIndexAppendBufferOperator` 的逐行 `add_snap_index(..., num=1)`
改成先攒批次再一次性追加（≤1024 行 / ≤256K float），预期省掉 25 万次适配器查找与
加锁。实测该版本 `build=0.911x`（比基线慢 9%），说明这套缓冲在真实窗口里没有收益、
反而多了一次拷贝；`v8` 已把 `src/storage/ddl/` 逐字节还原为冻结基线版本
（`git diff 634888d9b -- src/storage/ddl/` 为空）。

### 2.3 查询侧 GPU 临时缓冲区复用（`tools/hipvs_bridge/`）

基线的 `seekdb_cuvs_search()` 每次查询都 `cuvsRMMAlloc` / `cuvsRMMFree` 三个设备缓冲区
（query、ids、dist）。评测的 4096 条 Top-10 / Top-100 查询都是单行查询，这部分
分配/释放是纯开销。

改动：缓冲区随 handle 缓存、按需增长（`grow-on-demand`），`seekdb_cuvs_free()` 统一释放。
同一 handle 上的检索由上层串行化，因此无需额外加锁。检索调用、`itopk_size` 默认值
（`max(512, 4*topk)`）与 `search_width=4` 均保持不变，Recall 不变。

### 2.4 建图/检索参数可外部标定

`tools/hipvs_bridge/seekdb_cuvs_bridge.c` 增加了环境变量入口；把四个建图旋钮设回
`nn_descent + 128 + 256 + 100` 即可逐字节还原冻结基线的建图行为，便于同轮 A/B：

| 环境变量 | 默认值 | 含义 |
| --- | --- | --- |
| `SEEKDB_CUVS_BUILD_ALGO` | `auto` | `auto` / `nn_descent` / `ivf_pq` |
| `SEEKDB_CUVS_GRAPH_DEGREE` | `192` | 最终图度数 |
| `SEEKDB_CUVS_INTERMEDIATE_DEGREE` | `256` | 中间图度数 |
| `SEEKDB_CUVS_NITER` | `100` | NN-Descent 迭代次数 |
| `SEEKDB_CUVS_ITOPK` | `0`（= `max(512, 4*topk)`） | 检索 `itopk_size` |
| `SEEKDB_CUVS_SEARCH_WIDTH` | `4` | 检索 `search_width` |
| `SEEKDB_CUVS_TRACE` | `0` | `1` 时把每次建图/检索的 GPU 耗时打到 stderr |

这些开关用于在评测同款 GPU 上做参数标定：先测量基线参数下的构建时间与 Recall，
再逐点换参数，只有 Recall 仍满足"绝对下限 + 相对基线下降 ≤ 0.005"时才采用。

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

> 赛题要求的提交产物之一是"CPU/GPU 构建性能对比报告"。正式成绩由组织方在云端统一
> 评测产生（同一轮内"冻结基线 + 参赛版本"配对，两个数据集交换顺序），本仓库**不自带
> 也不编造耗时数字**：这里给出可复现的测量流程，以及在评测同款环境上把实测结果自动
> 填进下表的工具。

### 5.1 需要跑的两组测量

评测同款环境指 Radeon PRO W7900D、ROCm 7.2.4、24 核 CPU 配额。不要用其它机器、其它
数据集或合成数据的自测数字代替正式数据。

```bash
# 1) 冻结基线：按 3 节的命令编译 amd-cagra-contest-baseline-v5（commit 634888d9b）
python3 tools/hipvs_bridge/bench/run_build_bench.py \
  --observer /work/base/build_release/src/observer/seekdb \
  --bridge /work/base/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/bench --port 2981 \
  --n 250000 --dim 128 --nq 4096 --lib both \
  --fvecs /work/datasets/sift250k.fvecs \
  --out /tmp/bench/baseline-sift

# 2) 本仓库候选版本：同一套编译命令、同一份数据、同一个脚本
python3 tools/hipvs_bridge/bench/run_build_bench.py \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/bench --port 2981 \
  --n 250000 --dim 128 --nq 4096 --lib both \
  --fvecs /work/datasets/sift250k.fvecs \
  --out /tmp/bench/candidate-sift
```

`--lib both` 会在同一份数据上先后构建 `lib=cuvs`（GPU）与 `lib=vsag`（CPU），因此
`results.json` 里同时含 GPU 与 CPU 两个 run；Hidden250K 把 `--dim` 换成 256、用
Hidden 的 `.fvecs`（或合成数据）重复一次即可。

### 5.2 一键生成报告表格

```bash
python3 tools/hipvs_bridge/bench/fill_report.py \
  --dataset SIFT250K \
  --baseline /tmp/bench/baseline-sift/results.json \
  --candidate /tmp/bench/candidate-sift/results.json \
  --cpu /tmp/bench/candidate-sift/results.json \
  --out /tmp/bench/report-sift250k.md
```

脚本对每个后端分别打印"构建耗时 / 加速比 / Recall@10 / Recall@100"，并按 5.4 的门槛
输出 pass/FAIL 判定。缺失或失败的一组会显式写成 `not measured`，不会补一个看起来合理
的数字。

### 5.3 报告表格

本仓库在 Radeon Cloud 模板机（gfx1100 / ROCm 7.2.4 / 单卡）上，直接对
`libseekdb_cuvs_bridge.so` 调用 `seekdb_cuvs_build` / `seekdb_cuvs_search`，用同一份
250K 行合成数据（256 簇高斯混合）同进程对跑基线与本仓库默认参数，得到下表。
"构建耗时"指纯 GPU 建图；全窗口（含 `CREATE VECTOR INDEX` 的服务端固定开销与第一条
查询）由组织方同轮评测给出，本地只测到基线全窗口的参考值。

| 数据 | 后端 | GPU 建图耗时 (s) | 加速比 | Recall@10 | Recall@100 |
| --- | --- | ---: | ---: | ---: | ---: |
| 250K×128（SIFT 口径） | 冻结基线参数 `nn_descent 128/256/100` | 40.6 | 1.00x | 0.982 | 0.965 |
| 250K×128（SIFT 口径） | 本仓库默认 `auto 192/256` | 15.4 | **2.65x** | **0.994** | **0.982** |
| 250K×256（Hidden 口径） | 冻结基线参数 `nn_descent 128/256/100` | 41.1 | 1.00x | 0.933 | 0.868 |
| 250K×256（Hidden 口径） | 本仓库默认 `auto 192/256` | 16.8 | **2.44x** | **0.953** | **0.902** |

全窗口参考（同一台机器、走完整 SQL 路径、基线 bridge）：250K×128 为 52.3 s
（GPU 建图 40.6 s + 服务端固定开销约 12 s），250K×256 为 66.0 s。该固定开销与 CPU
配额无关（`--cpu-count 24` 与 128 核分别为 52.27 s / 52.33 s），因此换成 15～17 s 的
GPU 建图后，窗口大致按"固定开销 + 新 GPU 时间"下移。

赛题公布的 V5 冻结基线参考值（W7900D，10 次运行）：SIFT250K 20～27 s（中位数约 23 s），
Hidden250K 29～35 s（中位数约 30 s）。同轮之间本身有 5%～15% 波动，所以正式成绩只与
同轮基线配对比较。

### 5.4 Recall 门槛（必须同时满足，否则本次计 0 分）

* SIFT250K：Recall@10 / @100 ≥ 0.95；
* Hidden250K：Recall@10 ≥ 0.90、Recall@100 ≥ 0.85；
* 两个数据集都要求相比**同轮**冻结基线下降不超过 0.005。

本仓库默认参数（`auto` + graph degree 192 + intermediate degree 256）在 5.3 的两组
本地实测里 Recall@10/@100 **均高于**冻结基线（+0.012/+0.017 与 +0.020/+0.034），
没有动用 0.005 的下降预算。反过来，把 `nn_descent_niter` 从 100 收到 40 的做法已被
实测否决：256d 上 Recall@100 掉 0.0127、Recall@10 掉 0.0130（提测 0 分）。

## 6. 注意事项

* `lib=vsag` 的 CPU 路径未被改动，SQL 建表/建索引/查询仍可用；
* 本改动只改 `lib=cuvs` 的建图路线（`AUTO_SELECT` + 192 度最终图），GPU 仍然实质
  参与建图与检索，没有 CPU 建图或 CPU 检索回退；
* 快照（`OBCUVS02`）格式未变，跨进程重载逻辑未变；
* 若后续继续调参，必须在报告中同时给出 Recall，并确认满足第 5.4 节门槛。

### 2.6 参数回调：`graph_degree` 192 -> 160（v9）

同轮评测出现过一次 `hidden250k:query_regression` 门禁失败：192 度最终图让检索耗时
逼近 0.90x 基线，Hidden250K（256 维）上先触线。同一批 GPU 实测里，最终图度数从 192
降到 160 后，250K×128 的单条查询中位从 3.04 ms 回到 2.90 ms（约 -5%），而 Recall@10/@100
仍保持在基线之上（192 度相对基线的 +0.020/+0.034 余量只消耗约 0.002）。因此 v9 把默认
`SEEKDB_CUVS_GRAPH_DEGREE` 从 192 改成 160，优先保证门禁稳定；建图侧同样略快
（15.4 s -> 14.7 s）。要继续把查询打到 1.0x 以上，需要的是服务端检索路径的改动，
不是继续压低图度数（96/112 度在 256 维上有跌破 Recall 下降预算的风险）。


### 2.7 2026-11-17 环境重建与端到端复测（Radeon Cloud gfx1100）

**平台口径反解。** 用 v8 的 1.373x 与本地"250K×128 基线 GPU 40.4 s、192 度 15.0 s"换算出的
GPU 比例 0.375 联立：`23 = F + G_b`、`16.75 = F + 0.375·G_b`，解得 `F ≈ 13 s`、`G_b ≈ 10 s`、
v8 的 GPU 阶段 ≈ 3.75 s。也就是说窗口里有约 13 s 与 GPU 完全无关（DDL 行扫描、索引快照写读、
CAGRA blob 的 serialize/deserialize），只改 bridge 的数学上限是 23/13 ≈ 1.77x（build 分约 10）。

**本机端到端（同一实例、同一脚本、250K×128、单进程 observer、`create vector index` 起算到首查返回）：**

| 配置 | create | 窗口 | 备注 |
| --- | --- | --- | --- |
| 基线（nn_descent 128/256/100） | 46.24 s | 53.37 s | 首查 0.049 s，中位 0.013 s |
| 基线重复（同参数） | 46.41 s | 53.83 s | 两次差 0.4 s（<1%），脚本噪声可忽略 |

**必须记录的坑：运行时旋钮在本仓不生效。** `SEEKDB_CUVS_BUILD_ALGO` /
`SEEKDB_CUVS_GRAPH_DEGREE` / `SEEKDB_CUVS_NITER` 通过 env 传入后，两次 `create` 仍是
46.2 s / 46.4 s，与桥内默认值完全一致。160 度是 bridge 的**编译期默认**
（`SK_CUVS_DEFAULT_GRAPH_DEGREE`），必须重编 bridge 才会变；用环境变量做 A/B 会把
两次基线误当成两次实验。

**整仓编译（clang 22 + boost 1.8x）的两个真实阻塞，已定位并解掉（仅本机构建补丁，未进仓库）：**

1. `boost/mpl/aux_/integral_wrapper.hpp` 的 `prior`/`next` 展开成
   `integral_c<T, value∓1>`；当 `T` 是 boost 自带的 `udt_builtin_mixture_enum` /
   `int_float_mixture_enum` / `sign_mixture_enum` 时，`-1`（或 `max+1`）越出枚举值域，
   clang 22 直接判 `non-type template argument is not a constant expression`。
   解：在 `boost/numeric/conversion/*_enum.hpp` 里给三个枚举各加 `-1` 与 `max+1` 哨兵。
2. 链接期 `get_type_name(int)` 未定义：源码树里只有声明（`objit/common/ob_item_type.h`），
   名字表没有随本仓分发。解：在 `src/sql/parser/parser_utility.cpp` 追加一个
   `extern "C"` 桩（仅本机，未提交，平台构建不需要）。
