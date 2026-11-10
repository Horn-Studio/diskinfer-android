# diskinfer 1.0.1 （Android）

把 GGUF 模型放在手机上、**边读磁盘边推理**的 llama.cpp 分支。`disktui` 是主入口；
1.0.1 起三端（Android / Linux / Windows）共用同一套新 TUI（模型选择 / 设置页 / 本机 `/v1` API）
和多语言界面（en / zh-Hans / zh-Hant）。

## 语言 / Language

脚本和 TUI 支持 **English / 简体中文 / 繁體中文**，默认按系统语言自动切换：

- 优先级：`--lang <tag>` > 环境变量 `DISKINFER_LANG` > 系统语言。
- `<tag>` 可用 `auto`、`en`、`zh`、`zh-Hans`、`zh-Hant`、`zh-CN`、`zh-TW`；
  `zh` 开头且含 `Hant`/`TW`/`HK`/`MO` → 繁体，其余 `zh*` → 简体，其它 → English。
- 系统语言来源：Android 读 `getprop persist.sys.locale`（失败回退 `$LANG`）；
  Linux 读 `$LC_ALL`/`$LC_MESSAGES`/`$LANG`；Windows 读 PowerShell UI culture。
- 覆盖范围：`install.sh`、`install.ps1`、`diskinfer`、`run_as_root.sh`、`setup.sh`、`termfont`。
  TUI 本身也接受 `--lang`。

```bash
DISKINFER_LANG=zh-Hant diskinfer -h
diskinfer --lang zh-Hans
sh install.sh --lang en
```

## 1.0.1 新特性

- **多语言**：脚本与 TUI 支持 en / zh-Hans / zh-Hant，按系统语言自动切换，`--lang` / `DISKINFER_LANG` 覆盖。
- **上游同步**：llama.cpp 升级到 v0.9.0（`cd959bae`），磁盘流式引擎与 MoE 快速路径已适配 `no_alloc` 搬迁与 `MUL_MAT_ID` -1 索引。
- **新 TUI 三端一致**：Android / Linux / Windows 共用同一套 `disktui`（模型选择、设置页、运行页、本机 `/v1` API）。
- **MoE 快速路径**：`LLAMA_DISK_MOE=1`（设置页可开，默认关闭），只换入 router 选中的专家；与整张量加载的贪心 token 一致。
- 磁盘流式推理仍默认开启；Android 三个 ABI（arm64-v8a / armeabi-v7a / x86_64）同源构建。

## 选哪个包

| 包 | 用在哪 |
|---|---|
| `diskinfer-1.0.1-arm64-v8a.zip` | 2017 年后的手机、平板、电视盒子（64 位 ARM） |
| `diskinfer-1.0.1-armeabi-v7a.zip` | 32 位 ARM：老设备、智能手表、部分车机 |
| `diskinfer-1.0.1-x86_64.zip` | 模拟器、ChromeOS、Android-x86 等 x86_64 设备 |

不确定就看这一行：

```bash
adb shell getprop ro.product.cpu.abilist
```

输出里**有 `arm64-v8a`** → 用 arm64 包；**只有 `armeabi-v7a`** → 用 armv7 包；
输出里是 `x86_64`（模拟器）→ 用 x86_64 包。

## 包里有什么

```
disktui              本项目的 TUI + 磁盘 streaming 引擎 + /v1 API（静态，strip 过）
diskrun              非交互测试驱动（同一引擎，打印 token id / 统计）
llama-completion     同一份 fork 编译出的标准 llama.cpp 命令行（静态）
setup.sh             设备端安装脚本（Termux / adb root 都走它）
diskinfer            Termux 启动器（PATH 上的命令，找模型后 exec disktui）
termfont             Termux 字号小工具（termfont 8..20）
run_as_root.sh       root 启动器（被 diskinfer 通过 su 调起）
99-disktui.sh        Magisk 开机脚本（root 时由 setup.sh 安装）
install.sh           Linux / macOS / Termux / Git Bash 一键安装
install.ps1          Windows 一键安装（右键 → Run with PowerShell）
README.md            本文件
SHA256SUMS.txt       包内文件校验和（解压后 `sha256sum -c SHA256SUMS.txt`）
```

所有文件都在 zip 根目录（**扁平布局**，没有 `device/` 子目录）。
两个二进制都是 strip 过的静态文件，不依赖任何 `.so`，Android 7+（API 24）直接跑。

**磁盘 streaming 是默认开的**，不用设任何环境变量。只有想回到原生行为时才需要
`LLAMA_DISK_STREAM_MB=0`。

