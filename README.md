# diskinfer Android（1.0.1）

在 Android（Termux / adb shell）上运行远大于可用内存的 GGUF 模型：权重留在硬盘，按计算图节点流式读进一块很小的内存 arena，用完 LRU 换出。
项目背景、原理与历史版本说明见 **[readme_old.md](readme_old.md)**。

> 版本：**diskinfer 1.0.1**。下面的数字除了标注"待测"的，都是在 Android x86_64 模拟器（MuMu，Android 12，8GB RAM，模型在 /data/local/tmp 本地盘）上实测得到的。

## 1.0.1 更新内容

### 极限小 arena

- **arena 小于最大张量时自动进入 tiny 模式**：`token_embd.weight` 输入只读被选中的行（prefill 用 compact 行 gather，不再整表读）；`output.weight` / tied `token_embd.weight` 的输出矩阵按行分块；其它放不下的大矩阵（ffn/attn）也可按行分块。**arena 下限 8 MiB**。
- 实测 arena 下限：Qwen3.5-0.8B-MoE 从 222 MiB → **8 MiB**；Bonsai2 27B 从 365 MiB → **16 MiB**，输出与 2048 MiB arena 逐 token 一致（见下表）。
- TUI 设置页「内存池 (MB)」支持 8..2048：128 MB 以上步进 128，以下减半（128→64→32→16→8→自动）；`0 = 自动`。
- 新增环境变量：`LLAMA_DISK_TINY_ARENA=0` 关闭自动 tiny；`LLAMA_DISK_OUT_CHUNK=<行数>`、`LLAMA_DISK_SPLIT_MM=0/1` 覆盖自动分块策略。

### TUI 设置页

- **Prefill 批量**（新设置项）：一次 prefill 多少个 token，1..256，默认 4。越大 prefill 越快，但峰值 RAM 越高（分块模式下计算缓冲会明显变大，设置页会同步显示）。
- **常驻 / 峰值 RAM 预估**（设置页底部实时显示）：由当前上下文长度、Prefill 批量、内存池、模型共同计算；KV / 递归状态 / 计算缓冲取自 llama.cpp 对临时 context 的真实尺寸报告，arena 与模型元数据为实测值。运行页也会显示同样信息。
- 无效/不匹配的设置会在底部提示；改动 arena 或 MoE 开关会在开始推理时重新加载模型。

### 读盘与寿命

- **读盘缓存自动策略**：模型文件 ≤ 可用内存的 45% 时自动用 buffered IO（page cache 吸收重复的整模型扫描，热起来后不再读闪存），否则用 direct IO（避免 page cache 挤占内存）。`LLAMA_DISK_ODIRECT=0/1` 可手动覆盖；加载日志会写明选择原因。
- 实测（Qwen 0.5B，tiny 8/16 MB arena）：direct 稳定 0.48 s/token；buffered 首次 0.89 s、第二次 0.61 s，之后 **0.17 s/token**（≈2.8×），第二次运行全程 0.16–0.17 s/token；logits 与 direct 完全一致。
- 大模型（Bonsai2 27B 占 7.26 GiB）在 8GB 模拟器上自动选 direct IO；手机上若可用内存足够大，buffered 同样会生效。**手机真机实测待测。**

### 原有的 1.0.1 功能（保持不变）

- 上游 llama.cpp **v0.9.0（`cd959bae`）**；MoE 快速推理（`MUL_MAT_ID` 只读激活专家）；新 TUI（模型选择 / 设置 / 运行页）；本机 `/v1` API（4000 端口、无鉴权、单并发、忙时 429、`--api-only`）；自适应安装器（root/非 root、Termux）；多语言（en / zh-Hans / zh-Hant）；三个 ABI（x86_64 / arm64-v8a / armeabi-v7a）。
- 修复的 bug：安装布局、MoE 快速路径 3 个、取消显示成错误、设置丢失、模型列表重复、TUI 排版、语言被 Termux 环境覆盖、内存池按键无效、无模型引导、版本号写死等。

## 实测数据（Android x86_64 模拟器，8GB RAM）

### 模型运行

