# diskinfer Android 1.0.1

## 更新了什么
- 版本号改为 **diskinfer 1.0.1**（标题栏同步）。
- **不再默认 "Hello" 自动推理**：提示词默认为空，自己输入。
- 设置页新增 **「内存池 (MB)」**：128–2048，`0 = 自动`；`←→` 调整或直接输入数字，开始推理时按新值重载模型。
- 运行页按键说明写清楚：`s` 开始推理、`c` 取消/返回、`q` 退出、`i` 编辑提示词。

## 怎么用
1. 解压（包名 `diskinfer-1.0.1-<abi>.zip`：x86_64 / arm64-v8a / armeabi-v7a）。
2. 安装：
   - 电脑 + adb：运行 `install.sh`（Windows 用 `install.ps1`），按提示填/拖入 adb 路径。
   - 手机 Termux：`cd` 到解压目录，执行 `sh install.sh`。
3. 放模型：Termux 里放 `~/models/`；或放到 `/sdcard/Download/` 后按安装脚本提示操作。
4. Termux 输入 `diskinfer`。
5. 选模型 → 设置页调参数 → 按 `s` 进运行页 → 按 `i` 输入提示词 → 按 `s` 开始推理（`c` 取消/返回，`q` 退出）。

> 包内不再附带 README，使用说明就是本文件。
