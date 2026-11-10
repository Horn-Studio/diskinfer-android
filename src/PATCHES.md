# 相对上游 llama.cpp 的改动清单（v0.9.0 基线）

基线：`ggml-org/llama.cpp` tag **v0.9.0** = `cd959bae99ff4bed22bef2604f45241c09bbb8c8`
（2026-11-04，nightly b12110，ggml 0.29.0）；fork 版本号 **0.9.0-dev**，见 `src/UPSTREAM.txt`。
本文件在三个仓库（diskinfer / diskinfer-linux / diskinfer-windows）内容一致。

> **源码树已经是应用补丁后的状态，日常构建不需要再打 patch。** 完整补丁只用于把一份干净的
> 上游 v0.9.0 树恢复成 fork：`patch/disk-stream-v090.patch`
> （本机同步产物：`F:\work\v090\patch\disk-stream-v090.patch`）。
>
> - 大小 **279,660 B**；**20 个文件**；`7724 insertions(+), 3 deletions(-)`
> - SHA256 `355E167A69EF49F1D65B3D77FF34D5854EAFC3840C1252B25964C36C86E102C5`
> - 已重放验证（2026-11-10）：在干净 v0.9.0 树上 `git apply --check` / `git apply` 均通过；
>   应用后与 canonical 源码树 **4068/4068 个文件 MD5 全同（0 差异）**。
>
> 旧基线 `0.7.0-dev @ 8e5a58d` 时代的逐提交跟踪分析见 `src/UPSTREAM-TRACKING.md`
> （仅 Android 仓库保留，**历史存档**，其中“当前落后 376 个提交”等结论已被本文件取代）。

## 1. 文件清单（相对 pristine v0.9.0）

| # | 文件 | 状态 | 规模 | 说明 |
|---|---|---|---|---|
| 1 | `examples/CMakeLists.txt` | 修改 | +3 | 注册 `diskc` / `diskrun` / `diskui` 三个子目录 |
| 2 | `examples/diskc/CMakeLists.txt` | 新增 | +6 | 常驻 JSON-lines 对话引擎 |
| 3 | `examples/diskc/diskc.cpp` | 新增 | +611 | `diskc`：stdin/stdout 行协议，GUI 对话模式使用 |
| 4 | `examples/diskrun/CMakeLists.txt` | 新增 | +5 | 最小测试驱动 |
| 5 | `examples/diskrun/diskrun.cpp` | 新增 | +257 | `diskrun`：贪心 token id 对比；含 `-no-extra-bufts` 与 repack 启动行 |
| 6 | `examples/diskui/CMakeLists.txt` | 新增 | +87 | 新 TUI 构建（diskui/engine/api/i18n）；含 `DISKINFER_UI_VERSION` 编译期注入（默认 `dev`） |
| 7 | `examples/diskui/api.cpp` | 新增 | +405 | OpenAI 兼容 `/v1` API（端口 4000） |
| 8 | `examples/diskui/api.h` | 新增 | +43 | API 服务接口 |
| 9 | `examples/diskui/diskui.cpp` | 新增 | +2275 | 三端统一的新 TUI（模型选择/设置/运行页，`--api-only`，i18n，Windows 可移植补丁） |
| 10 | `examples/diskui/engine.cpp` | 新增 | +802 | 引擎：加载模型、单轮生成、取消、实时统计 |
| 11 | `examples/diskui/engine.h` | 新增 | +168 | 引擎/统计结构 |
| 12 | `examples/diskui/i18n.cpp` | 新增 | +858 | en / zh-Hans / zh-Hant 词表 + 平台语言检测；含 `{V}` 版本占位符（展开为标题栏版本） |
| 13 | `examples/diskui/i18n.h` | 新增 | +41 | i18n API（`--lang` / `DISKINFER_LANG`） |
| 14 | `ggml/include/ggml-cpu.h` | 修改 | +5 | `ggml_node_residency_cb` typedef + `ggml_cpu_set_node_residency_cb()` |
| 15 | `ggml/src/ggml-cpu/ggml-cpu.c` | 修改 | +23/-2 | 节点 residency 回调（执行前/后）+ 安装回调时强制关闭算子融合 |
| 16 | `src/CMakeLists.txt` | 修改 | +1 | `llama-disk.cpp` 加入 llama 库 |
| 17 | `src/llama-disk.cpp` | 新增 | +2009 | 磁盘流式引擎 + MoE 专家级快速路径（含 v0.9.0 `-1` ids 语义、MTP/nextn 修复、arena 复用、stats） |
| 18 | `src/llama-disk.h` | 新增 | +68 | 磁盘引擎公开接口（`moe_enabled()` / `llama_disk_stats` 等） |
| 19 | `src/llama-graph.cpp` | 修改 | +28 | 输出层分块（`LLAMA_DISK_OUT_CHUNK`，默认 0=关） |
| 20 | `src/llama-model.cpp` | 修改 | +29/-1 | 磁盘模式加载分支、cleanup、关 extra bufts、权重张量登记 |