| 模型 | arena / 模式 | prefill | decode | 进程 RSS | logits 与基准 |
|---|---|---|---|---|---|
| Qwen3.5-0.8B-MoE Q4_K_M（514 MiB） | 2048 MiB，direct | 5 tok 归属一次运行 | 0.05 s/tok | 632 MiB | 基准 |
| 同上 | **16 MiB，tiny（自动）** | 同基准 | 0.43–0.57 s/tok | 132–146 MiB | 一致（`d62d8f31ce01faed` / `cae974809e29ce8f`） |
| 同上 | **8 MiB，tiny（自动）** | 同基准 | 0.43–0.51 s/tok | 132–139 MiB | 一致 |
| 同上 | 16 MiB，tiny + MoE 快速路径 | 同基准 | 0.36–0.37 s/tok | 133 MiB | 与"2048 + MoE"一致 |
| 同上 | 16 MiB，tiny，buffered | 首次 0.89 / 0.61 s | **0.16–0.17 s/tok** | 132 MiB | 与 direct 一致 |
| Bonsai2 27B Q2_0（7.26 GiB） | 2048 MiB，direct | 2 tok 9.7 s | 7.99 s/tok | 2317 MiB | 基准 |
| 同上 | **16 MiB，tiny（自动）** | 2 tok 22.7 s | 11.28 s/tok | **287 MiB** | 一致（`c6125f5659c3cd11` / `c7e01884c39ecab5`，token 37032） |

说明：
- tiny 模式下每个 token 仍要把整个模型顺序扫一遍，所以 decode 速度取决于闪存带宽，不会因为 arena 变小而变快；变小的只是**内存占用**。
- 模拟器的"闪存"是宿主机文件，速度比手机快；**arm64 / armv7 手机实测待测**。
- 27B 的 buffered IO 在 8GB 模拟器上不会自动开启（模型 > 45% 可用内存），**大内存手机上的收益待测**。

### RAM 预估（`disktui --mem-probe` 实测）

| 模型 | ctx / batch | arena | KV | 递归 | 计算缓冲 | 常驻 | 峰值 |
|---|---|---|---|---|---|---|---|
| Qwen0.8B | 16 / 4 | 995（自动） | 3.0 | 19.3 | 3.9 | 603.6 | 610.5 MiB |
| Qwen0.8B | 64 / 4 | 16 | 3.0 | 19.3 | 11.1 | 105.7 | 119.8 MiB |
| Qwen0.8B | 64 / 16 | 16 | 3.0 | 19.3 | 44.5 | 105.7 | 137.2 MiB |
| Qwen0.8B | 64 / 32 | 16 | 3.0 | 19.3 | 89.0 | 105.7 | 181.8 MiB |
| Qwen0.8B | 4096 / 64 | 995（自动） | 48.0 | 19.3 | 62.4 | 134.7 | 200.2 MiB |
| Bonsai2 27B | 32 / 4 | 995（自动） | 16.0 | 149.6 | 7.0 | 1228.5 | 1238.6 MiB |
| Bonsai2 27B | 32 / 4 | 16 | 16.0 | 149.6 | 11.6 | 249.5 | 264.1 MiB |
| Bonsai2 27B | 32 / 16 | 16 | 16.0 | 149.6 | 46.3 | 249.5 | 283.1 MiB |
| Bonsai2 27B | 4096 / 4 | 16 | 256.0 | 149.6 | 11.6 | 473.5 | 488.1 MiB |

- `常驻 = 进程基础 RSS + KV + 递归状态 + min(arena, 模型大小)`；`峰值 = 常驻 + 计算缓冲 + 批输入/输出缓冲`。
- 计算缓冲随 Prefill 批量增长（tiny 分块模式下尤其明显），设置页会实时显示，请按设备内存选择。
- **手机真机待测。**

## 快速开始

下载 Release 里对应架构的包：

```bash
adb shell getprop ro.product.cpu.abilist
#   有 arm64-v8a     -> diskinfer-1.0.1-arm64-v8a.zip
#   只有 armeabi-v7a -> diskinfer-1.0.1-armeabi-v7a.zip
#   模拟器 / x86_64  -> diskinfer-1.0.1-x86_64.zip

cd 解压出来的目录
./install.sh          # root 走 su 安装；非 root 推到 /sdcard/Download/diskinfer 并给 Termux 一行命令
```

模型放置：
- 有 root：`/data/local/tmp/llm/models/`（`adb push xxx.gguf /data/local/tmp/llm/models/`）
- 非 root：`~/models/`（Termux）或授权后的 `/storage/emulated/0/models/`

然后 Termux / adb shell 里运行：

