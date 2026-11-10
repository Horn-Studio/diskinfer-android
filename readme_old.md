# diskinfer-android - 在Android设备上运行远大于可用内存的 GGUF 模型

这是一个基于 llama.cpp 修改而生的项目。用 diskinfer 推理模型时，权重全程留在硬盘，不依靠系统的 swap 和 mmap，推理时按计算图节点把张量读进一块固定大小的内存 arena，用完即按 LRU 换出。代价是每个 token 都要把模型读一遍，运行速度基本正比于磁盘读速度。正因如此，设备可运行的最大模型取决于该模型的最大单张量/层大小。

此项目包含了一个 `disktui`，用于在 Termux 里进行可视化交互（模型选择 / 设置 / 推理），也可以只开 `/v1` API 供其它程序调用。

```
测试数据（仅作参考，实际使用效果以自己手机性能而定）
设备：小天才电话手表 Z8（高通骁龙429w即Wear4100，839MB内存，arm32设备，arena大小给与176MB，Bonsai-27B Q1_0）
  [ 1:28] prompt done: 5 tok in 88.6s (17.7 s/tok), io 37.4s
  [ 0:29] t1  io 17.7s cmp 11.5s tot 29.2s  ' Paris'
  [ 0:58] t2  io 17.8s cmp 11.4s tot 29.2s  '.'
  answer:  Paris.

设备：红米K20 Pro（高通骁龙855，8GB内存，arena大小给与2048MB，Bonsai-27B Q1_0）
  [ 0:41] prompt done: 6 tok in 41.8s (7.0 s/tok), io 25.7s
  [ 0:19] t1  io 12.5s cmp  7.2s tot 19.7s

设备：Android 12 模拟器（x86_64，8GB，arena 2048MB，Ternary-Bonsai-2-27B Q2_0 / 7.1GB）
  5.64 s/token（direct io = 1），token id 与 Windows 主机逐 token 一致
```

## 1.0.1 更新内容

- **上游升级**：llama.cpp `0.7.0-dev @ 8e5a58d` → **v0.9.0（`cd959bae`，2026-11-04，ggml 0.29.0）**，fork 版本号 `0.9.0-dev`。
- **MoE 快速推理（优化 + 修复）**：`MUL_MAT_ID` 节点只读取 router 选中的专家切片，其余专家权重不读盘（专家级流式，不再整张量换入）。
  开关：设置页 `MoE 快速路径`，或 `LLAMA_DISK_MOE=1`（默认关）。
  实测 Qwen3.5-0.8B-MoE（28 experts / top-12）`nodes=762 experts=10668 fallbacks=0`，贪心 token 与整张量加载逐 token 一致。
- **新 TUI**：模型选择页 → 设置页（上下文 / 温度 / 聊天模板 / MoE / **内存池 (MB)** / API）→ 运行页
  （prefill 进度条、生成 tok/s、io/cmp、累计读盘、换出、取消）。提示词由用户自己输入，小屏自动紧凑布局。
- **本机 `/v1` API（4000 端口）**：`/v1/models` 返回模型文件名、`/v1/chat/completions`、`/v1/completions`、`/health`；
  无鉴权、单并发、忙时 HTTP 429；`--api-only` 可无 TUI 常驻。
- **安装器自适应**：电脑上运行问 adb 路径（可拖入），root / 非 root 分流处理；Termux 内直接安装；
  包与脚本全扁平布局，`setup.sh` 兼容旧的 `device/` 布局。
- **多语言**：TUI / 启动器 / 安装器支持 en / zh-Hans / zh-Hant，按系统语言自动识别，`--lang` 或 `DISKINFER_LANG` 覆盖。
- **三个 ABI 包**：`x86_64`（模拟器 / ChromeOS）、`arm64-v8a`（手机 / 平板）、`armeabi-v7a`（32 位老设备）。
- **1.0.1 自身变化**：标题栏显示 `diskinfer 1.0.1`；不再默认 "Hello" 自动推理（提示词自己输入）；
  设置页新增「内存池 (MB)」（128–2048，0=自动）；运行页按键说明改为显式 `s=… c=… q=… i=…`；包内不再附 README。

## 修复的 bug（1.0.1）

