# diskinfer-android - 在Android设备上运行远大于可用内存的 GGUF 模型

这是一个基于llama.cpp修改而生的项目，在使用diskinfer推理模型时，权重全程留在硬盘，不依靠系统的swap和mmap，推理时按计算图节点把张量读进一块固定大小的内存arena，在使用完后即按 LRU 换出。代价是每个 token 都要把模型读一遍，运行速度绝对正比于磁盘读速度。正因如此，设备可运行的最大模型取决于该模型的最大单张量/层大小


此项目包含了一个 `disktui` 用于在termux进行可视化输出

```
测试数据（仅作参考，实际使用效果以自己手机性能而定，此次测试使用Bonsai-27B Q1_0推理）
设备：小天才电话手表 Z8（高通骁龙429w即Wear4100，839MB内存，arm32设备，arena大小给与176MB）
  [ 1:28] prompt done: 5 tok in 88.6s (17.7 s/tok), io 37.4s
  [ 0:29] t1  io 17.7s cmp 11.5s tot 29.2s  ' Paris'
  [ 0:58] t2  io 17.8s cmp 11.4s tot 29.2s  '.'
  answer:  Paris.

设备：红米K20 Pro（高通骁龙855，8GB内存，arena大小给与2048MB）
  [ 0:41] prompt done: 6 tok in 41.8s (7.0 s/tok), io 25.7s
  [ 0:19] t1  io 12.5s cmp  7.2s tot 19.7s
```

## 默认行为

`load_tensors()` 不再 mmap、也不全量读入，直接走磁盘 streaming。只有一个开关能关掉：

```
LLAMA_DISK_STREAM_MB=0      # 回到原生 llama.cpp 行为（mmap / 全量读入）
```


## arena 大小（自动决定）

| 情况 | arena |
|---|---|
| `LLAMA_DISK_STREAM_MB` 未设置或 `auto` | **内存总量 / 8**，至少 176 MiB |
| `LLAMA_DISK_STREAM_MB=N` | N MiB |

分配要求：
1. **不能小于最大单张量** → 自动抬到 `最大张量 + 24 MiB` 并提示（）
2. **上限 2048 MiB** → 超出截断
3. **按可用内存收窄** → `MemAvailable - 220 MiB`，剩下的留给词表 / KV / 计算缓冲

```
llama-disk: arena 111 MiB cannot hold the largest tensor 'token_embd.weight' (170 MiB), raised to 194 MiB
llama-disk: streaming weights from disk, arena = 194 MiB (raised to fit the largest tensor), direct io = 1, mlock = 1, prefetch = 0 (lookahead 16)
```

**关于提速**：arena 小于模型时，因为是顺序扫描 + LRU，命中率≈0，io 时间基本不变（模型大小 ÷ 带宽）。**arena 装得下整个模型时，第一个 token 之后权重就全在内存里**，此时将和llama.cpp原生CPU模式无异

## 快速开始
更简单地，你可以直接使用Agent工具，将项目地址发给他并为其提供adb工具，AI将自动完成。
-----
下载 Release 里对应你设备架构的包：

```bash
adb shell getprop ro.product.cpu.abilist
#   有 arm64-v8a     -> diskinfer-v3-arm64-v8a.zip
#   只有 armeabi-v7a -> diskinfer-v3-armeabi-v7a.zip

cd 解压出来的目录
./install.sh
adb push your-model.gguf /storage/emulated/0/models/
```

然后在设备的 Termux 里敲 `diskinfer`。详见Release。

## 支持的设备

- **架构**：`arm64-v8a`、`armeabi-v7a`（静态二进制，无 .so 依赖，Android 7+）
- **root**：推荐但非必须。有 root 会 `su` 设 `oom_score_adj=-1000`（小内存设备必须，否则跑几分钟就被lowmemorykiller杀掉）；
- **Termux**：用于命令入口；也可以直接 `adb shell` 跑二进制
- **模型**：任何 llama.cpp 认识的 GGUF，目前仅测试了qwen35架构的模型可以正常运行并读取，MoE模型正在开发，但并不是不可使用，而是速度跟稠密的该模型一样，无实用意义

## 参数

```
diskinfer                              交互，模型自动找 /storage/emulated/0/models/*.gguf
diskinfer "The capital of France is"   一次性
diskinfer -h                           全部参数

-m PATH       指定模型
--arena MiB    权重 arena（见上；不指定就自动）
-n N           最多生成多少 token（默认 256）
-c N           上下文（默认 256）
-ub N          prefill 批大小（默认 4；1 = 最省内存、prefill 最慢）
-t N           线程（默认 4，手机上可给 8）
--temp F       温度（默认 1.0）
--top-k N      默认 20        --top-p F   默认 0.95
--raw          裸补全（Bonsai-27B只能适应这个，不然会出现问题）    --chat  套 chat 模板并关掉 thinking
--sys TEXT     --chat 用的 system message
```

## 界面

```
+---------------------------------------+
| [  3.3s] model ready: qwen35 27B Q1_0  |
| [  3.4s] context ready  rss 217 MiB   |
| ---- turn ----                        |
| prompt = 17 tokens (raw completion)   |
| [ 1:38] pp 1-4/17 io 23.5s cmp 75.3s  |
| [ 3:15] pp 5-8/17 io 21.9s cmp 55.4s  |
| Q: The capital of France is           |
| A: Paris.                             |
| pp 13/17 ' is' L39 io 12.4s cmp 8.0s  |
+---------------------------------------+
```

