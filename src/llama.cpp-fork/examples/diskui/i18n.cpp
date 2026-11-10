// i18n.cpp: table driven en / zh-Hans / zh-Hant strings + platform language detection
//
// the tables are plain const char* rows (key, en, zh-Hans, zh-Hant) grouped by
// area. tr() is a linear scan: a few hundred short keys, called a few hundred
// times per TUI frame, which is far below the cost of painting the screen.

#include "i18n.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>
#endif

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#ifndef DISKINFER_UI_VERSION
#define DISKINFER_UI_VERSION "1.0.1"
#endif

namespace i18n {
namespace {

struct Entry {
    const char * key;
    const char * en;
    const char * hans;
    const char * hant;
};

// ---------------------------------------------------------------- UI strings
static const Entry kUiEntries[] = {
    // on/off and other tiny words
    { "common.on",                    "on",   u8"开",   u8"開" },
    { "common.off",                   "off",  u8"关",   u8"關" },
    { "common.auto",                  "auto", u8"自动", u8"自動" },
    { "common.none",                  "(none)", u8"（无）", u8"（無）" },
    { "common.raw",                   "raw",  u8"原始", u8"原始" },
    { "common.template",              "template", u8"模板", u8"範本" },
    { "common.finish_stop",           "stop",      u8"停止",   u8"停止" },
    { "common.finish_length",         "length",    u8"达到长度", u8"達到長度" },
    { "common.finish_cancelled",      "cancelled", u8"已取消", u8"已取消" },
    { "common.finish_error",          "error",     u8"错误",   u8"錯誤" },

    // title bars
    { "title.model",        u8" diskinfer {V}  ·  model ",
                            u8" diskinfer {V}  ·  选择模型 ",
                            u8" diskinfer {V}  ·  選擇模型 " },
    { "title.settings",     u8" diskinfer {V}  ·  disk streaming TUI ",
                            u8" diskinfer {V}  ·  磁盘流式 TUI ",
                            u8" diskinfer {V}  ·  磁碟串流 TUI " },
    { "title.run",          u8" diskinfer {V}  ·  run ",
                            u8" diskinfer {V}  ·  运行 ",
                            u8" diskinfer {V}  ·  執行 " },
    { "title.loading",      u8" diskinfer {V}  ·  loading model ",
                            u8" diskinfer {V}  ·  正在加载模型 ",
                            u8" diskinfer {V}  ·  正在載入模型 " },
    { "title.load_failed",  u8" diskinfer {V}  ·  load failed ",
                            u8" diskinfer {V}  ·  加载失败 ",
                            u8" diskinfer {V}  ·  載入失敗 " },

    // model picker
    { "pick.no_model",      "  No .gguf model found.",
                            u8"  未找到 .gguf 模型文件。",
                            u8"  未找到 .gguf 模型檔案。" },
    { "pick.where",         "  put a model into one of these directories:",
                            u8"  请把模型放到以下任一目录：",
                            u8"  請將模型放到以下任一目錄：" },
    { "pick.none_dirs",     "    (none of the default directories exist)",
                            u8"    （默认目录都不存在）",
                            u8"    （預設目錄都不存在）" },
    { "pick.example",       "  example:", u8"  示例：", u8"  範例：" },
    { "pick.example_cmd",   "    adb push model.gguf /data/local/tmp/llm/models/",
                            "    adb push model.gguf /data/local/tmp/llm/models/",
                            "    adb push model.gguf /data/local/tmp/llm/models/" },
    { "pick.or_pass",       "  or pass -m /path/to/model.gguf",
                            u8"  或用 -m /path/to/model.gguf 指定",
                            u8"  或用 -m /path/to/model.gguf 指定" },
    { "pick.choose_fmt",    "  Choose a model   (%d found)",
                            u8"  选择模型   （找到 %d 个）",
                            u8"  選擇模型   （找到 %d 個）" },
    { "pick.footer",        u8"  ↑↓ select   Enter open   r rescan   q quit",
                            u8"  ↑↓ 选择   Enter 打开   r 重新扫描   q 退出",
                            u8"  ↑↓ 選擇   Enter 開啟   r 重新掃描   q 離開" },

    // settings page
    { "set.model_fmt",      "  model   %s", u8"  模型   %s", u8"  模型   %s" },
    { "set.ctx_max_fmt",    "  ctx max %-6d layers %-3d  arena %s",
                            u8"  上下文上限 %-6d 层数 %-3d  内存池 %s",
                            u8"  上下文上限 %-6d 層數 %-3d  記憶體池 %s" },
    { "set.stream_fmt",     "  stream  %s on disk, %d tensors, direct io %s",
                            u8"  磁盘  %s，%d 个张量，直接 IO %s",
                            u8"  磁碟  %s，%d 個張量，直接 IO %s" },
    { "set.section",        "Settings", u8"设置", u8"設定" },
    { "set.context",        "Context length", u8"上下文长度", u8"上下文長度" },
    { "set.temperature",    "Temperature", u8"温度", u8"溫度" },
    { "set.chat_template",  "Chat template", u8"聊天模板", u8"聊天範本" },
    { "set.moe_path",       "MoE fast path", u8"MoE 快速路径", u8"MoE 快速路徑" },
    { "set.arena",          "Arena (MB)", u8"内存池 (MB)", u8"記憶體池 (MB)" },
    { "set.api_server",     "API server", u8"API 服务", u8"API 服務" },
    { "set.prefill",        "Prefill batch", u8"Prefill 批量", u8"Prefill 批次" },
    { "set.ctx_hint_fmt",   "3 .. %d   <- -> adjust, or type a number",
                            u8"3 .. %d   ←→ 调整，或直接输入数字",
                            u8"3 .. %d   ←→ 調整，或直接輸入數字" },
    { "set.temp_hint",      "0.00 .. 1.00   step 0.05",
                            u8"0.00 .. 1.00   步进 0.05",
                            u8"0.00 .. 1.00   步進 0.05" },
    { "set.chat_hint_fmt",  "ctx becomes 16 + %d template tokens",
                            u8"上下文将变为 16 + %d 个模板 token",
                            u8"上下文將變為 16 + %d 個範本 token" },
    { "set.chat_hint_unknown", "ctx becomes 16 + 16 if the template size is unknown",
                            u8"模板大小未知时，上下文变为 16 + 16",
                            u8"範本大小未知時，上下文變為 16 + 16" },
    { "set.moe_hint",       "only router selected experts are read (reloads the model)",
                            u8"只读取路由选中的专家（会重新加载模型）",
                            u8"只讀取路由選中的專家（會重新載入模型）" },
    { "set.arena_hint",     "8..2048 MB   0 = auto; below the largest tensor reads chunks (reload)",
                            u8"8..2048 MB，0=自动；小于最大张量时自动分块读（重载模型）",
                            u8"8..2048 MB，0=自動；小於最大張量時自動分塊讀（重載模型）" },
    { "set.prefill_hint",   "1..256 tok per prefill batch; bigger = fewer graph launches, more peak RAM",
                            u8"1..256 token/批；越大 prefill 越快、峰值内存越高",
                            u8"1..256 token/批；越大 prefill 越快、峰值記憶體越高" },
    { "set.ram_resident_fmt", "  RAM resident ~ %s   (kv %s + rec %s + arena %s)",
                               u8"  常驻 RAM ≈ %s（KV %s + 递归 %s + arena %s）",
                               u8"  常駐 RAM ≈ %s（KV %s + 遞迴 %s + arena %s）" },
    { "set.ram_peak_fmt",   "  RAM peak     ~ %s   (compute %s + batch/logits %s)",
                            u8"  峰值 RAM ≈ %s（计算缓冲 %s + 批/输出 %s）",
                            u8"  峰值 RAM ≈ %s（計算緩衝 %s + 批/輸出 %s）" },
    { "set.ram_note",       "  RAM estimate not available yet (model not loaded / busy)",
                            u8"  RAM 预估暂不可用（模型未加载/推理中）",
                            u8"  RAM 預估暫不可用（模型未載入/推理中）" },
    { "run.info_arena_tiny_fmt", "arena %s (chunked, %s rows, direct io %s)",
                                 u8"内存池 %s（分块，%s 行/块，direct io %s）",
                                 u8"記憶體池 %s（分塊，%s 行/塊，direct io %s）" },
    { "run.info_ram_fmt",       "  RAM est.  ~ %s resident / %s peak  (KV+rec %s)",
                                 u8"  RAM 预估 ≈ %s 常驻 / %s 峰值（KV+递归 %s）",
                                 u8"  RAM 預估 ≈ %s 常駐 / %s 峰值（KV+遞迴 %s）" },
    { "set.api_hint_fmt",   "http://127.0.0.1:%d/v1  (no key)",
                            u8"http://127.0.0.1:%d/v1  （无密钥）",
                            u8"http://127.0.0.1:%d/v1  （無金鑰）" },
    { "set.status_section", "Status", u8"状态", u8"狀態" },
    { "set.status_busy_fmt", "API busy: %s", u8"API 忙：%s", u8"API 忙：%s" },
    { "set.tui_running",    "TUI inference running ...",
                            u8"TUI 正在推理……",
                            u8"TUI 正在推理……" },
    { "set.system_prompt_none", "system prompt: (none)",
                            u8"系统提示：无",
                            u8"系統提示：無" },
    { "set.system_prompt_fmt", "system prompt: %s",
                            u8"系统提示：%s",
                            u8"系統提示：%s" },
    { "set.footer_wide",    u8"  ↑↓ select   ←→ adjust   Enter toggle   s enter run page   c back   q quit",
                            u8"  ↑↓ 选择   ←→ 调整   Enter 切换   s 进入运行页   c 返回   q 退出",
                            u8"  ↑↓ 選擇   ←→ 調整   Enter 切換   s 進入執行頁   c 返回   q 離開" },
    { "set.footer_narrow",  u8"  ↑↓ ←→ Enter  s run  c back  q quit",
                            u8"  ↑↓ ←→ Enter  s 运行  c 返回  q 退出",
                            u8"  ↑↓ ←→ Enter  s 執行  c 返回  q 離開" },
    { "set.api_footer_fmt", "  API: http://127.0.0.1:%d/v1  (no key)",
                            u8"  API：http://127.0.0.1:%d/v1  （无密钥）",
                            u8"  API：http://127.0.0.1:%d/v1  （無金鑰）" },
    { "set.log_section",    "Log", u8"日志", u8"日誌" },

    // run page
    { "run.prompt_section",   "Prompt", u8"提示词", u8"提示詞" },
    { "run.progress_section", "Progress", u8"进度", u8"進度" },
    { "run.answer_section",   "Answer", u8"回答", u8"回答" },
    { "run.model_section",    "Model", u8"模型", u8"模型" },
    { "run.log_section",      "Log", u8"日志", u8"日誌" },
    { "run.prompt_hint",      "   (i edit prompt   s or Enter start)",
                              u8"   （i 编辑提示词，s 或 Enter 开始推理）",
                              u8"   （i 編輯提示詞，s 或 Enter 開始推理）" },
    { "run.prompt_empty_hint", "   prompt is empty: press i to type one, then s to start",
                              u8"   提示词为空：按 i 输入，输入完成后按 s 开始推理",
                              u8"   提示詞為空：按 i 輸入，輸入完成後按 s 開始推理" },
    { "run.meta_fmt",         "  ctx %d  temp %.2f  %s  MoE %s  API %s",
                              u8"  上下文 %d  温度 %.2f  %s  MoE %s  API %s",
                              u8"  上下文 %d  溫度 %.2f  %s  MoE %s  API %s" },
    { "run.prefill_live_fmt", "  prefill  [%s] %d/%d tok   io %s  cmp %s",
                              u8"  预填充  [%s] %d/%d tok   io %s  cmp %s",
                              u8"  預填充  [%s] %d/%d tok   io %s  cmp %s" },
    { "run.prefill_done_fmt", "  prefill  %d tok in %s",
                              u8"  预填充  %d tok，用时 %s",
                              u8"  預填充  %d tok，耗時 %s" },
    { "run.prefill_active_fmt", "  prefill  %d tok done",
                              u8"  预填充  已完成 %d tok",
                              u8"  預填充  已完成 %d tok" },
    { "run.prefill_none",     "  prefill  -", u8"  预填充  -", u8"  預填充  -" },
    { "run.gen_live_fmt",     "  generate [%s] %d/%d  %.2f tok/s  L%d/%d",
                              u8"  生成 [%s] %d/%d  %.2f tok/s  L%d/%d",
                              u8"  生成 [%s] %d/%d  %.2f tok/s  L%d/%d" },
    { "run.gen_done_fmt",     "  generate %d tok   %.2f tok/s   finish %s",
                              u8"  生成 %d tok   %.2f tok/s   结束 %s",
                              u8"  生成 %d tok   %.2f tok/s   結束 %s" },
    { "run.gen_active_fmt",   "  generate %d tok   %.2f tok/s",
                              u8"  生成 %d tok   %.2f tok/s",
                              u8"  生成 %d tok   %.2f tok/s" },
    { "run.gen_none",         "  generate -", u8"  生成 -", u8"  生成 -" },
    { "run.disk_live_fmt",    "  disk  %s read  %llu loads  %llu evict  io %s cmp %s",
                              u8"  磁盘  %s 读取  %llu 次加载  %llu 次淘汰  io %s cmp %s",
                              u8"  磁碟  %s 讀取  %llu 次載入  %llu 次汰換  io %s cmp %s" },
    { "run.disk_idle_fmt",    "  disk  %s streamed on disk, %d tensors",
                              u8"  磁盘  %s 待流式读取，%d 个张量",
                              u8"  磁碟  %s 待串流讀取，%d 個張量" },
    { "run.api_busy_fmt",     "  API busy: %s", u8"  API 忙：%s", u8"  API 忙：%s" },
    { "run.tui_running",      "  TUI inference running ...",
                              u8"  TUI 正在推理……",
                              u8"  TUI 正在推理……" },
    { "run.idle_last_fmt",    "  idle   last: %d tok, %s",
                              u8"  空闲   上次：%d tok，%s",
                              u8"  閒置   上次：%d tok，%s" },
    { "run.idle_hint",        "  idle   s=start   i=edit prompt   c=back",
                              u8"  空闲   s=开始推理   i=编辑提示词   c=返回设置",
                              u8"  閒置   s=開始推理   i=編輯提示詞   c=返回設定" },
    { "run.footer_wide",      "  s=start infer   c=cancel/back   q=quit   i=edit prompt",
                              u8"  s=开始推理   c=取消/返回   q=退出   i=编辑提示词",
                              u8"  s=開始推理   c=取消/返回   q=離開   i=編輯提示詞" },
    { "run.footer_narrow",    "  s=start  c=cancel  q=quit  i=edit",
                              u8"  s=开始  c=取消  q=退出  i=编辑",
                              u8"  s=開始  c=取消  q=離開  i=編輯" },
    { "run.api_badge_busy",   "API busy ", u8"API 忙 ", u8"API 忙 " },
    { "run.api_badge_fmt",    "API :%d ", "API :%d ", "API :%d " },
    { "run.info_model_fmt",   "  model    %s", u8"  模型     %s", u8"  模型     %s" },
    { "run.info_path_fmt",    "  path     %s", u8"  路径     %s", u8"  路徑     %s" },
    { "run.info_size_fmt",    "  size     %s on disk, %d tensors registered",
                              u8"  大小     %s（磁盘），已登记 %d 个张量",
                              u8"  大小     %s（磁碟），已登記 %d 個張量" },
    { "run.info_arena_fmt",   "  arena    %s   direct io %s   mlock %s",
                              u8"  内存池   %s   直接 IO %s   mlock %s",
                              u8"  記憶體池 %s   直接 IO %s   mlock %s" },
    { "run.info_context_fmt", "  context  set %d   max %d   template %s",
                              u8"  上下文   设置 %d   上限 %d   模板 %s",
                              u8"  上下文   設定 %d   上限 %d   範本 %s" },
    { "run.info_runtime_fmt", "  runtime  %d threads   ubatch %d   max tokens %d",
                              u8"  运行时   %d 线程   批大小 %d   最大 token %d",
                              u8"  執行時   %d 執行緒   批次 %d   最大 token %d" },
    { "run.info_api_fmt",     "  api      http://127.0.0.1:%d/v1  (no key)",
                              u8"  接口     http://127.0.0.1:%d/v1  （无密钥）",
                              u8"  介面     http://127.0.0.1:%d/v1  （無金鑰）" },
    { "run.info_hint",        "  i edits the prompt, s or Enter starts",
                              u8"  i 编辑提示词，s 或 Enter 开始",
                              u8"  i 編輯提示詞，s 或 Enter 開始" },

    // loading screen
    { "load.registering", "  registering weights, allocating the disk arena ...",
                          u8"  正在登记权重、分配磁盘内存池……",
                          u8"  正在登記權重、分配磁碟記憶體池……" },
    { "load.wait",        "  this can take a few seconds",
                          u8"  可能需要几秒钟",
                          u8"  可能需要幾秒鐘" },
    { "load.press_back",  "  press any key to go back",
                          u8"  按任意键返回",
                          u8"  按任意鍵返回" },

    // status lines and page level messages
    { "st.ready",              "ready", u8"就绪", u8"就緒" },
    { "st.type_prompt",        "type a prompt first (i)",
                               u8"请先输入提示词（i）",
                               u8"請先輸入提示詞（i）" },
    { "st.no_model",           "no model loaded", u8"未加载模型", u8"未載入模型" },
    { "st.reloading_moe_on",   "reloading model, MoE on ...",
                               u8"正在重新加载模型，MoE 开……",
                               u8"正在重新載入模型，MoE 開……" },
    { "st.reloading_moe_off",  "reloading model, MoE off ...",
                               u8"正在重新加载模型，MoE 关……",
                               u8"正在重新載入模型，MoE 關……" },
    { "st.reloading_fmt",      "reloading model to apply the new settings ...",
                               u8"正在重新加载模型以应用新设置……",
                               u8"正在重新載入模型以套用新設定……" },
    { "st.reload_failed_fmt",  "reload failed: %s",
                               u8"重新加载失败：%s",
                               u8"重新載入失敗：%s" },
    { "st.running",            "running ...", u8"运行中……", u8"執行中……" },
    { "st.cancelling",         "cancelling ...", u8"正在取消……", u8"正在取消……" },
    { "st.cancelled",          "cancelled", u8"已取消", u8"已取消" },
    { "st.done",               "done", u8"完成", u8"完成" },
    { "st.error_fmt",          "error: %s", u8"错误：%s", u8"錯誤：%s" },
    { "st.api_start_failed_fmt", "API start failed: %s",
                               u8"API 启动失败：%s",
                               u8"API 啟動失敗：%s" },
    { "st.api_listening_fmt",  "API listening on :%d",
                               u8"API 正在监听 :%d",
                               u8"API 正在監聽 :%d" },
    { "st.api_stopped",        "API stopped", u8"API 已停止", u8"API 已停止" },
    { "st.no_template_new_model", "new model has no chat template, template switched off",
                               u8"新模型没有聊天模板，已关闭模板",
                               u8"新模型沒有聊天範本，已關閉範本" },
    { "st.ctx_clamped_fmt",    "ctx clamped to %d (model max %d)",
                               u8"上下文已限制为 %d（模型上限 %d）",
                               u8"上下文已限制為 %d（模型上限 %d）" },
    { "st.api_restart_failed", "API could not be restarted",
                               u8"API 无法重新启动",
                               u8"API 無法重新啟動" },
    { "st.gen_threw",          "generation threw",
                               u8"生成过程异常",
                               u8"生成過程異常" },
    { "st.busy_fmt",           "busy: %s", u8"忙：%s", u8"忙：%s" },
    { "st.load_failed_fmt",    "model load failed: %s",
                               u8"模型加载失败：%s",
                               u8"模型載入失敗：%s" },
    { "log.model_switch_fmt",  "---- model: %s ----",
                               u8"---- 模型：%s ----",
                               u8"---- 模型：%s ----" },

    // command line: --help (whole block) and the no-TTY explanation
    { "help.body",
      "disktui: settings page + run page TUI for the disk streaming build,\n"
      "         and an OpenAI compatible, no-auth API on port 4000\n"
      "\n"
      "usage: disktui [-m model.gguf] [options]\n"
      "       disktui -m model.gguf --api-only [--n-ctx N]\n"
      "\n"
      "ui:\n"
      "  -m PATH            model to load (otherwise a picker scans $DISKINFER_MODEL_DIRS,\n"
      "                     ~/models, /storage/emulated/0/models, /data/local/tmp/llm/models)\n"
      "  --lang LANG        UI language: auto (default), en, zh-Hans, zh-Hant\n"
      "  --moe              start with the MoE expert fast path on\n"
      "  --api              start with the API server on\n"
      "  --chat             start with the chat template setting on (default: raw)\n"
      "  -q, --prompt TEXT  initial prompt (default \"Hello\")\n"
      "  --sys TEXT         system message used when the chat template is on\n"
      "  --raw              raw completion mode (default)\n"
      "\n"
      "inference:\n"
      "  -c, --n-ctx N      context length (default 16, clamped to the model max)\n"
      "  -n, --n-predict N  max tokens to generate (default 256)\n"
      "  -t, --threads N    threads (default 4)\n"
      "  -ub N              prefill batch (default 4)\n"
      "  --temp F           temperature 0..1 (default 0.70)\n"
      "  --top-k N          top-k (default 40)\n"
      "  --top-p F          top-p (default 0.95)\n"
      "  --arena MiB        set LLAMA_DISK_STREAM_MB (weight arena size)\n"
      "\n"
      "api:\n"
      "  --api-only         serve the API without the TUI (no TTY needed)\n"
      "  --port N           listen port (default 4000)\n"
      "\n"
      "keys:\n"
      "  settings:  up/down select, left/right adjust, Enter toggle, s start, c back, q quit\n"
      "  run:       s start, c cancel, i edit prompt, q quit\n",
      u8"disktui：磁盘流式构建的“设置页 + 运行页”终端界面，\n"
      u8"         并在同一进程提供 4000 端口、无鉴权的 OpenAI 兼容 API\n"
      "\n"
      u8"用法：disktui [-m model.gguf] [选项]\n"
      "      disktui -m model.gguf --api-only [--n-ctx N]\n"
      "\n"
      u8"界面：\n"
      u8"  -m 路径            要加载的模型（不指定则扫描 $DISKINFER_MODEL_DIRS、\n"
      u8"                     ~/models、/storage/emulated/0/models、/data/local/tmp/llm/models）\n"
      u8"  --lang 语言        界面语言：auto（默认）、en、zh-Hans、zh-Hant\n"
      u8"  --moe              以开启 MoE 专家快速路径的状态启动\n"
      u8"  --api              以开启 API 服务的状态启动\n"
      u8"  --chat             以开启聊天模板的状态启动（默认：原始补全）\n"
      u8"  -q, --prompt 文本  初始提示词（默认 \"Hello\"）\n"
      u8"  --sys 文本         开启聊天模板时使用的系统消息\n"
      u8"  --raw              原始补全模式（默认）\n"
      "\n"
      u8"推理：\n"
      u8"  -c, --n-ctx N      上下文长度（默认 16，不超过模型上限）\n"
      u8"  -n, --n-predict N  最大生成 token 数（默认 256）\n"
      u8"  -t, --threads N    线程数（默认 4）\n"
      u8"  -ub N              预填充批大小（默认 4）\n"
      u8"  --temp F           温度 0..1（默认 0.70）\n"
      u8"  --top-k N          top-k（默认 40）\n"
      u8"  --top-p F          top-p（默认 0.95）\n"
      u8"  --arena MiB        设置 LLAMA_DISK_STREAM_MB（权重内存池大小）\n"
      "\n"
      u8"API：\n"
      u8"  --api-only         不开 TUI，仅提供 API 服务（无需 TTY）\n"
      u8"  --port N           监听端口（默认 4000）\n"
      "\n"
      u8"按键：\n"
      u8"  设置页：上下选择、左右调整、Enter 切换、s 开始、c 返回、q 退出\n"
      u8"  运行页：s 开始、c 取消、i 编辑提示词、q 退出\n",
      u8"disktui：磁碟串流建置的「設定頁 + 執行頁」終端介面，\n"
      u8"         並在同一行程提供 4000 埠、無鑑權的 OpenAI 相容 API\n"
      "\n"
      u8"用法：disktui [-m model.gguf] [選項]\n"
      "      disktui -m model.gguf --api-only [--n-ctx N]\n"
      "\n"
      u8"介面：\n"
      u8"  -m 路徑            要載入的模型（未指定則掃描 $DISKINFER_MODEL_DIRS、\n"
      u8"                     ~/models、/storage/emulated/0/models、/data/local/tmp/llm/models）\n"
      u8"  --lang 語言        介面語言：auto（預設）、en、zh-Hans、zh-Hant\n"
      u8"  --moe              以開啟 MoE 專家快速路徑的狀態啟動\n"
      u8"  --api              以開啟 API 服務的狀態啟動\n"
      u8"  --chat             以開啟聊天範本的狀態啟動（預設：原始補全）\n"
      u8"  -q, --prompt 文字  初始提示詞（預設 \"Hello\"）\n"
      u8"  --sys 文字         開啟聊天範本時使用的系統訊息\n"
      u8"  --raw              原始補全模式（預設）\n"
      "\n"
      u8"推理：\n"
      u8"  -c, --n-ctx N      上下文長度（預設 16，不超過模型上限）\n"
      u8"  -n, --n-predict N  最大生成 token 數（預設 256）\n"
      u8"  -t, --threads N    執行緒數（預設 4）\n"
      u8"  -ub N              預填充批次大小（預設 4）\n"
      u8"  --temp F           溫度 0..1（預設 0.70）\n"
      u8"  --top-k N          top-k（預設 40）\n"
      u8"  --top-p F          top-p（預設 0.95）\n"
      u8"  --arena MiB        設定 LLAMA_DISK_STREAM_MB（權重記憶體池大小）\n"
      "\n"
      u8"API：\n"
      u8"  --api-only         不開 TUI，僅提供 API 服務（無需 TTY）\n"
      u8"  --port N           監聽埠（預設 4000）\n"
      "\n"
      u8"按鍵：\n"
      u8"  設定頁：上下選擇、左右調整、Enter 切換、s 開始、c 返回、q 離開\n"
      u8"  執行頁：s 開始、c 取消、i 編輯提示詞、q 離開\n" },

    { "misc.no_tty",
      "disktui: no interactive terminal detected.\n"
      "\n"
      "the TUI needs a real terminal (stdin and stdout must be a TTY).\n"
      "to run the OpenAI compatible API headless instead, use:\n"
      "\n"
      "    disktui -m /path/to/model.gguf --api-only\n"
      "    disktui -m /path/to/model.gguf --api-only --n-ctx 256\n"
      "\n"
      "the API listens on http://0.0.0.0:4000/v1 (no auth), GET /v1/models\n"
      "is available even when no model is loaded.\n",
      u8"disktui：未检测到交互式终端。\n"
      "\n"
      u8"TUI 需要真实终端（stdin 与 stdout 都必须是 TTY）。\n"
      u8"如果只想后台运行 OpenAI 兼容 API，请使用：\n"
      "\n"
      "    disktui -m /path/to/model.gguf --api-only\n"
      "    disktui -m /path/to/model.gguf --api-only --n-ctx 256\n"
      "\n"
      u8"API 监听 http://0.0.0.0:4000/v1（无鉴权），即使未加载模型，\n"
      u8"GET /v1/models 也可用。\n",
      u8"disktui：未偵測到互動式終端。\n"
      "\n"
      u8"TUI 需要真實終端（stdin 與 stdout 都必須是 TTY）。\n"
      u8"如果只想在背景執行 OpenAI 相容 API，請使用：\n"
      "\n"
      "    disktui -m /path/to/model.gguf --api-only\n"
      "    disktui -m /path/to/model.gguf --api-only --n-ctx 256\n"
      "\n"
      u8"API 監聽 http://0.0.0.0:4000/v1（無鑑權），即使未載入模型，\n"
      u8"GET /v1/models 也可用。\n" },

    { "misc.unknown_arg_fmt", "disktui: unknown argument '%s' (ignored)",
                              u8"disktui: 未知参数 '%s'（已忽略）",
                              u8"disktui: 未知參數 '%s'（已忽略）" },
    { "misc.fatal_fmt",       "disktui: fatal: %s",
                              u8"disktui: 致命错误：%s",
                              u8"disktui: 嚴重錯誤：%s" },
    { "misc.fatal_unknown",   "disktui: fatal: unknown exception",
                              u8"disktui: 致命错误：未知异常",
                              u8"disktui: 嚴重錯誤：未知例外" },
};

// ------------------------------------------------------------ engine strings
static const Entry kEngineEntries[] = {
    { "eng.load_path_fmt",    "[load] %s", u8"[load] 加载 %s", u8"[load] 載入 %s" },
    { "eng.cache_on_fmt",     "[load] read cache on: model %s fits into %s available RAM (buffered io, less flash wear)",
                              u8"[load] 读盘缓存开启：模型 %s 可放入可用内存 %s（buffered，读盘更少更省寿命）",
                              u8"[load] 讀盤快取開啟：模型 %s 可放入可用記憶體 %s（buffered，讀盤更少更省壽命）" },
    { "eng.cache_off_fmt",    "[load] read cache off: model %s > %s available RAM (direct io, no page cache pressure)",
                              u8"[load] 读盘缓存关闭：模型 %s > 可用内存 %s（direct io，避免 page cache 挤占内存）",
                              u8"[load] 讀盤快取關閉：模型 %s > 可用記憶體 %s（direct io，避免 page cache 擠佔記憶體）" },
    { "eng.moe_on",           "[load] moe fast path on (LLAMA_DISK_MOE=1)",
                              u8"[load] MoE 快速路径：开（LLAMA_DISK_MOE=1）",
                              u8"[load] MoE 快速路徑：開（LLAMA_DISK_MOE=1）" },
    { "eng.moe_off",          "[load] moe fast path off (LLAMA_DISK_MOE=0)",
                              u8"[load] MoE 快速路径：关（LLAMA_DISK_MOE=0）",
                              u8"[load] MoE 快速路徑：關（LLAMA_DISK_MOE=0）" },
    { "eng.cannot_open_fmt",  "cannot open '%s'", u8"无法打开 '%s'", u8"無法開啟 '%s'" },
    { "eng.not_gguf_fmt",     "'%s' is not a GGUF file",
                              u8"'%s' 不是 GGUF 文件",
                              u8"'%s' 不是 GGUF 檔案" },
    { "eng.load_threw_fmt",   "model load threw: %s",
                              u8"模型加载异常：%s",
                              u8"模型載入異常：%s" },
    { "eng.load_threw",       "model load threw", u8"模型加载异常", u8"模型載入異常" },
    { "eng.load_failed",      "model load failed", u8"模型加载失败", u8"模型載入失敗" },
    { "eng.ready_fmt",        "[load] model ready in %s: %s",
                              u8"[load] 模型就绪，用时 %s：%s",
                              u8"[load] 模型就緒，耗時 %s：%s" },
    { "eng.info_fmt",         "[load] n_ctx_train %d  n_layer %d  vocab %d  arena %s  direct-io %d  streaming %s",
                              u8"[load] 训练上下文 %d  层数 %d  词表 %d  内存池 %s  直接 IO %d  流式 %s",
                              u8"[load] 訓練上下文 %d  層數 %d  詞表 %d  記憶體池 %s  直接 IO %d  串流 %s" },
    { "eng.no_model",         "(no model)", u8"（无模型）", u8"（無模型）" },
    { "eng.no_model_loaded",  "no model loaded", u8"未加载模型", u8"未載入模型" },
    { "eng.tokenize_failed",  "failed to tokenize the prompt",
                              u8"提示词分词失败",
                              u8"提示詞分詞失敗" },
    { "eng.ctx_too_small",    "context is too small for this prompt",
                              u8"上下文太小，放不下该提示词",
                              u8"上下文太小，放不下該提示詞" },
    { "eng.warn_prompt_trim_fmt", "[warn] prompt %d tok does not fit into ctx %d, keeping the last %d",
                              u8"[warn] 提示词 %d tok 超出上下文 %d，仅保留最后 %d",
                              u8"[warn] 提示詞 %d tok 超出上下文 %d，僅保留最後 %d" },
    { "eng.warn_npredict_fmt", "[warn] ctx %d leaves room for %d generated tokens",
                              u8"[warn] 上下文 %d 只剩 %d 个生成 token 的空间",
                              u8"[warn] 上下文 %d 只剩 %d 個生成 token 的空間" },
    { "eng.ctx_create_failed", "failed to create a llama_context",
                              u8"创建 llama_context 失败",
                              u8"建立 llama_context 失敗" },
    { "eng.decode_failed_fmt", "decode failed at token %d",
                              u8"在第 %d 个 token 处解码失败",
                              u8"在第 %d 個 token 處解碼失敗" },
    { "eng.prefill_failed_fmt", "prefill decode failed at token %d",
                              u8"预填充在第 %d 个 token 处失败",
                              u8"預填充在第 %d 個 token 處失敗" },
    { "eng.pp_fmt",           "[pp] %d/%d tok  io %.1fs  cmp %.1fs  '%s'",
                              "[pp] %d/%d tok  io %.1fs  cmp %.1fs  '%s'",
                              "[pp] %d/%d tok  io %.1fs  cmp %.1fs  '%s'" },
    { "eng.pp_done_fmt",      "[pp] prompt done: %d tok in %s (%.2f s/tok), io %s",
                              u8"[pp] 预填充完成：%d tok，用时 %s（%.2f s/tok），io %s",
                              u8"[pp] 預填充完成：%d tok，耗時 %s（%.2f s/tok），io %s" },
    { "eng.cancel_prefill",   "[cancel] user cancelled during prefill",
                              u8"[cancel] 用户取消（预填充阶段）",
                              u8"[cancel] 使用者取消（預填充階段）" },
    { "eng.cancel_gen_fmt",   "[cancel] user cancelled after %d tokens",
                              u8"[cancel] 用户取消，已生成 %d 个 token",
                              u8"[cancel] 使用者取消，已生成 %d 個 token" },
    { "eng.gen_line_fmt",     "[gen] t%-4d io %5.1fs cmp %5.1fs tot %5.1fs '%s'",
                              "[gen] t%-4d io %5.1fs cmp %5.1fs tot %5.1fs '%s'",
                              "[gen] t%-4d io %5.1fs cmp %5.1fs tot %5.1fs '%s'" },
    { "eng.gen_done_fmt",     "[gen] done: %d tok in %s (%.2f tok/s)  finish=%s  io %s  cmp %s",
                              u8"[gen] 完成：%d tok，用时 %s（%.2f tok/s）  结束=%s  io %s  cmp %s",
                              u8"[gen] 完成：%d tok，耗時 %s（%.2f tok/s）  結束=%s  io %s  cmp %s" },
    { "eng.finish_stop",      "stop", u8"停止", u8"停止" },
    { "eng.finish_length",    "length", u8"达到长度", u8"達到長度" },
    { "eng.finish_cancelled", "cancelled", u8"已取消", u8"已取消" },
    { "eng.finish_error",     "error", u8"错误", u8"錯誤" },
};

// --------------------------------------------------------------- api strings
// (HTTP JSON "message" fields stay English on purpose: clients and tests rely on
// them; only the log lines and the TUI visible text are localised)
static const Entry kApiEntries[] = {
    { "api.log_get_fmt",       "[api] GET %s from %s",
                               u8"[api] 收到 GET %s，来自 %s",
                               u8"[api] 收到 GET %s，來自 %s" },
    { "api.log_post_chat_fmt", "[api] POST /v1/chat/completions from %s (%zu bytes)",
                               u8"[api] 收到 POST /v1/chat/completions，来自 %s（%zu 字节）",
                               u8"[api] 收到 POST /v1/chat/completions，來自 %s（%zu 位元組）" },
    { "api.log_chat_req_fmt",  "[api] chat: %zu messages, max_tokens %d, temp %.2f",
                               u8"[api] 对话请求：%zu 条消息，max_tokens %d，温度 %.2f",
                               u8"[api] 對話請求：%zu 則訊息，max_tokens %d，溫度 %.2f" },
    { "api.log_post_comp_fmt", "[api] POST /v1/completions from %s",
                               u8"[api] 收到 POST /v1/completions，来自 %s",
                               u8"[api] 收到 POST /v1/completions，來自 %s" },
    { "api.log_comp_req_fmt",  "[api] completion: prompt %zu bytes, max_tokens %d",
                               u8"[api] 补全请求：提示词 %zu 字节，max_tokens %d",
                               u8"[api] 補全請求：提示詞 %zu 位元組，max_tokens %d" },
    { "api.log_listening_fmt", "[api] listening on http://%s:%d/v1  (no auth, concurrency 1)",
                               u8"[api] 正在监听 http://%s:%d/v1（无鉴权，单并发）",
                               u8"[api] 正在監聽 http://%s:%d/v1（無鑑權，單並發）" },
    { "api.log_exception_fmt", "[api] exception in %s: %s",
                               u8"[api] %s 处理异常：%s",
                               u8"[api] %s 處理例外：%s" },
    { "api.bind_failed_fmt",   "cannot bind %s:%d (is another disktui or server already using the port?)",
                               u8"无法绑定 %s:%d（是否已有 disktui 或其他服务占用该端口？）",
                               u8"無法綁定 %s:%d（是否已有 disktui 或其他服務佔用該埠？）" },

    { "aonly.banner_fmt",      "diskinfer api-only  (pid %d)",
                               u8"diskinfer API 模式（pid %d）",
                               u8"diskinfer API 模式（pid %d）" },
    { "aonly.use_first_fmt",   "[api] no -m given, using the first model found: %s",
                               u8"[api] 未指定 -m，使用找到的第一个模型：%s",
                               u8"[api] 未指定 -m，使用找到的第一個模型：%s" },
    { "aonly.no_model_dirs",   "[api] no .gguf model found in: ",
                               u8"[api] 未在以下目录找到 .gguf 模型：",
                               u8"[api] 未在以下目錄找到 .gguf 模型：" },
    { "aonly.no_model_note",   "[api] starting without a model: /v1/models returns an empty list, inference returns 503",
                               u8"[api] 未加载模型启动：/v1/models 返回空列表，推理请求返回 503",
                               u8"[api] 未載入模型啟動：/v1/models 傳回空清單，推理請求傳回 503" },
    { "aonly.load_failed_fmt", "! model load failed: %s",
                               u8"! 模型加载失败：%s",
                               u8"! 模型載入失敗：%s" },
    { "aonly.continue_fmt",    "[api] continuing without a model (503 on inference)",
                               u8"[api] 继续以无模型状态运行（推理返回 503）",
                               u8"[api] 繼續以無模型狀態執行（推理傳回 503）" },
    { "aonly.listening_fmt",   "diskinfer: listening on http://0.0.0.0:%d/v1  (no auth, one inference at a time)",
                               u8"diskinfer: 正在监听 http://0.0.0.0:%d/v1（无鉴权，同一时间一个推理）",
                               u8"diskinfer: 正在監聽 http://0.0.0.0:%d/v1（無鑑權，同一時間一個推理）" },
    { "aonly.model_fmt",       "  model      : %s", u8"  模型       : %s", u8"  模型       : %s" },
    { "aonly.ctx_fmt",         "  n_ctx      : %d   n_predict %d   threads %d   ubatch %d   temp %.2f",
                               u8"  上下文     : %d   最大生成 %d   线程 %d   批大小 %d   温度 %.2f",
                               u8"  上下文     : %d   最大生成 %d   執行緒 %d   批次 %d   溫度 %.2f" },
    { "aonly.endpoints",       "  endpoints  : GET /v1/models, GET /health, POST /v1/chat/completions, POST /v1/completions",
                               u8"  端点       : GET /v1/models, GET /health, POST /v1/chat/completions, POST /v1/completions",
                               u8"  端點       : GET /v1/models, GET /health, POST /v1/chat/completions, POST /v1/completions" },
    { "aonly.stop_fmt",        "  stop       : send SIGTERM (or SIGINT) to pid %d",
                               u8"  停止       : 向 pid %d 发送 SIGTERM（或 SIGINT）",
                               u8"  停止       : 向 pid %d 傳送 SIGTERM（或 SIGINT）" },
    { "aonly.signal_fmt",      "diskinfer: signal %d, stopping",
                               u8"diskinfer: 收到信号 %d，正在停止",
                               u8"diskinfer: 收到訊號 %d，正在停止" },
    { "aonly.bye",             "diskinfer: bye", u8"diskinfer: 再见", u8"diskinfer: 再見" },
};

// ------------------------------------------------------------------ runtime

std::atomic<int> g_lang{ (int) Lang::En };

const char * env_nonempty(const char * name) {
    const char * v = getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

bool iequals(const char * a, const char * b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (std::tolower((unsigned char) *a) != std::tolower((unsigned char) *b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

const char * pick(const Entry & e, Lang lang) {
    switch (lang) {
        case Lang::ZhHans: return e.hans;
        case Lang::ZhHant: return e.hant;
        default:           return e.en;
    }
}

// {V} in the tables expands to the UI version (set by the build: -DDISKINFER_UI_VERSION)
const char * subst_version(const char * s) {
    if (s == nullptr || strstr(s, "{V}") == nullptr) {
        return s;
    }
    static thread_local std::string buf;
    buf = s;
    const char * tag = DISKINFER_UI_VERSION;
    size_t pos = 0;
    while ((pos = buf.find("{V}", pos)) != std::string::npos) {
        buf.replace(pos, 3, tag);
        pos += strlen(tag);
    }
    return buf.c_str();
}
const char * lookup(const char * key) {
    if (key == nullptr) {
        return "";
    }
    const Lang lang = (Lang) g_lang.load();
    for (const auto & e : kUiEntries) {
        if (strcmp(e.key, key) == 0) {
            return subst_version(pick(e, lang));
        }
    }
    for (const auto & e : kEngineEntries) {
        if (strcmp(e.key, key) == 0) {
            return subst_version(pick(e, lang));
        }
    }
    for (const auto & e : kApiEntries) {
        if (strcmp(e.key, key) == 0) {
            return subst_version(pick(e, lang));
        }
    }
    return key;
}

// "zh_TW.UTF-8" -> "zh_tw", "en-US@mod" -> "en-us": lowercase, ascii only
std::string normalize_tag(const char * tag) {
    std::string out;
    if (tag == nullptr) {
        return out;
    }
    for (const char * p = tag; *p; ++p) {
        const unsigned char c = (unsigned char) *p;
        if (c == '.' || c == '@') {
            break;   // drop encoding / modifier
        }
        if (c == '_' || c == '-') {
            out += '-';
        } else if (std::isalnum(c)) {
            out += (char) std::tolower(c);
        }
    }
    return out;
}

bool contains(const std::string & hay, const char * needle) {
    return hay.find(needle) != std::string::npos;
}

#if defined(_WIN32)
std::string wide_to_utf8(const wchar_t * w) {
    if (w == nullptr) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out((size_t) need - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, nullptr, nullptr);
    return out;
}
#endif

// the platform's own locale, lower priority than DISKINFER_LANG
std::string system_tag() {
#if defined(_WIN32)
    // LANG/LC_ALL first (tests and launchers set them), then the OS locale
    if (const char * v = env_nonempty("LANG")) {
        return v;
    }
    if (const char * v = env_nonempty("LC_ALL")) {
        return v;
    }
    wchar_t buf[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) {
        return wide_to_utf8(buf);
    }
    return std::string();
#elif defined(__ANDROID__)
    // the phone's language is the product locale. Termux may export its own
    // LANG (en_US.UTF-8 by default) which has nothing to do with the device
    // setting, so LANG/LC_ALL are only a fallback when the properties are
    // missing; --lang and DISKINFER_LANG (checked earlier) are the overrides
    char prop[PROP_VALUE_MAX];
    memset(prop, 0, sizeof(prop));
    if (__system_property_get("persist.sys.locale", prop) > 0 && prop[0] != '\0') {
        return prop;
    }
    memset(prop, 0, sizeof(prop));
    if (__system_property_get("ro.product.locale", prop) > 0 && prop[0] != '\0') {
        return prop;
    }
    if (const char * v = env_nonempty("LANG")) {
        return v;
    }
    if (const char * v = env_nonempty("LC_ALL")) {
        return v;
    }
    return std::string();
#else
    if (const char * v = env_nonempty("LC_ALL")) {
        return v;
    }
    if (const char * v = env_nonempty("LC_MESSAGES")) {
        return v;
    }
    if (const char * v = env_nonempty("LANG")) {
        return v;
    }
    // /etc/default/locale: LANG=zh_CN.UTF-8
    if (FILE * f = fopen("/etc/default/locale", "r")) {
        char line[512];
        std::string found;
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (strncmp(line, "LANG=", 5) == 0) {
                std::string v = line + 5;
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == '"' || v.back() == ' ')) {
                    v.pop_back();
                }
                if (!v.empty() && v.front() == '"') {
                    v.erase(v.begin());
                }
                found = v;
                break;
            }
        }
        fclose(f);
        if (!found.empty()) {
            return found;
        }
    }
    return std::string();
#endif
}

} // namespace

Lang from_tag(const char * tag) {
    const std::string t = normalize_tag(tag);
    if (t.rfind("zh", 0) == 0) {
        if (contains(t, "hant") || contains(t, "tw") || contains(t, "hk") || contains(t, "mo")) {
            return Lang::ZhHant;
        }
        return Lang::ZhHans;
    }
    return Lang::En;
}

Lang detect() {
    if (const char * v = env_nonempty("DISKINFER_LANG")) {
        if (!iequals(v, "auto")) {
            return from_tag(v);
        }
    }
    const std::string tag = system_tag();
    if (tag.empty()) {
        return Lang::En;
    }
    return from_tag(tag.c_str());
}

void init(int argc, char ** argv) {
    std::string cli;
    if (argv != nullptr) {
        int w = 0;
        for (int r = 0; r < argc; ++r) {
            if (argv[r] == nullptr) {
                continue;
            }
            const std::string a = argv[r];
            bool drop = false;
            if (a == "--lang") {
                if (r + 1 < argc && argv[r + 1] != nullptr) {
                    cli = argv[r + 1];
                    ++r;
                }
                drop = true;
            } else if (a.rfind("--lang=", 0) == 0) {
                cli = a.substr(7);
                drop = true;
            }
            if (drop) {
                continue;
            }
            argv[w++] = argv[r];
        }
        for (int i = w; i < argc; ++i) {
            argv[i] = nullptr;
        }
    }

    if (!cli.empty() && !iequals(cli.c_str(), "auto")) {
        g_lang.store((int) from_tag(cli.c_str()));
        return;
    }
    if (const char * v = env_nonempty("DISKINFER_LANG")) {
        if (!iequals(v, "auto")) {
            g_lang.store((int) from_tag(v));
            return;
        }
    }
    const std::string tag = system_tag();
    g_lang.store((int) (tag.empty() ? Lang::En : from_tag(tag.c_str())));
}

Lang current() {
    return (Lang) g_lang.load();
}

const char * lang_id() {
    switch (current()) {
        case Lang::ZhHans: return "zh-Hans";
        case Lang::ZhHant: return "zh-Hant";
        default:           return "en";
    }
}

const char * tr(const char * key) {
    return lookup(key);
}

std::string tr(const std::string & key) {
    return std::string(lookup(key.c_str()));
}

std::string trf(const char * key, ...) {
    const char * fmt = lookup(key);
    char buf[1024];
    va_list ap;
    va_start(ap, key);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return std::string(fmt);
    }
    const size_t len = (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf) - 1;
    return std::string(buf, len);
}

} // namespace i18n