1. **安装布局 bug（网上反馈）**：旧 `install.sh` 把 `device/` 推成 `/data/local/tmp/llm/device/…`，而启动器/脚本按扁平路径找 → 装完命令不可用。现在包与安装器全扁平布局（`setup.sh` 同时兼容旧 `device/` 布局并自动挪平），安装后逐文件自检。
2. **MoE 快速路径 3 个 bug**：① ids 是 argsort view、行 stride 含 padding → 按 stride 正确解析；② qwen35 的 MTP/nextn 层 0-token 空节点被整张量读入并常驻 arena → 跳过空节点；③ 一次回退后整张量常驻导致 MoE 永久失效 → 改为可回收复用。
3. **取消显示成错误**：旧版取消后显示 `finish error`，还打 `ggml/llama_decode` 错误日志 → 现在显示 `finish cancelled`，只记一行 `[cancel] user cancelled after N tokens`。
4. **设置跨页/换模型丢失**：ctx、温度、MoE/API 开关会被重置 → 现在会话内保留，超出新模型上限时 clamp 并提示。
5. **模型列表重复**：同一模型出现在多个目录时重复列出 → 按 basename + 大小去重。
6. **TUI 排版**：Answer 内容不在 Answer 分区、设置页底部按键行被截断、换模型后日志混在一起 → 已修（换模型插入 `---- model: xxx ----` 分隔行）。
7. **语言被 Termux 环境覆盖**：Android 检测顺序改为 `persist.sys.locale` → `ro.product.locale` → `LANG/LC_ALL`，Termux 默认 `LANG=en_US.UTF-8` 不再把中文界面顶成英文。
8. **设置页选中「内存池」后按 `s` 无效**（1.0.1 修复）：该设置项的处理函数吞掉了后续按键 → 改为落空处理。
9. **无模型引导缺失**：以前只说"没找到模型" → 现在按 root / 非 root 给出各自放置路径与命令。
10. **安装后版本号不对**：标题栏原来写死 `v5`/`v4` → 改为构建期 `-DDISKINFER_UI_VERSION` 注入，Android 显示 `diskinfer 1.0.1`。

## 默认行为

`load_tensors()` 不再 mmap、也不全量读入，直接走磁盘 streaming。只有一个开关能关掉：

```
LLAMA_DISK_STREAM_MB=0      # 回到原生 llama.cpp 行为（mmap / 全量读入）
```

## arena 大小（自动决定）

| 情况 | arena |
|---|---|
| `LLAMA_DISK_STREAM_MB` 未设置 / 空 | **内存总量 / 8**（启动器注入），引擎自动再按可用内存收窄 |
| `LLAMA_DISK_STREAM_MB=N` | N MiB，夹取到 128 .. 2048 MiB |
| 小于最大单张量 | 自动抬到 `最大张量 + 24 MiB` 并写日志 |
| 按可用内存收窄 | `MemAvailable - 220 MiB`（下限 176 MiB；仅当收窄后仍 ≥ 最大张量+24 MiB 才收窄） |

```
llama-disk: arena 111 MiB cannot hold the largest tensor 'token_embd.weight' (170 MiB), raised to 194 MiB
llama-disk: streaming weights from disk, arena = 194 MiB (raised to fit the largest tensor), direct io = 1, mlock = 1, prefetch = 0 (lookahead 16)
```

注意：`LLAMA_DISK_STREAM_MB=auto` **不是**自动，`atoi("auto")==0` 会关掉流式，请留空。
TUI 设置页的「内存池 (MB)」可以覆盖它（0=自动），开始推理时若改过会重新加载模型。

**关于提速**：arena 小于模型时，因为是顺序扫描 + LRU，命中率≈0，io 时间基本不变（模型大小 ÷ 带宽）。
**arena 装得下整个模型时，第一个 token 之后权重就全在内存里**，此时与 llama.cpp 原生 CPU 模式无异。

## 快速开始

更简单地，你可以直接使用 Agent 工具，将项目地址发给他并为其提供 adb 工具，AI 将自动完成。

下载 Release 里对应你设备架构的包：