- 上面 `行数-3` 行：滚动日志（加载细节、每批 prefill、每个生成 token）
- 下面固定 3 行：`Q:` / `A:`（超长显示尾部）/ 状态行
- 状态行 250ms 刷新：`<阶段> <第几个>/<总共> '<当前token>' L<当前层> io <本批已读>/<累计> ld<读次数> cmp <已算>`
- 完整日志写到 `/data/local/tmp/llm/diskinfer.log`（有 root）或 `~/diskinfer.log`
- 答案另存 `~/last_answer.txt`

## 也能用普通 llama.cpp 的命令行

release 里带了 `llama-completion`，就是本 fork 编译出的标准 llama.cpp 工具，可以直接使用该llama.cpp构建而无需依赖termux

```bash
./llama-completion -m model.gguf -p "The capital of France is" -n 32 --temp 0.3

# 想回到原生行为（模型能装进内存时，~~但这还有什么意义，你为何不去用原版~~）
LLAMA_DISK_STREAM_MB=0 ./llama-completion -m model.gguf -p "..." -n 32
```

chat 模板、sampling 预设、`-cnv` 这些原生功能都能用，只是没有 `disktui` 的分屏和逐 token 计时。

## 原理
值得注意的是，此项目原本是用作在小天才电话手表上运行Qwen3.8 27B的实验项目，故下面的实现路径与小内存+armv7a设备相匹，armv8设备未做特殊优化，或者说不是该项目的主要受益设备
（以下轨迹由DeepSeek-V4.1-Flash-1017生成，如果你对AI生成的文字感到不适，请在这里停止阅读或跳转到 已知限制  章节查看）。

----

llama.cpp 默认要么全量读入内存，要么 mmap 全部。这两种在这类设备上都不可行：

1. **mmap 不行**：32 位进程虚拟地址只有 ~3GB，3.5GB 的模型连映射都做不到
2. **全量读入不行**：内存装不下

这里走第三条路 —— **`no_alloc` + 节点级按需换入**：

- 用 llama.cpp 的 `no_alloc` 模式加载：只读元数据，权重一个字节都不读，张量拿 dummy buffer
- 在 ggml CPU 后端的图执行循环里插节点回调（`ggml_cpu_set_node_residency_cb`）
- 每个节点执行前，把它需要的张量从 gguf 里 `pread` 进 arena（默认 `O_DIRECT` 绕 page cache），
  执行后解除 pin；arena 满就按 LRU 换出
- 张量按文件顺序预排序，读盘基本是顺序的

改动的文件（都在 `src/llama.cpp-fork/` 里，已经改好，直接编译）：

| 文件 | 改了什么 |
|---|---|
| `src/llama-disk.h` `.cpp` | **新增**，磁盘 streaming 引擎（arena/块分配/LRU/O_DIRECT/诊断） |
| `examples/diskui/` | **新增**，TUI + 驱动 |
| `examples/diskrun/` | **新增**，最小驱动（不带 TUI，调试用） |
| `src/llama-model.cpp` | 磁盘模式：跳过 mmap、`no_alloc`、登记权重张量 |
| `src/CMakeLists.txt` | 加 `llama-disk.cpp` |
| `ggml/include/ggml-cpu.h` | 声明 `ggml_cpu_set_node_residency_cb()` |
| `ggml/src/ggml-cpu/ggml-cpu.c` | 节点执行前后调回调 |
| `examples/CMakeLists.txt` | 加 `diskui` / `diskrun` |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `LLAMA_DISK_STREAM_MB` | **开启（auto 定容）** | arena 大小；`0` = 关闭，回到原生 mmap |
| `LLAMA_DISK_ODIRECT` | 1 | `O_DIRECT`，绕过 page cache；`0` 关掉 |
| `LLAMA_DISK_MLOCK` | 0 | mlock arena，防 zram 换出（`diskinfer` 默认开） |
| `LLAMA_DISK_PREFETCH` | 0 | 后台预读线程（实测无收益） |
| `LLAMA_DISK_LOOKAHEAD` | 16 | 预读看几个张量 |
| `LLAMA_DISK_VERBOSE` | 0 | 每 4096 次回调打计数 |
| `LLAMA_DISK_DEBUG` | 0 | 看门狗 + 读盘诊断 |
| `LLAMA_DISK_WINDOW` | 0 | 行/视图窗口（实验性，有已知死锁） |
| `LLAMA_DISK_OUT_CHUNK` | 0 | 分块 logits 行数（实验性） |
| `GGML_CPU_DISABLE_FUSION` | 自动置 1 | 磁盘模式下由 `init()` 设置 |

## 如何自己编译

```bash
NDK=/path/to/android-ndk ./build/build-android.sh
```

产物在 `build/out/<abi>/{disktui,llama-completion}`，已 strip。
源码就是 `src/llama.cpp-fork/`，不需要打任何 patch。

两个坑（脚本里已处理）：

- **armv7 必须手动加 `-mfpu=neon-vfpv4 -mfloat-abi=softfp`**，NDK 的 `ANDROID_ARM_NEON=TRUE`
  在这个组合下不会真的加 `-mfpu=neon`，漏了就是纯标量，慢 2-3 倍
- **32 位必须加 `-D_FILE_OFFSET_BITS=64`**，否则 `off_t` 是 32 位，>2GB 的文件连 GGUF magic
  都读不出来


## 已知限制
欢迎回来，这里以后的文字不再由AI生成。

---

- 每个 token 都要全部遍历一遍模型自身，因此速度绝对由闪存带宽决定（顺序读 150~250MB/s 很常见，但如何老旧的设备算力也是需要时间的）
- arena < 模型时加大 arena 基本没用（顺序扫描 + LRU 命中率≈0）
- 小心zram消耗殆尽，这样会导致emmc的读取速度大幅度下降
- `LLAMA_DISK_WINDOW` / `LLAMA_DISK_OUT_CHUNK` 是实验特性，未经过测试，小arena的情况下会出问题


**欢迎让你的老设备爆发潜力!**
