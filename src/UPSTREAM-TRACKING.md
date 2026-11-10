# diskinfer 对上游 llama.cpp 的版本跟进分析

> **【历史存档 · 2026-11-10】** 本文件分析的是旧基线 `0.7.0-dev @ 8e5a58d` 到当时 master 的差距。
> 仓库现已升级到 **v0.9.0（`cd959bae`）**：当前基线、补丁与未跟进提交见 `src/UPSTREAM.txt` 与
> `src/PATCHES.md`。下文“当前落后 376 个提交 / 建议 pin 到 v0.9.0”等结论只代表 2026-11-10 00:10
> 的分析时点，升级已完成，勿再据此判断现状。

只读分析，不改任何 C++ 代码。上游数据全部来自 GitHub REST API 的实时查询（查询时间见下）。

- 分析对象：`src/llama.cpp-fork/`（fork 基线 = zip 快照，目录里没有 `.git`）
- 分析时间：2026-11-10 00:10 - 00:35 (+08:00)
- 结论一句话：**当前落后 376 个提交 / 16.4 天，patch 点没有一处被上游删改到编译不过，但 MoE 快路径（MUL_MAT_ID）和 CPU 计算缓冲（F32 src1）这两条线必须适配；建议下一步 pin 到 v0.9.0 而不是 master HEAD。**

---

## 1. 数据来源

| 用途 | URL | 查询时间 (+08:00) |
|---|---|---|
| 基线 commit 校验 | `https://api.github.com/repos/ggml-org/llama.cpp/commits/8e5a58df87ab59dfceb5b961ae26691e39f30c24` | 2026-11-10 00:12 |
| 基线 -> master HEAD 全量提交 | `https://api.github.com/repos/ggml-org/llama.cpp/compare/8e5a58df87ab59dfceb5b961ae26691e39f30c24...a4d4f2fd4ec85ec6a0a9ece4870416c1b6cc24f5` | 2026-11-10 00:20 |
| 基线 -> v0.9.0 提交数 | `https://api.github.com/repos/ggml-org/llama.cpp/compare/8e5a58df87ab59dfceb5b961ae26691e39f30c24...cd959bae99ff4bed22bef2604f45241c09bbb8c8` | 2026-11-10 00:19 |
| v0.9.0 release 说明 | `https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/v0.9.0` | 2026-11-10 00:24 |
| 逐文件提交列表 | `https://api.github.com/repos/ggml-org/llama.cpp/commits?path=<文件>&since=2026-10-24T00:00:00Z&per_page=100`（对 `src/llama-model.cpp`、`src/llama-model-loader.h`、`src/llama-model-loader.cpp`、`ggml/include/ggml-cpu.h`、`ggml/src/ggml-cpu/ggml-cpu.c`、`ggml/src/ggml-cpu/ops.cpp`、`ggml/include/ggml.h`、`ggml/src/ggml.c`、`ggml/src/ggml-backend.cpp`、`src/llama-graph.cpp`、`src/llama.cpp`、`CMakeLists.txt`、`src/CMakeLists.txt`、`examples/CMakeLists.txt` 各查一次） | 2026-11-10 00:13 - 00:18 |
| 单个提交的 diff / 文件清单 | `https://api.github.com/repos/ggml-org/llama.cpp/commits/<sha>`（`a4699400` `701fd25b` `12ab5979` `0ac1a7fd` `e95426f1` `c0ab6e5c` `8642541b`） | 2026-11-10 00:21 - 00:31 |

查询限制说明：未认证 API 每小时 60 次，本次共用掉约 40 次；`compare` 的 `commits` 数组最多返回 250 条（本次 376 条里有 250 条带完整信息，剩下 126 条只体现在 `ahead_by` 里），所以"某个提交是否在基线之后"是靠逐文件查询确认的，不是靠 compare 列表。

`raw.githubusercontent.com` 在本机被 TLS 拦掉，所以没能把上游文件拉下来做离线 `diff`；patch 点判断依据是 GitHub API 返回的 hunk 头 + fork 源码里的实际行号。

---

## 2. 版本对照