```bash
adb shell getprop ro.product.cpu.abilist
#   有 arm64-v8a     -> diskinfer-1.0.1-arm64-v8a.zip
#   只有 armeabi-v7a -> diskinfer-1.0.1-armeabi-v7a.zip
#   模拟器 / x86_64  -> diskinfer-1.0.1-x86_64.zip

cd 解压出来的目录
./install.sh          # 有 root 走 su 安装；没有 root 会推到 /sdcard/Download/diskinfer 并给 Termux 一行命令
```

安装后把模型放到手机：有 root 用 `/data/local/tmp/llm/models/`，没有 root 放 `~/models/` 或授权后的
`/storage/emulated/0/models/`（安装器会按身份提示具体路径）。然后在 Termux 里敲 `diskinfer`。

## 支持的设备

- **架构**：`arm64-v8a`、`armeabi-v7a`、`x86_64`（静态二进制，无 .so 依赖，Android 7+）
- **root**：推荐但非必须。有 root 会 `su` 设 `oom_score_adj=-1000`（小内存设备必须，否则跑几分钟就被 lowmemorykiller 杀掉）
- **Termux**：用于命令入口；也可以直接 `adb shell` 跑二进制
- **模型**：任何 llama.cpp 认识的 GGUF。稠密模型与 **MoE 模型**都能跑；MoE 模型开启快速路径后只读激活专家（详见下）
- **多语言**：en / zh-Hans / zh-Hant，按系统语言自动切换

## 参数

```
diskinfer                                 打开 TUI（模型选择 → 设置页 → 运行页）
diskinfer -m ~/models/x.gguf              跳过模型选择
diskinfer --api-only -m ~/models/x.gguf   只开 /v1 API（端口 4000）
diskinfer -h                              全部参数

-m PATH        指定模型
--arena MiB    权重 arena（见上；不指定就自动）
-n N           最多生成多少 token（默认 256）
-c N           上下文（默认 16；TUI 设置页可改，上限为模型训练长度）
-ub N          prefill 批大小（默认 4；1 = 最省内存、prefill 最慢）
-t N           线程（默认 4）
--temp F       温度（默认 0.70）
--top-k N      默认 40         --top-p F   默认 0.95
--raw          裸补全          --chat      套 chat 模板
--sys TEXT     --chat 用的 system message
--moe          启动时 MoE 快速路径默认开（等价 LLAMA_DISK_MOE=1）
--api          启动 TUI 时同时开 /v1 API
--port N       API 端口（默认 4000）
--lang TAG     界面语言 auto/en/zh-Hans/zh-Hant
```

## TUI

- **模型选择页**：↑↓ 选择、Enter 打开、`r` 重新扫描、`q` 退出；显示模型大小。
- **设置页**：↑↓ 选项、←→ 调整、Enter 切换、`s` 进入运行页、`c` 返回、`q` 退出。
  - 上下文长度（3 .. 模型上限，默认 16）
  - 温度（0.00–1.00，默认 0.70）
  - 聊天模板（默认关；开启时 ctx 自动变成 `16 + 模板额外 token 数`，之后可改）
  - MoE 快速路径（默认关；开启后重载模型）
  - 内存池 (MB)（128–2048，0=自动；改动后开始推理时重载模型）
  - API 服务（默认关；开启后显示 `http://127.0.0.1:4000/v1`）
- **运行页**：Prompt 区（提示词默认为空，由你自己输入）、Progress 区（prefill 进度条 / 生成 tok/s / io / cmp /
  累计读盘 / 换出 / TUI 推理中 / API 推理进度）、Answer 区、Log 区。
  底部固定显示：`s=开始推理  c=取消/返回  q=退出  i=编辑提示词`。
- 完整日志写到 `/data/local/tmp/llm/diskinfer.log`（有 root）或 `~/diskinfer.log`。

## `/v1` API

无鉴权、OpenAI 兼容子集、**同时只跑一个推理**（TUI 自己的推理也算）。忙时返回
`HTTP 429` + `{"error":{"message":"busy","type":"rate_limit_error"}}`。

```bash
adb forward tcp:4000 tcp:4000
curl http://127.0.0.1:4000/health
curl http://127.0.0.1:4000/v1/models
curl http://127.0.0.1:4000/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"x.gguf","messages":[{"role":"user","content":"hi"}],"max_tokens":32}'
curl http://127.0.0.1:4000/v1/completions -H 'Content-Type: application/json' \
  -d '{"model":"x.gguf","prompt":"The capital of France is","max_tokens":16}'
```