## 安装

先解压，进入目录。安装器会根据当前环境自动选择路径：**在 Termux 里运行 = 直接装到手机；
在电脑上运行 = 通过 adb 装**。

### Windows（电脑 + USB/无线 adb）

右键 `install.ps1` → **Run with PowerShell**（或终端里 `powershell -ExecutionPolicy Bypass -File .\install.ps1`）。
它会问 adb 在哪（可以把 `adb.exe` 直接拖进窗口；回车则用 PATH 里的 adb），然后：

- 有 root：推到 `/data/local/tmp/llm/` 后自动 `su` 跑 `setup.sh`，完事。
- 无 root：把二进制和脚本放到 `/sdcard/Download/diskinfer/`，并打印在 Termux 里要跑的一行：

  ```sh
  sh /sdcard/Download/diskinfer/setup.sh /sdcard/Download/diskinfer/disktui
  ```

### Linux / macOS / Git Bash

```bash
bash install.sh          # 或 ./install.sh
```

同样的交互：问 adb 路径（支持带引号/拖拽/纯 `adb`），驱动 root 或非 root 流程。
无 root 的最后一步同上：在 Termux 里跑 `sh /sdcard/Download/diskinfer/setup.sh /sdcard/Download/diskinfer/disktui`。

### 在 Termux 里（把包拷进手机后）

```bash
cd ~/Download/diskinfer      # 或你解压的位置
sh install.sh
```

直接装：`disktui`/`diskrun`/`llama-completion` 进 `~/llm/`，`diskinfer`/`termfont` 进
`$PREFIX/bin`。有 su 时同时装 `/data/local/tmp/llm/` 那套 root 启动器。
**非 root 不设 oom、不强制 mlock**（root 启动器才会做这些）。

### 手动装（不想跑脚本）

```bash
adb push disktui diskrun llama-completion /data/local/tmp/llm/
adb push setup.sh diskinfer termfont run_as_root.sh 99-disktui.sh /data/local/tmp/llm/
adb shell chmod 755 /data/local/tmp/llm/*
adb shell su -c 'sh /data/local/tmp/llm/setup.sh /data/local/tmp/llm/disktui'
```

注意脚本必须**扁平**地放在 `/data/local/tmp/llm/` 下（v3 曾经错误地推成
`/data/local/tmp/llm/device/…`；`setup.sh` 仍兼容那种旧布局，会自己把文件挪平）。

## 放模型

按身份二选一：

```bash
# root（或 adb shell 直接用）
adb shell mkdir -p /data/local/tmp/llm/models
adb push your-model.gguf /data/local/tmp/llm/models/
# 共享目录（Termux 授权后可读）
adb shell mkdir -p /storage/emulated/0/models
adb push your-model.gguf /storage/emulated/0/models/
# 没有 root / Termux 用户：私有目录，永远可读
cp /sdcard/Download/your-model.gguf ~/models/
```

> Android 11+ 上 Termux 默认读不到 `/storage/emulated/0`。两个办法：
> 1. 在 Termux 里执行 `termux-setup-storage`，或者电脑上：
>    `adb shell appops set com.termux MANAGE_EXTERNAL_STORAGE allow`
>    以及 `adb shell pm grant com.termux android.permission.READ_EXTERNAL_STORAGE`
> 2. 或者把模型放到 Termux 私有目录（永远能读）：
>    `cp /sdcard/Download/x.gguf ~/models/`

启动器按顺序找模型：`/data/local/tmp/llm/models` → `~/models` → `/storage/emulated/0/models`，
并把存在的目录通过 `DISKINFER_MODEL_DIRS`（冒号分隔）传给 `disktui`。

## 跑

设备上打开 Termux（新窗口让 `diskinfer` 进 PATH）：

```
diskinfer                              打开 TUI（模型选择 → 设置页 → 运行页）
diskinfer -m ~/models/x.gguf           跳过模型选择
diskinfer --api-only -m ~/models/x.gguf  无 TUI，只开 /v1 API（端口 4000）
diskinfer -h                           全部参数
```

TUI 设置页：↑↓ 选择、←→ 调整、Enter 切换、`s` 进入运行页、`c` 返回、`q` 退出。
可调：上下文长度、温度、聊天模板、MoE 快速路径（`LLAMA_DISK_MOE=1`）、内存池 (MB)、API 开关。