合计：**6 个修改 + 14 个新增 = 20 个文件**；`7724 insertions(+), 3 deletions(-)`。
（`gguf-py/**/__pycache__/*.pyc` 等构建垃圾不进入补丁。）

## 2. 改动分组

### 2.1 磁盘流式引擎（新增，核心）

`src/llama-disk.h` / `src/llama-disk.cpp`：

- arena 块分配 + LRU 换出；`pread` / `O_DIRECT` 定位读（Windows 分支 `ReadFile` + `OVERLAPPED`、
  `FILE_FLAG_NO_BUFFERING`，卷不支持时回退 buffered）。
- 节点 pin/unpin；`/proc/meminfo`（Windows `GlobalMemoryStatusEx`）自动定容；`mlock` / `VirtualLock`；
  诊断计数与消息回调。
- **MoE 专家级快速路径**（`LLAMA_DISK_MOE=1`，默认 0）：按 router 选中的专家读取 `ffn_*_exps`
  切片；开启时自动禁用 prefetch；前置检查失败 / arena 不足 / 选中 ids 全为 `-1` 时回退整张量加载。
- **MTP / nextn 0-token 空节点修复**：v0.9.0 的 MTP 空节点不再触发“整张量常驻 arena”，
  避免一次回退把 MoE 快路径永久关掉。
- **arena 复用**：同进程多次加载/切换模型时复用已分配的 arena。
- 新增 `llama_disk_stats` 与 `moe_enabled()`：moe / moe_nodes / moe_experts / moe_expert_bytes 等统计。

### 2.2 模型接入（修改）

`src/llama-model.cpp`：

- `disk_stream` 分支：跳过 mmap、`ml.no_alloc = true`、`lazy.mode = OFF`，并调用 `llama_disk::init()`。
- `make_cpu_buft_list(..., params.use_extra_bufts && !disk_stream, ...)`：磁盘模式不挂 extra bufts，
  即**不做权重重排（repack）**，与 native 的对照需要 `-no-extra-bufts`（见 `diskrun`）。
- `if (ml.no_alloc)` 早退分支：登记权重张量 → `finish_register()` → 直接 `return true`，
  跳过 `load_all_data()` 与 `load_tensors_hadamard()`。
- `~llama_model()` 开头调用 `llama_disk::cleanup()`（同一进程可能多次加载模型）。

### 2.3 图执行回调（修改）

- `ggml/include/ggml-cpu.h`：`typedef void (*ggml_node_residency_cb)(struct ggml_tensor *, bool done);`
  + `ggml_cpu_set_node_residency_cb()`。
- `ggml/src/ggml-cpu/ggml-cpu.c`：
  - 全局回调 + setter；**安装回调时强制 `ggml_cpu_disable_fusion = true`**
    （融合节点不会进入执行循环，其权重永远不会被换入）。
  - 图执行线程的节点循环里，在 `ggml_compute_forward()` 前后各调一次 `cb(node, false/true)`。
  - `ggml_cpu_init()` 保留 `GGML_CPU_DISABLE_FUSION` 环境变量判定，回调非空时也强制置位（兜底）。

### 2.4 输出层分块（修改，实验性，默认关）

`src/llama-graph.cpp`：`llm_graph_context::build_lora_mm()` 开头新增输出层分块矩阵乘——
当 `LLAMA_DISK_OUT_CHUNK > 0` 且 `w` 为 `output.weight`、无 LoRA、张量连续、≥64 MiB、
`cur->ne[1] <= 8` 时，按 `n_in × chunk_rows` 用 `ggml_view_2d` 逐块 `ggml_mul_mat` 再 `ggml_concat`。

默认 `0` = 关闭，**默认路径与上游完全一致**；本 fork 的构建/冒烟均走默认路径。已知限制见 §5。

### 2.5 CMake 注册（修改）

- `src/CMakeLists.txt`：`llama-disk.cpp` 加入 `llama` 目标。
- `examples/CMakeLists.txt`：`add_subdirectory(diskc)` / `diskrun` / `diskui`。

### 2.6 新 TUI / 工具（新增）

- `examples/diskui/`（8 个文件）：三端统一的**新 TUI**——模型选择页 → 设置页（上下文 / 温度 /
  聊天模板 / MoE / API）→ 运行页（进度、tok/s、io/cmp、读盘/换出）；同一进程提供
  OpenAI 兼容 `/v1` API（端口 4000，无鉴权，单推理槽，忙时 429）；`--api-only` 无 TTY 模式；
  i18n（en / zh-Hans / zh-Hant）。`diskui.cpp` 含 Windows 可移植性补丁（`<signal.h>`、`<io.h>`、
  `stream_is_tty()` 走 `_isatty(_fileno())`）。
- **标题栏版本注入**：`examples/diskui/CMakeLists.txt` 提供 `DISKINFER_UI_VERSION` 编译期定义，
  未指定时默认 `dev`；构建脚本分别注入 Android `1.0.1`、Linux/Windows `1.1-preview`，
  `i18n.cpp` 中的 `{V}` 占位符展开为该版本号（三端标题栏一致显示各自产品版本）。