- `GET /health`：`{"status":"ok","busy":false,"model":"x.gguf","port":4000}`
- `GET /v1/models`：`id` 就是模型文件名
- `POST /v1/chat/completions`、`POST /v1/completions`：`stream:false`

## MoE 快速路径（`LLAMA_DISK_MOE=1`）

MoE 模型的 `ffn_*_exps` 是三维权重张量 `[embd, ffn, n_expert]`。默认整张换入；
开启快速路径后，每个 `MUL_MAT_ID` 节点只 `pread` router 选中的专家切片：

- 打开方式：设置页 `MoE 快速路径`，或 `LLAMA_DISK_MOE=1`（默认关）
- 实测：Qwen3.5-0.8B-MoE（28 experts / top-12，arena 1024 MiB）
  `nodes=762 experts=10668 expert_read=887MiB fallbacks=0`，逐节点 `selected 14/28 experts`
- 正确性：开/关的贪心 token id 逐 token 一致；非 MoE 模型开启后行为不变
- 限制：MoE 开启时自动禁用预读；`LLAMA_DISK_WINDOW=1` 不要和 MoE 组合

## 也能用普通 llama.cpp 的命令行

Release 里带了 `llama-completion`，就是本 fork 编译出的标准 llama.cpp 工具：

```bash
./llama-completion -m model.gguf -p "The capital of France is" -n 32 --temp 0.3

# 想回到原生行为（模型能装进内存时）
LLAMA_DISK_STREAM_MB=0 ./llama-completion -m model.gguf -p "..." -n 32
```

chat 模板、sampling 预设、`-cnv` 这些原生功能都能用，只是没有 `disktui` 的分屏和逐 token 计时。

## 原理

值得注意的是，此项目原本是用作在小天才电话手表上运行 Qwen3.8 27B 的实验项目，故实现路径与小内存 + armv7a
设备相匹，armv8 设备未做特殊优化，或者说不是该项目的主要受益设备。

---

llama.cpp 默认要么全量读入内存，要么 mmap 全部。这两种在这类设备上都不可行：

1. **mmap 不行**：32 位进程虚拟地址只有 ~3GB，3.5GB 的模型连映射都做不到
2. **全量读入不行**：内存装不下

这里走第三条路 —— **`no_alloc` + 节点级按需换入**：

- 用 llama.cpp 的 `no_alloc` 模式加载：只读元数据，权重一个字节都不读，张量拿 dummy buffer
- 在 ggml CPU 后端的图执行循环里插节点回调（`ggml_cpu_set_node_residency_cb`）
- 每个节点执行前，把它需要的张量从 gguf 里 `pread` 进 arena（默认 `O_DIRECT` 绕 page cache），
  执行后解除 pin；arena 满就按 LRU 换出
- 张量按文件顺序预排序，读盘基本是顺序的
- **MoE 快速路径**（1.0.1 加入）：识别 `GGML_OP_MUL_MAT_ID`，读 `src[2]` 的 I32 专家 ids、去重后只读这些专家切片，
  重映射 ids 并临时改节点输入，`done` 时恢复
- **多语言**（1.0.1 加入）：TUI/引擎日志/安装器/启动器统一词表 en/zh-Hans/zh-Hant

改动的文件（都在 `src/llama.cpp-fork/` 里，已经改好，直接编译）：