TUI 运行页：提示词默认为空（进入后光标直接在 Prompt 区，`i` 重新编辑，`s` 或 Enter 开始推理）；
底部固定显示按键说明 `s=开始推理  c=取消/返回  q=退出  i=编辑提示词`。

`/v1` API 与 OpenAI 兼容（子集），无鉴权，同时只跑一个推理（忙时 429）：

```bash
adb forward tcp:4000 tcp:4000
curl http://127.0.0.1:4000/v1/models
curl http://127.0.0.1:4000/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"x.gguf","messages":[{"role":"user","content":"hi"}],"max_tokens":32}'
```

标准 llama.cpp 命令行也还在（在 `/data/local/tmp/llm/` 下，或 `~/llm/`）：

```bash
./llama-completion -m /path/model.gguf -p "Hello" -n 32 --temp 0.3
# 磁盘模式是默认的；要原生行为就加 LLAMA_DISK_STREAM_MB=0
```

## MoE 快速路径（默认关闭，设置页 / `LLAMA_DISK_MOE=1` 可开）

MoE 模型（如 Qwen3.5-MoE）的 `ffn_*_exps` 是三维专家权重张量。默认整张换入；打开
设置页的 **MoE fast path**（或 `LLAMA_DISK_MOE=1`）后，每个 `MUL_MAT_ID` 节点只读 router
选中的专家切片：

- 实测（Qwen3.5-0.8B-MoE，28 experts/top-12，arena 1024 MiB）：`nodes=912 experts=12768
  expert_read=1061MiB fallbacks=0`，逐节点 `selected 14/28 experts`
- 开/关的贪心 token id 逐 token 一致；非 MoE 模型开启后行为不变（无 MUL_MAT_ID 节点）
- MoE 开启时自动禁用预读；`LLAMA_DISK_WINDOW=1` 是实验特性，**不要**和 MoE 组合

> 注意：磁盘模式为省内存不使用权重重排（repack）。在启用 repack 的桌面平台（AVX2/ARM dotprod），
> 同一模型的贪心 token 序列可能与原生 mmap 模式不同（浮点累加顺序差异，不是错误）。
> Android 目标构建为 baseline（`GGML_NATIVE=OFF`），本机实测流式与原生一致。

## 第一次运行该看到什么

```
llama-disk: 851 tensors registered, 3616 MiB to stream, largest tensor 'token_embd.weight' 170 MiB
llama-disk: streaming weights from disk, arena = 194 MiB (raised to fit the largest tensor), direct io = 1, mlock = 1, prefetch = 0 (lookahead 16)
```

第二行会明确写出 arena 有多大、为什么是这个值。如果这两行没出现，说明
`LLAMA_DISK_STREAM_MB=0` 被设进了环境。

## 常见问题

**`diskinfer: not found`** — root 的 Termux shell 看不到 `$PREFIX/bin`。重开一个 Termux
窗口，或者 `export PATH=/data/data/com.termux/files/usr/bin:$PATH`。`setup.sh` 已经写进
`~/.bashrc` 并做了 `/debug_ramdisk` 软链，正常不会遇到。

**装完 `diskinfer` 不在 `/data/local/tmp/llm/`** — v3 的 bug（推成了 `device/` 子目录）。
安装器推的是扁平布局，旧的 `device/` 布局再跑一次 `setup.sh` 也会被摆平。可以直接检查：

```bash
adb shell ls -l /data/local/tmp/llm
```

**跑到几分钟被系统杀掉** — 没 root，进程没设 `oom_score_adj=-1000`。1GB 这种小设备必须
有 root，8GB 以上一般没事。

**很慢 / 每秒不到 1 个 token** — 正常。每个 token 都要把模型读一遍，3.5GB 模型在
200MB/s 的闪存上就是 17 秒起步。想快只能让 arena 装下整个模型（内存要够）或者换更小的模型。

**输出是乱码或一直 `<think>`** — 这个 Q1 量化模型在温度 1.0 下容易胡思乱想，用
`--temp 0.3`；中文能力也明显不如英文，这是模型本身的问题。

**想看原生 llama.cpp 的速度**（模型能装进内存时）

```bash
LLAMA_DISK_STREAM_MB=0 ./llama-completion -m model.gguf -p "..." -n 32
```

**卸载**

```bash
adb shell su -c 'rm -rf /data/data/com.termux/files/home/llm /data/local/tmp/llm \
  /data/data/com.termux/files/usr/bin/diskinfer /data/data/com.termux/files/usr/bin/termfont \
  /data/adb/service.d/99-disktui.sh /sdcard/Download/diskinfer'
```