- `examples/diskrun/`：最小驱动（无 TUI）。含 `-no-extra-bufts` / `--no-extra-bufts`，并在启动时
  打印 `extra bufts requested = N, repack active = N`，用于桌面平台“流式 vs native”的公平对照。
- `examples/diskc/`：JSON-lines 常驻对话引擎（PySide6 GUI 的对话模式使用）。

## 3. v0.9.0 适配点（上游改名 / 语义变化）

| 上游提交 / 变化 | 内容 | fork 适配 |
|---|---|---|
| `701fd25b`（2026-10-24） | `MUL_MAT_ID` 的 ids 允许 `-1`（CPU kernel 对 -1 槽位置 0 跳过） | `llama-disk.cpp`：ids 校验接受 `-1`；只有 `>= 0` 的 id 参与去重 / 专家切片读取；重映射时 **`-1` 原样保留**；**全部 ids 都是 -1 时回退整张量加载**。`llama-disk.h` 的 `moe_enabled()` 注释补充 -1 语义 |
| `a4699400`（2026-10-26） | `no_alloc` 从 `llama_hparams` 搬到 `llama_model_loader`（`src/llama-model-loader.h`） | fork 原本就只用 `ml.no_alloc`，**无需改代码**；3-way 合并后编译 + 运行验证通过 |
| v0.9.0 `ggml-cpu.c` | 上游改动集中在 F8_E4M3、RISC-V 宏、`ggml_compute_forward_mul_mat_id` 的 -1 处理；**图执行循环、`ggml_cpu_try_fuse_ops()`、fusion init 的位置未变** | residency 回调插桩原样合并；安装回调强制关 fusion 的语义保持 |
| v0.9.0 `llama-graph.cpp` | `build_lora_mm(w, cur, w_s)` 签名未变 | OUT_CHUNK 分支默认不进入，默认路径零行为差异 |
| MSVC（Windows） | 新 TUI 原本按 POSIX 写 | 3 处最小可移植补丁：`<signal.h>`、`<io.h>`、`stream_is_tty()` |
| 产品版本标识 | 新 TUI 标题栏需要显示各端版本 | `DISKINFER_UI_VERSION` 编译期注入：Android `1.0.1`、Linux/Windows `1.1-preview`；CMake 默认 `dev` |

## 4. 重放 / 升级方法

```bash
# 1) 干净的上游 v0.9.0
unzip llama-v0.9.0.zip -d v090 && cd v090
git init && git config core.autocrlf false && git config core.eol lf
printf '* -text\n' > .git/info/attributes
git add -A && git commit -m "base v0.9.0"

# 2) 应用 fork 补丁（仓库内路径 patch/disk-stream-v090.patch；
#    本机同步产物在 F:\work\v090\patch\disk-stream-v090.patch）
git -c core.autocrlf=false apply --check patch/disk-stream-v090.patch
git -c core.autocrlf=false apply         patch/disk-stream-v090.patch

# 3)（可选）逐文件校验：与 canonical 树 4068/4068 MD5 相同
```

注意：

- Windows 本机 git 全局常带 `core.autocrlf=true`；不关掉时补丁会带 CRLF 上下文，`git apply`
  报 “patch does not apply”。务必 `-c core.autocrlf=false`，补丁本身是 LF。
- 补丁是**相对 pristine v0.9.0** 的完整差异，不是相对旧基线；旧基线的 patch 记录已不适用。
- 本仓库三个目录（src / examples / ggml）已经是应用后的树，**不要再 apply**，否则会冲突。

## 5. 维护 checklist（上游再变时）

1. 重点看：
   - `llama_model::load_tensors()` 的 no_alloc 分支/返回点（磁盘模式登记张量后直接 `return true`，
     跳过 `load_all_data()` 与 `load_tensors_hadamard()`）；
   - `ggml-cpu.c` 的图执行循环（回调必须插在 `ggml_compute_forward()` 前后，且融合必须关闭）；
   - `llama_model_loader` 的 `file_id()` / `get_weight()` / `offs` 语义（补丁依赖）；
   - `llama_model_loader::no_alloc` 字段位置（上游 `a4699400` 之后在 loader 层）。
2. 改完按 README 的「从源码编译」各平台交叉编译一次，核对启动日志三行：
   `tensors registered` / `streaming weights from disk` / `arena =`。
3. MoE 模型冒烟判据：`LLAMA_DISK_MOE=0` 与 `=1` 的贪心 token id 完全一致；`LLAMA_DISK_VERBOSE=1`
   出现 `selected N/28 experts`；`fallbacks` 不增长。
4. 桌面平台对照判据：`native + -no-extra-bufts`（同 repack 布局）与流式的贪心 token id 一致；
   默认 native（repack 开启）与流式不同属预期。
5. 已知不要碰的：`LLAMA_DISK_WINDOW=1`（既有数值问题，未修，建议不用、勿与 MoE 组合）；
   `LLAMA_DISK_OUT_CHUNK`（实验性，默认 0）。