| 文件 | 改了什么 |
|---|---|
| `src/llama-disk.h` `.cpp` | **新增**，磁盘 streaming 引擎（arena/块分配/LRU/O_DIRECT/诊断 + MoE 专家级换入） |
| `examples/diskui/` | **新增**，新 TUI + 引擎 + `/v1` API + i18n |
| `examples/diskrun/` | **新增**，最小驱动（不带 TUI，调试用；`-no-extra-bufts` 用于对照） |
| `examples/diskc/` | **新增**，常驻 JSON-lines 对话引擎（GUI 用） |
| `src/llama-model.cpp` | 磁盘模式：跳过 mmap、`no_alloc`、登记权重张量 |
| `src/llama-graph.cpp` | 可选输出层分块（`LLAMA_DISK_OUT_CHUNK`，默认关） |
| `src/CMakeLists.txt` | 加 `llama-disk.cpp` |
| `ggml/include/ggml-cpu.h` | 声明 `ggml_cpu_set_node_residency_cb()` |
| `ggml/src/ggml-cpu/ggml-cpu.c` | 节点执行前后调回调 + 关融合 |
| `examples/CMakeLists.txt` | 加 `diskui` / `diskrun` / `diskc` |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `LLAMA_DISK_STREAM_MB` | **开启（auto 定容）** | arena 大小；`0` = 关闭，回到原生 mmap |
| `LLAMA_DISK_ODIRECT` | 1 | `O_DIRECT`，绕过 page cache；`0` 关掉 |
| `LLAMA_DISK_MLOCK` | 0 | mlock arena，防 zram 换出（`diskinfer` 默认开） |
| `LLAMA_DISK_MOE` | 0 | 1 = MoE 快速路径（只读激活专家） |
| `LLAMA_DISK_PREFETCH` | 0 | 后台预读线程（实测无收益；MoE 开时强制关） |
| `LLAMA_DISK_LOOKAHEAD` | 16 | 预读看几个张量 |
| `LLAMA_DISK_VERBOSE` | 0 | 每 4096 次回调打计数；MoE 逐节点命中日志 |
| `LLAMA_DISK_DEBUG` | 0 | 看门狗 + 读盘诊断 |
| `LLAMA_DISK_WINDOW` | 0 | 行/视图窗口（实验性，有已知问题） |
| `LLAMA_DISK_OUT_CHUNK` | 0 | 分块 logits 行数（实验性） |
| `GGML_CPU_DISABLE_FUSION` | 自动置 1 | 磁盘模式下由 `init()` 设置 |
| `DISKINFER_LANG` | 自动 | 界面语言覆盖（en / zh-Hans / zh-Hant） |
| `DISKINFER_MODEL_DIRS` | 启动器设置 | 模型目录（冒号分隔） |

## 如何自己编译

```bash
# Linux / macOS：NDK=/path/to/android-ndk ./build/build-android.sh         # 三个 ABI
#                 NDK=/path/to/android-ndk ./build/build-android.sh x86_64  # 单个 ABI
# Windows（本仓库实测路径）：
pwsh -File build\build-android.ps1 -Abi x86_64            # 或 -Abi arm64-v8a,armeabi-v7a
```

产物在 `build/out/<abi>/{disktui,diskrun,llama-completion}`，已 strip。
源码就是 `src/llama.cpp-fork/`，不需要打任何 patch（完整 fork 补丁在 `patch/disk-stream-v090.patch`）。
打包 release：`bash build/package-android.sh` → `dist/diskinfer-1.0.1-<abi>.zip`。

两个坑（脚本里已处理）：

- **armv7 必须手动加 `-mfpu=neon-vfpv4 -mfloat-abi=softfp`**，NDK 的 `ANDROID_ARM_NEON=TRUE` 在这个组合下不会真的加 `-mfpu=neon`，漏了就是纯标量，慢 2-3 倍
- **32 位必须加 `-D_FILE_OFFSET_BITS=64`**，否则 `off_t` 是 32 位，>2GB 的文件连 GGUF magic 都读不出来

## 已知限制

---

- 每个 token 都要全部遍历一遍模型自身，因此速度绝对由闪存带宽决定（顺序读 150~250MB/s 很常见，但老旧设备算力也是需要时间的）
- arena < 模型时加大 arena 基本没用（顺序扫描 + LRU 命中率≈0）
- 小心 zram 消耗殆尽，这样会导致 emmc 的读取速度大幅度下降
- 非 root 无法设置 oom 保护，长推理可能在 1–2 GB 设备上被系统回收
- 桌面平台 native 默认启用 repack：与磁盘流式对照时 token 序列可能不同（浮点累加顺序，不是 bug）；Android 目标构建为 baseline，本机实测一致
- `LLAMA_DISK_WINDOW` / `LLAMA_DISK_OUT_CHUNK` 是实验特性，未经过测试，小 arena 的情况下会出问题；不要与 MoE 组合

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
