# diskinfer Android（1.0.1）

## 1.0.1 新内容（含 2026-11-09 需求批）

### 功能：MoE 优化、新 TUI、`/v1` API、安装器、多语言

- **磁盘流式推理**：权重不 mmap、不整体读入内存，按计算图节点 `pread` 进固定大小 arena，用完按 LRU 换出；
  arena 能装下整个模型时，第一个 token 之后就没有 IO。
  arena 由 `LLAMA_DISK_STREAM_MB` 控制，TUI 设置页里也能改。
- **MoE 快速推理（优化 + 修复）**：`MUL_MAT_ID` 节点只读取 router 选中的专家切片，其余专家权重不读盘（专家级流式，不再整张量换入）；
  设置页 `MoE 快速路径` 开关或 `LLAMA_DISK_MOE=1`（默认关）。
  实测 Qwen3.5-0.8B-MoE（28 experts / top-12）`nodes=762 experts=10668 fallbacks=0`，
  贪心 token 与整张量加载逐 token 一致。
- **新 TUI（`diskinfer` 直接进入）**：
  - 模型选择页：自动扫描模型目录、显示大小、同名模型去重；`r` 重新扫描。
  - 设置页：上下文长度（3 … 模型上限，默认 16）、温度（0–1，默认 0.70）、聊天模板（默认关，
    开启时 ctx 自动变成 `16 + 模板额外 token 数`，之后可改）、MoE 快速路径（默认关）、
    **内存池 (MB)**（128–2048，`0 = 自动`）、API 服务（默认关，开启后提示 `http://127.0.0.1:4000/v1`）。
  - 运行页：Prompt / Progress（prefill 进度条、生成 tok/s、io/cmp、累计读盘、换出）/ Answer / Log；
    提示词默认为空、由用户输入；底部固定按键说明 `s=开始推理  c=取消/返回  q=退出  i=编辑提示词`。
  - 小屏适配：行数 < 20 或列数 < 60 自动紧凑布局（900×1600 手机 + Termux 40×55 实测）。
- **本机 `/v1` API（4000 端口）**：无鉴权；`GET /v1/models` 返回模型文件名，`POST /v1/chat/completions`、
  `POST /v1/completions`、`GET /health`；**同时只跑一个推理，忙时 HTTP 429**；`--api-only` 可无 TUI 常驻。
- **安装器（自适应 root / 非 root）**：
  - 电脑上运行：问 adb 路径（支持拖入/引号/纯 `adb`）；root → 推 `/data/local/tmp/llm/` 并 `su` 安装，
    非 root → 推 `/sdcard/Download/diskinfer/` 并打印 Termux 一行命令。
  - Termux 内运行：直接装到 `~/llm/` + `$PREFIX/bin`。
  - root 额外装 oom 保护 / 开机脚本；非 root 不设。无模型时按 root / 非 root 分别提示模型放置路径。
  - 包与安装脚本全扁平布局，`setup.sh` 也兼容旧的 `device/` 布局并自动挪平。
- **三个 ABI 包**：`x86_64`（模拟器/ChromeOS）、`arm64-v8a`（手机/平板）、`armeabi-v7a`（32 位老设备）。
- **多语言**：TUI / 启动器 / 安装器支持 en / zh-Hans / zh-Hant，按系统语言自动识别
  （Android `persist.sys.locale` → `ro.product.locale` → `LANG/LC_ALL`，Termux 默认 `LANG` 不会盖过手机语言），
  `--lang` 或 `DISKINFER_LANG` 可覆盖。
- **上游基线**：fork 升级到 llama.cpp **v0.9.0（`cd959bae`）**，版本号 `0.9.0-dev`。

> 下面这批功能（MoE 快速推理优化、新 TUI、`/v1` API、安装器自适应、多语言、上游 v0.9.0）
> 以及「修复的 bug」里的全部条目，都属于 **1.0.1** 的内容。

### 1.0.1 自身变化

- 标题栏版本显示 **`diskinfer 1.0.1`**（原 `v5`）。
- **不再默认 "Hello" 自动推理**：提示词默认为空，进入运行页时光标直接在 Prompt 区，由用户自己输入。
- 设置页新增 **内存池 (MB)**；运行页底部按键说明改为显式的 `s=… c=… q=… i=…`。
- 包内不再附带 README（更新/使用说明单独发布）。

## 修复的 bug（同样是 1.0.1 的内容）

1. **安装布局 bug（网上反馈）**：旧 `install.sh` 把 `device/` 推成 `/data/local/tmp/llm/device/…`，
   而启动器/脚本按扁平路径找 → 装完命令不可用。现在包与安装器全部扁平布局（`setup.sh` 同时兼容
   旧的 `device/` 布局并自动挪平），安装后逐文件自检。
2. **MoE 快速路径 3 个 bug**：① ids 是 argsort view、行 stride 含 padding → 按 stride 正确解析；
   ② qwen35 的 MTP/nextn 层 0-token 空节点被整张量读入并常驻 arena → 跳过空节点；
   ③ 一次回退后整张量常驻导致 MoE 永久失效 → 改为可回收复用。
3. **取消显示成错误**：旧版取消后显示 `finish error`，还打 `ggml/llama_decode` 错误日志 →
   现在显示 `finish cancelled`，只记一行 `[cancel] user cancelled after N tokens`。
