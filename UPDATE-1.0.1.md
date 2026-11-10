## diskinfer Android 1.0.1

### 本轮更新（仍然打包为 1.0.1）

**极限小 arena：把大张量继续拆小**
- arena 小于最大张量时自动进入 tiny 模式：`token_embd` 输入只读被选中的行（prefill 用 compact 行 gather），输出层（含 tied `token_embd.weight`）按行分块，其它放不下的大矩阵（ffn/attn）按行分块。
- arena 下限到 **8 MiB**；实测 Qwen3.5-0.8B-MoE 用 **8–16 MiB**、Bonsai2 27B 用 **16 MiB** 都能跑，输出与 2048 MiB arena 逐 token 一致。
- TUI「内存池 (MB)」支持 8..2048（128 以下减半步进，0=自动）；`LLAMA_DISK_TINY_ARENA=0` 可关闭自动 tiny。

**TUI 新增设置 / 显示**
- 新增「Prefill 批量」：一次 prefill 的 token 数，1..256，默认 4；越大 prefill 越快、峰值内存越高。
- 设置页底部实时显示「常驻 RAM / 峰值 RAM」预估（KV、递归状态、计算缓冲取自 llama.cpp 对临时 context 的真实尺寸；arena、模型元数据为实测值），运行页同样显示。

**读盘与闪存寿命**
- 读盘缓存自动策略：模型 ≤ 可用内存 45% 时用 buffered IO（page cache 吸收重复整模型扫描，热起来后不再读闪存），否则 direct IO；`LLAMA_DISK_ODIRECT=0/1` 可覆盖。
- 实测 Qwen 0.5B：direct 0.48 s/token → buffered 热缓存 **0.17 s/token**（logits 一致）。

**修复**
- 行窗口/视图窗口的 pin 计数、O_DIRECT 对齐偏移、窗口提前释放、并发安装竞态、prefill 多 token 的 compact gather 等一整轮正确性 bug；window 模式与基准逐 token 一致。
- 其余 1.0.1 内容不变：llama.cpp v0.9.0、MoE 快速路径、新 TUI、`/v1` API、自适应安装器、多语言、三 ABI。

### 实测（Android x86_64 模拟器，8GB RAM，本地盘）

| 模型 | arena / 模式 | decode | RSS | logits |
|---|---|---|---|---|
| Qwen3.5-0.8B-MoE 514 MiB | 2048 MiB direct | 0.05 s/tok | 632 MiB | 基准 |
| 同上 | 16 MiB tiny | 0.43–0.57 s/tok | 146 MiB | 一致 |
| 同上 | 8 MiB tiny | 0.43–0.51 s/tok | 139 MiB | 一致 |
| 同上 | 16 MiB tiny + buffered | 0.16–0.17 s/tok | 132 MiB | 一致 |
| Bonsai2 27B 7.26 GiB | 2048 MiB direct | 7.99 s/tok | 2317 MiB | 基准 |
| 同上 | 16 MiB tiny | 11.28 s/tok | **287 MiB** | 一致 |

手机真机（arm64/armv7）、27B 的 buffered 收益、真实闪存速度：**待测**。

### 使用方法

```bash
adb shell getprop ro.product.cpu.abilist
cd 解压目录 && ./install.sh          # root 走 su，非 root 给 Termux 命令
adb push model.gguf /data/local/tmp/llm/models/
diskinfer                            # TUI：模型 → 设置页 → 运行页
diskinfer --api-only -m model.gguf   # 只开 /v1 API（4000）
LLAMA_DISK_STREAM_MB=16 diskinfer    # 极小 arena（自动分块）
```

TUI 设置页：上下文长度、温度、聊天模板、MoE 快速路径、内存池 (MB) 8..2048、Prefill 批量 1..256、API 服务；底部显示常驻/峰值 RAM 预估。

### 注意事项

- tiny arena 只省内存、不省 IO：每个 token 仍会顺序扫一遍模型，速度取决于闪存带宽。
- tiny 模式下 Prefill 批量增大会明显提高计算缓冲（实测 ub16 +44.5 MiB、ub32 +89 MiB），小内存设备建议 1..8。
- 模型放不进可用内存时会自动用 direct IO（避免 page cache 挤占）；想强制缓存可设 `LLAMA_DISK_ODIRECT=0`。

更多细节与完整实测表见仓库 README。