| 项 | fork（当前） | 上游 v0.9.0 | 上游 master HEAD |
|---|---|---|---|
| 提交 | `8e5a58df87ab59dfceb5b961ae26691e39f30c24` | `cd959bae99ff4bed22bef2604f45241c09bbb8c8` | `a4d4f2fd4ec85ec6a0a9ece4870416c1b6cc24f5` |
| 提交时间 (UTC) | 2026-10-24T04:05:05Z | 2026-11-04T12:06:26Z | 2026-11-09T13:48:32Z |
| 标题 | vulkan: ggml src1 precision support (#30656) | llama.cpp : bump version to 0.9.0 (#31646) | ggml-zdnn: accept non-zdnn tensors, invalidate compute tensors after every op (#31824) |
| `LLAMA_VERSION` | 0.7.0-dev | 0.9.0（release 发布于 2026-11-04T13:08:06Z） | 0.9.0-dev（HEAD 上还没有 0.10.0 的 bump 提交；`-dev` 由 `LLAMA_BUILD_IS_DEV` 默认 ON 决定） |
| `GGML_VERSION` | 0.27.0 | 0.29.0 | 0.29.0 + 若干提交 |
| nightly | 推断 b1185x | b12110 | b12228 |

版本号落后的两个 minor 分别对应：

| 版本 | bump 提交 | 时间 (UTC) |
|---|---|---|
| v0.8.0 | `8642541b255bfb5dc39cd351a178be09013a55d6` | 2026-10-28T11:30:38Z |
| v0.9.0 | `cd959bae99ff4bed22bef2604f45241c09bbb8c8` | 2026-11-04T12:06:26Z |

---

## 3. 差距量化

| 指标 | 数值 | 依据 |
|---|---|---|
| 落后提交数 | **376** | compare `ahead_by = 376`（`total_commits = 376`） |
| 落后天数 | **16.4 天**（16 天 9 小时 43 分） | 2026-10-24T04:05:05Z -> 2026-11-09T13:48:32Z；到本文写作时（2026-11-10 00:20）约 16.8 天 |
| 其中到 v0.9.0 | 256 个提交 | compare `baseline...cd959bae` 的 `ahead_by = 256` |
| 其中 v0.9.0 之后 | 120 个提交 | 376 - 256 |
| nightly build 差距 | **约 118 个**（b12110 -> b12228） | 两个 nightly 号取自已知事实（v0.9.0 = b12110，HEAD = b12228） |
| fork 基线 nightly | **约 b1185x（b11840 - b11865）** | 推断值：b12110 - 256 = b11854。校验：v0.9.0 -> HEAD 是 120 个提交但 nightly 只涨了 118（有 2 次提交没单独出 nightly），所以按提交数反推的误差在 ±3 以内，取 ±10 的保守区间 |
| 上游提交速率 | 约 22.9 提交/天 | 376 / 16.4 |
| 代码规模差 | fork patch 约 8 文件 / ~230 行，对 376 个提交 | 见 `PATCHES.md` |

> 再次强调：**b1185x 是推断**，没有实际查基线当天的 nightly tag（tag 列表接口拿不到每个 tag 的提交时间，逐 tag 查会吃掉整个 API 配额）。引用时请写"约 b1185x"，不要写成精确 b 号。

---

## 4. fork patch 点的上游变动热度

按 `GET /commits?path=` 查到的基线之后提交数（2026-10-24T00:00:00Z 起）：

| 上游文件 | 提交数 | 说明 |
|---|---|---|
| `ggml/include/ggml-cpu.h` | **0** | fork 在这里只加了一行 `ggml_cpu_set_node_residency_cb()` 声明 -> 不会被冲突，但也不保证以后不被 `ggml-cpu.h` 的其它重构带走 |
| `src/llama-model-loader.h` | **0** | `llama_tensor_weight::offs` / `file_id()` / `no_alloc` / `use_mmap` / `lazy` 语义全部稳定，fork 登记权重依赖的就是这几个字段 |
| `examples/CMakeLists.txt` | **0** | `diskc/diskrun/diskui` 的 `add_subdirectory` 不受影响 |
| `src/CMakeLists.txt` | 1 | 只有 `c0ab6e5c`（加 pthread 链接），改的是 `target_link_libraries`，不碰源文件列表 |
| `ggml/src/ggml-cpu/ggml-cpu.c` | 4 | `701fd25b` `68d20b88` `feaf8000` `12ab5979` |
| `ggml/include/ggml.h` | 6 | 全是新算子/新类型/文档注释，fork 不 patch 这个文件 |
| `ggml/src/ggml-cpu/ops.cpp` | 9 | 含 `12ab5979`（conv/im2col 重写）、`701fd25b`（add_id 的 -1） |
| `src/llama-graph.cpp` | 21 | fork 不 patch 这个文件，但 MoE 图形态在这里变了（见 6.2） |
| `src/llama-model.cpp` | 20 | 含 `a4699400`（no_alloc 搬家），只 1 个跟 patch 点相关 |
| `CMakeLists.txt`（根） | 4 | 2 个版本号 bump + Windows manifest + WASI |
| `ggml/src/ggml-cpu/ggml-cpu.cpp` | ≥1 | `12ab5979` 顺带改了 `supports_op`（未单独按路径查，从该提交的文件清单确认） |

**好消息**：fork 的 14 处 patch 点（5 个被改的上游文件 + 新增文件）里，唯一"上游连一行都没动"的两个文件恰好是最脆弱的两个——`ggml-cpu.h` 的回调声明和 `llama-model-loader.h` 的权重偏移语义。也就是说这次 376 个提交里，**没有一处是上游直接删掉了 fork 依赖的符号**。

---

## 5. 受影响提交分级

级别定义：
- **必须适配**：不处理就功能错误、MoE 快路径失效、或明显超内存预算
- **建议跟进**：不处理能跑，但会跟上游越差越远，或影响后续适配成本
- **无影响**：确认过不碰 fork 的路径

### 5.1 必须适配

| 提交 | 时间 (UTC) | 一句话 | 为什么 |
|---|---|---|---|
| `a4699400e3ccca756297ef4433eb1d7998fca525` | 2026-10-26T14:21:25Z | `llama : move no_alloc out of llama_hparams (#31141)` | 删掉 `llama_hparams::no_alloc`，改由 `llama_model_params::no_alloc` + 新增 `llama_model::no_alloc()` 表达；`llama-model.cpp` +24/−2，`llama-hparams.h` −1，`llama.cpp` −1。fork 用的是 `ml.no_alloc`（loader 级，未受影响），但 `llama-model.cpp:2480` / `:2535` 这两处上游会用 `no_alloc()` 覆盖，patch 上下文会漂 |
| `701fd25b3a55a90297dec0d925204b29084054c8` | 2026-10-24T10:15:36Z | `ggml : allow MUL_MAT_ID -1 index to skip the computation (#26631)` | 给 `MUL_MAT_ID` / `ADD_ID` 定义 `id == -1` 语义（跳过该 slot，结果行置零）。**fork `llama-disk.cpp:1025` 目前把 `id < 0` 判为 `"expert id out of range"` 并整体回退到整张量读取** -> 一旦上游图里出现 -1（见 `e95426f1`），MoE 快路径直接失效，退化成读全量专家 |
| `12ab5979869aecdf3593de260a99ca91263bf5a1` | 2026-11-08T09:44:00Z | `ggml : switch to F32 src1 in the CPU backend (#31854)` | `type_traits_cpu[F16].vec_dot_type` 从 `F16` 改成 `F32`，`ggml_graph_plan` 里 F16 权重 + F32 src1 的 src1 计算缓冲从 `fp16` 变成 `float`（**2 倍**），并新增 `ggml_vec_dot_f16_f32`；`ops.cpp` 的 conv/im2col/conv_transpose 一起重写。对 839 MB 设备是实打实的内存预算变化 |
| `e95426f16982054c379ca7bdc8186996dd22f0fe` | 2026-11-07T16:20:58Z | `llama_moe_cache: run missed experts on CPU, add tuning script (#30966)` | 每个 MoE 层从 1 个 `MUL_MAT_ID` 变成 **2 个 `MUL_MAT_ID` + 1 个 `ggml_add`**（device 缓存那份 + CPU miss 那份），CPU 那份的 ids 里含 `-1`。fork 的 `moe_window` 以权重张量为 key，同一个权重在一次前向里会被两个不同 id 集合访问 |

### 5.2 建议跟进

| 提交 | 时间 (UTC) | 一句话 | 说明 |
|---|---|---|---|
| `c4fb579695320929b6a453f4f3a56951bda332ee` | 2026-11-06T09:32:54Z | `moe-cache : LRFU/MQ-like hybrid cache, ClassLRU (#31233)` | 与 `e95426f1` 同一条特性链（专家缓存策略），`llama-moe-cache.cpp` 大改；fork 不 patch 这个文件，但要跟就跟整条链 |
| `cf34d311332e362198f0a410c466d1e940287d02` | 2026-11-08T13:29:11Z | `moe-cache : ensure CPU part does not wait for the cached experts (#31888)` | `llama-graph.cpp` +3，只是图依赖顺序，配合 `e95426f1` 一起 cherry-pick |
| `68d20b884c3878217de79eade0a249efde96ab38` | 2026-10-29T17:14:37Z | `Quant: OCP FP8 E4M3 support (#27512)` | 新 tensor type `F8_E4M3` + 新 ftype，`ggml.h`/`ggml-cpu.c`/`ops.cpp`/`llama-model.cpp`/`llama-model-loader.cpp` 全链。fork 的 `register_tensor()` 按 `ggml_nbytes()` 处理、类型无关，能白拿；但要确认 armv7 上 FP8 反量化被编进去（NEON 路径） |
| `1b6935b227b9f9aa89deea8016c0e208f547446f` | 2026-11-02T16:13:33Z | `llama : flash_attn_ext_rows - more efficient unified kv for multi-seq (#29510)` | 新算子 `FLASH_ATTN_EXT_ROWS`。fork 的回调是按节点触发的，算子无关，所以不会挂；但它改的是 unified KV 的图形态，回归用例要重跑 |
| `d6be065572cdb7bf64014c8c5771e210c9570e44` | 2026-11-08T07:37:46Z | `llama, mtmd: follow the reference precision of ggml_prec_set_acc calls (#31851)` | 和 `12ab5979` 配对（精度设置跟随），单独 pick 前者数值会飘，建议成对拿 |
| `cd959bae99ff4bed22bef2604f45241c09bbb8c8` + `8642541b255bfb5dc39cd351a178be09013a55d6` | 2026-11-04 / 2026-10-28 | 版本号 bump 到 0.9.0 / 0.8.0 | 升级时必须同步改 `CMakeLists.txt` 的 `LLAMA_VERSION_*`（fork 现在是 0.7.0-dev）和 `ggml/CMakeLists.txt` 的 `GGML_VERSION_*`（0.27.0） |

### 5.3 已确认无影响（含容易误判的几个）

| 提交 | 时间 (UTC) | 一句话 | 判断依据 |
|---|---|---|---|
| `0ac1a7fd35c127a18c0cbc6c57d12e256a6a385b` | 2026-10-29T11:22:43Z | `llama : reuse the tensor staging buffer and skip the direct-io bounce buffer (#31321)` | 改动只在 `llama_model_loader::load_all_data()`（+22/−6，单文件）。**磁盘模式在 `llama-model.cpp:2065` 已经 `return true`，根本不进 `load_all_data()`**；而 fork 自己的读盘路径早就是同样的"对齐窗口 + arena 内 envelope"（`llama-disk.cpp:556-591` 算对齐区间、`:696` 发起读）。属于"上游把 fork 的思路在原生路径上做了一遍"，对 fork 零影响，可作为对照确认 fork 的对齐逻辑没写反 |
| `c0ab6e5c0bab3fb21e6cb9d2371f3c41d85ea775` | 2026-11-07T06:43:50Z | `llama : add missing link to pthread (#31785)` | 只改 `src/CMakeLists.txt` 的 `target_link_libraries(llama PUBLIC ggml PRIVATE Threads::Threads)`；fork 改的是同文件 `:19` 的源文件列表，hunk 不重叠。Android bionic 自带 pthread，静态链接无需额外 `-lpthread` |
| `806b3c2bed44490233f1e8ffa403519370965fe7` | 2026-11-09T12:40:17Z | `ggml-backend: don't upload inputs that are not read (#31419)` | 权重不走 `ggml_backend_tensor_set()`（fork 直接写进 arena 的 `t->data`），dummy buffer（0 字节）语义未变；以后若改成 `buffer_from_host_ptr` 再复查 |
| `e0f96a1a12217d1fab1f281ebc34faca2550c6b9` | 2026-11-05T14:41:03Z | `llama: refactoring: move model to class member of llm_graph_context (#31733)` | fork 不 patch `llama-graph.cpp` |
| `feaf800060a6633e0ae99b835a260fd5d7d4865f` | 2026-11-04T09:11:52Z | `ggml : Gate RVV code on the target extension (#31607)` | RISC-V only |
| `a2b301831d05b07860de508d229d430207127802` | 2026-11-08T06:11:07Z | `speculative : do not apply the MoE cache to the draft model (#31497)` | fork 不用投机解码 |
| `53eb66943217eb32fb2b3b6240d0320d320c1768` | 2026-11-09T08:10:59Z | `opencl: add f32 MUL_MAT_ID (#31783)` | 非 CPU 后端 |
| `4d71b42f` / `de5122d8` | 2026-10-26 / 2026-10-27 | `ggml-cpu : rewrite concat as one row kernel (#31179)` / `ggml : relax scale to non-contiguous views (#31146)` | 只影响 CONCAT / SCALE 的性能与形状检查，两个算子都不读权重 |
| `6023692b` `a8fad21e` `fb599727` `ab3768cd` `ee17f293` `0138937e` `7a2e5aba` `1fa7f1c4` `27b7a8a0` `d78f40d9` `961c376a` `30322097` | 10-24 ~ 11-09 | KV cache / RoPE / 各新模型架构 / MTP | 全部在 `llama-graph.cpp`、`llama-kv-cache*`、`llama-model.cpp` 的建图层，不碰权重加载与 CPU 图执行循环 |

---

## 6. 关键提交详解

### 6.1 `a4699400` no_alloc 搬家（最容易被低估）

上游把 `no_alloc` 从 `llama_hparams` 搬到了三处：

- `llama_model_params::no_alloc`（`llama.h` 里本来就有）
- 新增 `llama_model::no_alloc()`（`llama-model.h`，实现是 `return params.no_alloc;`）
- 新增 `llama_memory_params::no_alloc` + `llama_memory_bufs`（KV cache 也能"只模拟分配"，用 dummy buffer 量内存）

对 fork 的影响：

1. fork 在 `llama-model.cpp:1612-1617` 只设 `ml.no_alloc = true`（loader 级），**没有**设 `params.no_alloc`。所以 `hparams.no_alloc` 在磁盘模式下一直是 `false`，`print_info()` 会打印 `no_alloc = 0`（这是现状，不是这次升级引入的）。
2. 升级后 `hparams.no_alloc` 这个字段没了，`:2480` 的 `if (hparams.no_alloc)` 和 `:2535` 的日志会变成 `no_alloc()`。fork 的 patch 上下文行会跟着漂，但**语义不变**。
3. **不要顺手把 `params.no_alloc` 打开**。`a4699400` 之后 `params.no_alloc` 会一路传给 `create_memory()`，KV cache 也会走 dummy buffer 分支（不真分配）——直接推理就崩。磁盘模式仍然只设 `ml.no_alloc`。
4. 附带问题：`llama_model::memory_breakdown()` 是按 `hparams.no_alloc`（升级后 `no_alloc()`）分支的（`src/llama-model.cpp:2477-2494`）。磁盘模式下这个值是 `false`，于是走 `else` 分支按 `ggml_backend_buffer_get_size()` 累加，而权重 buffer 是 0 字节 dummy -> **权重内存被算成 0**。不会崩（那行 `GGML_ASSERT` 上游已经注释掉了），但任何基于它的内存估算/日志在磁盘模式下都是错的。

### 6.2 `701fd25b` + `e95426f1`：MoE 快路径的真正威胁

`701fd25b` 给 `MUL_MAT_ID` 定义了 `-1`：

```c
assert(i02 == -1 || (i02 >= 0 && i02 < n_as));
if (i02 == -1) { memset(dst 行, 0, ...); continue; }
```

`e95426f1` 之后 `llama-graph.cpp` 的 MoE 前向变成：

```
experts = MUL_MAT_ID(device 缓存张量, cur, slots)        // 命中的专家
experts = ggml_add(experts, MUL_MAT_ID(w, cur, cpu_ids)) // miss 的专家在 CPU 上补算
```

`cpu_ids` 是 `get_rows(cpu_map, ids)`，不在 CPU 那份里的 slot 就是 `-1`。也就是说：**只要用户打开 `moe_cache_size`，图里就会出现带 -1 的 `MUL_MAT_ID`**。

fork 现在的处理（`llama-disk.cpp:1015-1030`）：

```cpp
const int32_t id = ...;
if (id < 0 || id >= n_expert) return no("expert id out of range");   // -> 整张量回退
```

后果不是算错（整张量都读进来了，结果仍然正确），而是**每个 MoE 节点都要读全量专家矩阵**，I/O 直接退化到稠密模型水平，`LLAMA_DISK_MOE=1` 白开。修复方向很明确：

1. `moe_try_node()` 里允许 `id == -1`：不参与 `tls_moe_sel` 去重，`tls_moe_remap` 里保持 `-1`；
2. 读盘时只读被选中的 `n_sel` 个专家切片（已有逻辑不变）；
3. 把 `-1` 原样写进 compact ids，让上游 kernel 自己 `memset` 置零；
4. 统计里单独计数 `skipped_ids`，方便判断是不是上游图变了。

顺带注意：`e95426f1` 让同一个权重张量在一次前向里被两个 `MUL_MAT_ID` 节点访问（id 集合不同）。fork 的 `moe_window` 是 `unordered_map<ggml_tensor*, moe_window>`，节点执行完就 `moe_release()` 擦掉，所以能跑，但**同一层会读两遍不同子集**，MoE 快路径的收益要重新测。

### 6.3 `12ab5979` F32 src1：低内存设备的隐形开销

改动分三块：

1. `type_traits_cpu[GGML_TYPE_F16].vec_dot_type`：`F16` -> `F32`，新增 `ggml_vec_dot_f16_f32()`（`vec.cpp` +79 行）；
2. `ggml_graph_plan` 的 work buffer 估算：F16 权重 + F32 src1 时 `cur += sizeof(float)*ne10*ne11`（原来是 `sizeof(ggml_fp16_t)*...`），`MUL_MAT_ID` 分支同理 -> **src1 缓冲翻倍**；
3. `ops.cpp` 的 conv / im2col / conv_transpose 全部按 F32 src1 重写。

对 fork：

- fork 装上 residency 回调后**强制关闭算子融合**（`ggml-cpu.c:3091`，`if (ggml_cpu_disable_fusion || cplan->use_ref)`），所以 `ops.cpp` 里那部分融合相关重写 fork 看不到；剩下的 conv / im2col / conv_transpose 是音频/多模态模型的算子，纯文本推理（diskrun / diskui / llama-completion）用不到；
- 但 `ggml_graph_plan` 的缓冲估算跟融合无关，**src1 翻倍是实打实的**。839 MB / 176 MiB arena 的手表上，图计划缓冲本来就在 `MemAvailable - 220 MiB` 的预算里挤，升级前必须实测；
- 数值会变（`src1` 不再量化到 F16），所以"升级前后 token id 逐字一致"这种跨版本对比会失败——**回归要改成同一次构建内 `LLAMA_DISK_STREAM_MB=0` vs 流式对比**（fork 现有测试就是这么做的，`test/run-tests.ps1`）。

### 6.4 `0ac1a7fd` 与 fork 的关系（不是威胁，是对照）

上游在 `load_all_data()` 里做的事：按 `file->read_alignment()` 把 `offs` 向下对齐、读到临时 buffer、从 `pad` 处取数据，省掉 direct-io 的 bounce buffer。

fork 在 `llama-disk.cpp` 里早就是这个做法（`dt.delta` / `read_off` / `read_len` / arena 内 `align_up` envelope，`:556-591`、`:696`）。区别是 fork 的 buffer 就是 arena 里的对齐块，没有额外分配。这条**不需要跟**，但可以作为"上游认可了这条路径"的证据写进设计说明。

---

## 7. 升级策略

### 7.1 三条路线

| 路线 | 目标 | 成本 | 风险 | 适用 |
|---|---|---|---|---|
| A. 继续 pin | 保持 `8e5a58d`，只手工挑单点修复 | 最低 | 每天 +23 个提交，patch 上下文漂移，越晚越贵；`llama-model.cpp` 已经 20 个提交 | 只想发版、不打算跟 MoE |
| **B. 跟到 v0.9.0（推荐）** | pin `cd959bae99ff4bed22bef2604f45241c09bbb8c8` | 256 个提交，含 `a4699400` `701fd25b` `68d20b88` `1b6935b2` `0ac1a7fd` + 两个版本号 bump | 中等：只有 -1 语义要适配，**不含** `e95426f1` 的双 `MUL_MAT_ID`、**不含** `12ab5979` 的 F32 src1 | 现在 |
| C. 跟到 master HEAD | pin `a4d4f2fd...` | 376 个提交 | 高：要一次性吃下 `12ab5979` + `c4fb5796`/`e95426f1`/`cf34d311` 的 MoE 图改造 | MoE 快路径稳定后 |

**选 B 的硬理由**：`12ab5979`（11-08）和 `e95426f1`（11-07）都在 v0.9.0 之后。跟到 v0.9.0 能拿到 0.9.0 的 release 说明、ggml 0.29.0、以及 `701fd25b` 的 -1 语义（为将来的 MoE 适配铺路），同时把两个最高风险项推迟到下一轮，可以分开验证。

### 7.2 操作流程（fork 是 zip 快照，没有 git 历史）

现在 `src/llama.cpp-fork/` 不是 git 仓库，每次升级只能"重新解压 + 手工套 patch"。建议借这次升级把流程固定下来：

1. **先建 patch 序列**：在 `src/llama.cpp-fork/` 里 `git init` + 以基线为第一个提交，把 fork 的 14 处 hunk 落成一个 `0001-disk-streaming.patch`（`git diff` 生成）。以后 `git apply -3` / `git rebase` 才有东西可依。
2. **取上游**：下载 `https://github.com/ggml-org/llama.cpp/archive/cd959bae99ff4bed22bef2604f45241c09bbb8c8.tar.gz`，解到新目录，不要覆盖现有 fork。
3. **迁移 fork 文件**：`src/llama-disk.{h,cpp}`、`examples/diskui|diskrun|diskc` 是纯新增，直接拷。
4. **套 14 处 patch 点**（见 `PATCHES.md` 的行号表），逐个文件 `diff -u` 对照，别整体 `patch -p1`。
5. **改版本号**：`CMakeLists.txt` 的 `LLAMA_VERSION_{MAJOR,MINOR,PATCH}` 跟上游（0.9.0），`ggml/CMakeLists.txt` 的 `GGML_VERSION_*` 跟上游（0.29.0）。
6. **编译两 ABI + 跑 checklist**（下一节）。

节奏建议：**每 2-4 周跟一次**（上游 ~23 提交/天，2 周约 320 个提交，4 周约 640 个；超过 4 周 patch 上下文基本要重写）。每次跟完更新 `UPSTREAM.txt` 的 baseline 行和本文档的版本对照表。

### 7.3 优先 cherry-pick / 优先适配顺序

如果暂时不整体升级，只挑单点，按这个顺序：

1. `a4699400`（no_alloc 搬家）—— 越晚越贵，且它改的 `llama-model.cpp` 正是 fork patch 最密集的文件
2. `701fd25b`（-1 语义）—— MoE 快路径的前置条件，单独 pick 没有副作用（只是多允许一种输入）
3. `68d20b88`（FP8 E4M3）—— 纯增量，能白拿新量化格式
4. `12ab5979` + `d6be0655`（F32 src1 + 精度跟随）—— 必须成对，且必须在目标设备上量过内存再合
5. `c4fb5796` + `e95426f1` + `cf34d311`（MoE cache 链）—— 最后做，做完要重写 `moe_try_node()` 的 -1 处理

---

## 8. 升级 checklist

### 编译前

- [ ] 确认目标 commit 的 `LLAMA_VERSION` / `GGML_VERSION`，改好两个 `CMakeLists.txt`
- [ ] 确认 `ggml/include/ggml-cpu.h` 里 `ggml_node_residency_cb` 声明还在（这个文件 0 提交，但重建 patch 时容易漏）
- [ ] 确认 `src/llama-model-loader.h` 里 `llama_tensor_weight::offs` / `file_id()` / `no_alloc` 还在（fork 登记权重依赖这三个）
- [ ] 备份当前可用的 `build/out/<abi>/{disktui,llama-completion}`，用于 A/B 对比和回滚

### 编译

- [ ] `NDK=/path/to/android-ndk ./build/build-android.sh` 两个 ABI 都过
- [ ] armv7 检查：`-mfpu=neon-vfpv4 -mfloat-abi=softfp` 和 `-D_FILE_OFFSET_BITS=64` 都在（README 里那两个坑）
- [ ] `grep -c "ggml_cpu_set_node_residency_cb"` 在两个目标文件里都能查到符号（`nm` 看 `T` 而不是 `U`）

### 功能（必须全过）

- [ ] 启动日志三行齐全：
      `llama-disk: tensors registered ...` / `llama-disk: streaming weights from disk, arena = ... MiB ...` / arena 抬高提示（小 arena 时）
- [ ] `LLAMA_DISK_STREAM_MB=0` 与流式，**同一次构建内**贪心 token id 逐 token 一致
- [ ] `--raw` 与 `--chat` 各跑一轮（chat 模板 + thinking 关闭）
- [ ] 小 arena（如 `--arena 200`）能跑完，不 OOM，不触发 lowmemorykiller
- [ ] arena 装得下整个模型时，第一个 token 之后 `io` 应≈0（验证 LRU 命中）
- [ ] MoE 模型 + `LLAMA_DISK_MOE=1`：日志里 `llama-disk[moe]: nodes = N, experts = M, ... fallbacks = 0`，**fallbacks 必须为 0**
- [ ] 关 O_DIRECT（`LLAMA_DISK_ODIRECT=0`）能跑（对齐路径的兜底）

### 性能 / 内存

- [ ] `io` / `cmp` 取 3 次中位数，跟升级前对比（同模型、同 arena、同 `-t`）
- [ ] 记录 RSS 峰值；`12ab5979` 之后重点看计算缓冲（src1 翻倍），839 MB 设备上必须实测
- [ ] `LLAMA_DISK_VERBOSE=1` 看回调计数增长是否线性（漏节点会表现为计数停滞）

### 文档

- [ ] 更新 `UPSTREAM.txt` 的 baseline / latest / gap 三行
- [ ] 更新本文档的版本对照表与"受影响提交"表（把新 pin 之后查到的提交补进去）
- [ ] 若改了 patch 点行号，同步 `PATCHES.md` 的行号列

---

## 9. 风险与未决项

1. **nightly 号是推断值**（b1185x，±10）。要精确值就得逐 tag 查提交时间，会吃掉整个 API 配额，不值得。
2. **本文档的数据是 2026-11-10 00:10 的快照**。上游 ~23 提交/天，一两天后 `ahead_by` 就会变；引用差距时请重新查 compare。
3. **`no_alloc` 还在搬家过程中**：`llama_hparams` -> `llama_model_params` -> `llama_memory_params`（`a4699400` 一次动了三处）。再跟 2-3 轮，`ml.no_alloc` 这个 loader 级字段也可能被收编，那时 fork 的入口要跟着改。
4. **上游正在把 MoE 往"缓存 + CPU 分流"方向做**（`c4fb5796` / `e95426f1` / `cf34d311` / `a2b30183`），图形态一年内可能再变。fork 的 `moe_try_node()` 依赖 `MUL_MAT_ID` 的 `src[0]` 是权重、`src[2]` 是 ids 这个约定，要持续盯。
5. **`12ab5979` 的 F32 src1 在小内存设备上是负收益风险**：src1 缓冲翻倍 + F16 权重走 F32 点积，armv7 上可能更慢。合之前必须在手表上量，必要时保留 `vec_dot_type = F16` 的分支（但那就变成 fork 私有改动了，要写进 `PATCHES.md`）。
6. **`memory_breakdown()` 在磁盘模式下把权重算成 0**（dummy buffer size = 0，且 `hparams/params.no_alloc` 是 false 走 `else` 分支），不是崩溃，但任何基于它的内存估算都会低估。上游 `--fit` 之类的内存路径在长，值得盯。
7. **没有上游 git 历史**，所有"是否冲突"的判断都是基于 hunk 头 + 行号的静态推断，不是真实 3-way merge 结果。真正合并时仍可能出现上下文漂移导致的 conflict，留出半天时间手工对。
8. **行号是快照**：本文和 `PATCHES.md` 里的行号按 2026-11-10 00:18 的 `src/llama.cpp-fork/` 状态记录。该目录在本分析期间还有并行改动（`src/llama-disk.cpp` 与 `examples/diskui/*` 的 mtime 在 00:02 - 00:18 之间），重建 patch 前请用 `PATCHES.md` 里的特征串（函数名、日志文本）重新定位，不要直接信行号。

---

## 10. 附：本次查询到的完整提交清单（关键部分）

按时间正序，仅列与 fork 相关的：

```
701fd25b  2026-10-24T10:15:36Z  ggml : allow MUL_MAT_ID -1 index to skip the computation (#26631)
74bca395  2026-10-25T01:43:51Z  model: DSV4.1 flash (#31086)
4d71b42f  2026-10-26T14:18:45Z  ggml-cpu : rewrite concat as one row kernel for all types (#31179)
a4699400  2026-10-26T14:21:25Z  llama : move no_alloc out of llama_hparams (#31141)
7a2e5aba  2026-10-27T05:53:44Z  llama : remove obsolete n_pad from memory instances (#31200)
de5122d8  2026-10-27T05:55:31Z  ggml : relax scale to non-contiguous views (#31146)
8642541b  2026-10-28T11:30:38Z  llama.cpp : bump version to 0.8.0 (#31309)
0ac1a7fd  2026-10-29T11:22:43Z  llama : reuse the tensor staging buffer and skip the direct-io bounce buffer (#31321)
68d20b88  2026-10-29T17:14:37Z  Quant: OCP FP8 E4M3 support (#27512)
1b6935b2  2026-11-02T16:13:33Z  llama : flash_attn_ext_rows - more efficient unified kv for multi-seq (#29510)
feaf8000  2026-11-04T09:11:52Z  ggml : Gate RVV code on the target extension (#31607)
cd959bae  2026-11-04T12:06:26Z  llama.cpp : bump version to 0.9.0 (#31646)      <- v0.9.0
e0f96a1a  2026-11-05T14:41:03Z  llama: refactoring: move model to class member of llm_graph_context (#31733)
c4fb5796  2026-11-06T09:32:54Z  moe-cache : LRFU/MQ-like hybrid cache, ClassLRU (#31233)
c0ab6e5c  2026-11-07T06:43:50Z  llama : add missing link to pthread (#31785)
e95426f1  2026-11-07T16:20:58Z  llama_moe_cache: run missed experts on CPU, add tuning script (#30966)
d6be0655  2026-11-08T07:37:46Z  llama, mtmd: follow the reference precision of ggml_prec_set_acc calls (#31851)
12ab5979  2026-11-08T09:44:00Z  ggml : switch to F32 src1 in the CPU backend (#31854)
cf34d311  2026-11-08T13:29:11Z  moe-cache : ensure CPU part does not wait for the cached experts (#31888)
53eb6694  2026-11-09T08:10:59Z  opencl: add f32 MUL_MAT_ID (#31783)
806b3c2b  2026-11-09T12:40:17Z  ggml-backend: don't upload inputs that are not read (#31419)
a4d4f2fd  2026-11-09T13:48:32Z  ggml-zdnn: accept non-zdnn tensors, ... (#31824)      <- master HEAD
```