```bash
diskinfer                                  # TUI：模型选择 → 设置页 → 运行页
diskinfer -m ~/models/x.gguf               # 跳过模型选择
diskinfer --api-only -m ~/models/x.gguf    # 只开 /v1 API（4000 端口）
LLAMA_DISK_STREAM_MB=16 diskinfer          # 直接指定极小 arena（自动 tiny 分块）
diskinfer --mem-probe -m x.gguf -c 64 -ub 8  # 只打印 RAM 预估，不推理
```

TUI 按键：设置页 `↑↓` 选择、`←→` 调整、`Enter` 切换、`s` 开始推理、`c` 返回、`q` 退出；运行页 `s` 开始、`c` 取消/返回、`i` 编辑提示词、`q` 退出。

## `/v1` API

```bash
adb forward tcp:4000 tcp:4000
curl http://127.0.0.1:4000/v1/models
curl http://127.0.0.1:4000/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"x.gguf","messages":[{"role":"user","content":"hi"}],"max_tokens":32}'
```

无鉴权、单并发、忙时 `429`；`--api-only` 可无 TUI 常驻。

## 参数 / 环境变量

| 项 | 说明 |
|---|---|
| `-m PATH` / `-c N` / `-n N` | 模型 / 上下文 / 最大生成 token |
| `-ub N` / `--threads N` | prefill 批量 / 线程数 |
| `--arena MiB` | 权重 arena（8..2048；不指定=自动） |
| `--moe` / `--api` / `--api-only` / `--port N` | MoE 快速路径 / 同时开 API / 只开 API / 端口 |
| `--lang auto\|en\|zh-Hans\|zh-Hant` | 界面语言 |
| `--mem-probe` | 加载模型并打印 RAM 预估后退出 |
| `LLAMA_DISK_STREAM_MB` | arena 大小（0=关闭磁盘流式；空=自动 1/8 RAM） |
| `LLAMA_DISK_ODIRECT` | 1=direct io，0=page cache（默认按模型/内存自动） |
| `LLAMA_DISK_TINY_ARENA` | 0=关闭自动 tiny 模式 |
| `LLAMA_DISK_OUT_CHUNK` / `LLAMA_DISK_SPLIT_MM` | 覆盖输出层/大矩阵的分块行数与开关 |
| `LLAMA_DISK_MOE` | 1=MoE 快速路径 |
| `LLAMA_DISK_VERBOSE` / `LLAMA_DISK_DEBUG` / `LLAMA_DISK_WINDOW_DEBUG` | 诊断日志 |
| `LLAMA_DISK_MLOCK` / `LLAMA_DISK_PREFETCH` / `LLAMA_DISK_LOOKAHEAD` | arena 锁定 / 预读（默认关）/ 预读窗口 |
| `DISKINFER_LANG` / `DISKINFER_MODEL_DIRS` | 界面语言 / 模型目录（冒号分隔） |

## 已知限制

- arena 小于模型时，每个 token 都要把模型完整读一遍：速度取决于闪存带宽，tiny arena 只省内存、不省 IO。
- tiny 分块会多出窗口/分块 pread 与 barrier 开销，速度通常与普通小 arena 持平或略慢；arena 装得下整个模型时与 llama.cpp 原生 CPU 模式相当。
- Prefill 批量在 tiny 模式下会显著增加计算缓冲（实测 ub16 → +44.5 MiB，ub32 → +89 MiB），小内存设备请保持 1..8。
- 非 root 无法设置 oom 保护，长推理可能被系统回收。
- armv7（32 位）无法寻址 ≥ 4 GiB 的 GGUF。
- 桌面平台 native 默认启用 repack，与流式对照时 token 可能不同（浮点累加顺序，不是 bug）；Android 目标构建为 baseline。

## 待测

- arm64-v8a / armeabi-v7a 真机的 tiny arena 速度与 buffered IO 收益。
- 27B 模型在 ≥ 12 GB RAM 手机上的 buffered IO 效果。
- 不同厂商闪存（UFS 2.x/3.x、eMMC）下的 decode 速度与寿命影响。

## 卸载

```bash
adb shell rm -rf /data/local/tmp/llm /sdcard/Download/diskinfer
# Termux 内：
rm -rf ~/llm
rm -f $PREFIX/bin/diskinfer $PREFIX/bin/termfont
# root 还要删开机脚本
adb shell su -c 'rm -f /data/adb/service.d/99-disktui.sh'
```

**欢迎让你的老设备爆发潜力!**