4. **设置跨页/换模型丢失**：ctx、温度、MoE/API 开关会被重置 → 现在会话内保留，超出新模型上限时 clamp 并提示。
5. **模型列表重复**：同一模型出现在多个目录时重复列出 → 按 basename + 大小去重。
6. **TUI 排版**：Answer 内容不在 Answer 分区、设置页底部按键行被截断、换模型后日志混在一起 →
   已修（换模型时插入 `---- model: xxx ----` 分隔行）。
7. **语言被 Termux 环境覆盖**：检测顺序改为 `persist.sys.locale` → `ro.product.locale` → `LANG/LC_ALL`，
   Termux 默认的 `LANG=en_US.UTF-8` 不再把中文界面顶成英文。
8. **设置页选中「内存池」后按 `s` 无效**（1.0.1 修复）：该设置项的处理函数吞掉了后续按键 → 改为落空处理。
9. **无模型引导缺失**：以前只说"没找到模型" → 现在按 root / 非 root 给出各自放置路径与命令。
10. **安装后版本号不对**：标题栏原来写死 `v5`/`v4` → 改为构建期 `-DDISKINFER_UI_VERSION` 注入，
    Android 显示 `diskinfer 1.0.1`。

## 使用方法

1. **解压**：包名 `diskinfer-1.0.1-<abi>.zip`（按手机选 x86_64 / arm64-v8a / armeabi-v7a）：
   ```bash
   adb shell getprop ro.product.cpu.abilist    # 有 arm64-v8a 用 arm64；只有 v7a 用 v7a；x86_64 用 x86_64
   ```
2. **安装**：
   - 电脑 + adb：解压后运行 `install.sh`（Windows 用 `install.ps1`），按提示填/拖入 adb 路径，root/非 root 自动处理。
   - 手机 Termux：把包拷进手机，`cd` 到解压目录后 `sh install.sh`。
3. **放模型**：
   ```bash
   # 非 root（Termux）：私有目录，永远可读
   cp /sdcard/Download/xxx.gguf ~/models/
   # 有 root / adb 直接用
   adb push xxx.gguf /data/local/tmp/llm/models/
   # 共享目录（Termux 授权后可读）
   adb push xxx.gguf /storage/emulated/0/models/
   ```
4. **运行**：
   ```bash
   diskinfer                    # 打开 TUI
   diskinfer -m ~/models/xxx.gguf   # 跳过模型选择
   diskinfer --api-only -m ~/models/xxx.gguf   # 只开 /v1 API（端口 4000）
   ```
5. **TUI 操作**：选模型 → 设置页调参数 → 按 `s` 进入运行页 → 按 `i` 输入提示词 → 按 `s` 开始推理；
   `c` 取消/返回，`q` 退出。
6. **API 示例**：
   ```bash
   adb forward tcp:4000 tcp:4000
   curl http://127.0.0.1:4000/v1/models
   curl http://127.0.0.1:4000/v1/chat/completions -H 'Content-Type: application/json' \
     -d '{"model":"xxx.gguf","messages":[{"role":"user","content":"hi"}],"max_tokens":32}'
   ```

## 常用参数 / 环境变量

| 项 | 说明 |
|---|---|
| `-m PATH` | 指定模型 |
| `--arena MiB` | 权重 arena（128–2048） |
| `-c N` / `-n N` | 上下文 / 最大生成 token |
| `-t N` / `-ub N` | 线程 / prefill 批大小 |
| `--temp F` | 温度 |
| `--api-only` | 无 TUI，只开 `/v1` API |
| `--lang auto\|en\|zh-Hans\|zh-Hant` | 界面语言 |
| `LLAMA_DISK_STREAM_MB` | arena 大小；`0` = 关闭磁盘流式（回到原生 mmap） |
| `LLAMA_DISK_MOE` | `1` = MoE 快速路径 |
| `LLAMA_DISK_ODIRECT` / `LLAMA_DISK_MLOCK` | `O_DIRECT` 读 / mlock arena（默认 1 / 0） |
| `LLAMA_DISK_VERBOSE` | 统计日志 |
| `DISKINFER_LANG` | 界面语言覆盖 |
| `DISKINFER_MODEL_DIRS` | 模型目录（冒号分隔，启动器自动设置） |

## 已知限制

- arena < 模型时，每个 token 都要把模型完整读一遍，速度 ≈ 闪存顺序带宽（加大 arena 不是万能）。
- 非 root 无法设置 oom 保护，长推理在 1–2 GB 设备上可能被系统回收。
- armv7（32 位）无法寻址 ≥ 4 GiB 的 GGUF。
- 桌面平台 native 默认启用 repack，与流式对照时 token 可能与流式不同（浮点累加顺序，不是 bug）；
  Android 目标构建为 baseline，本机实测一致。
- `LLAMA_DISK_WINDOW=1` 是既有实验特性，不要与 MoE 组合。

## 卸载

```bash
adb shell rm -rf /data/local/tmp/llm /sdcard/Download/diskinfer
# Termux 内：
rm -rf ~/llm
rm -f $PREFIX/bin/diskinfer $PREFIX/bin/termfont
# root 还要删开机脚本
adb shell su -c 'rm -f /data/adb/service.d/99-disktui.sh'
```
