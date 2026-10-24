// disktui: single turn chat UI for the disk streaming build
//
// layout: the upper part of the terminal is a scrolling log, the last three rows are
//         Q / A / status. every generated token reports when its disk read finished and
//         when its compute finished.

#include "llama.h"
#include "ggml-cpu.h"
#include "llama-disk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------- terminal

static int T_ROWS = 18;
static int T_COLS = 46;
static int LOG_ROWS = 15;
static std::deque<std::string> g_log;
static std::recursive_mutex g_paint_mtx;

static std::string g_q;       // current question
static std::string g_a;       // answer so far
static std::string g_status;  // bottom status line

static long rss_kb() {
#if defined(_WIN32)
    return -1;
#else
    FILE * f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) { fclose(f); return -1; }
    fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

static void term_size() {
#if defined(_WIN32)
    return;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        T_ROWS = ws.ws_row;
        T_COLS = ws.ws_col;
    }
#endif
    LOG_ROWS = std::max(4, T_ROWS - 3);
}

// ---- utf-8 aware text handling: never cut a code point in half and measure real columns
static int utf8_decode(const std::string & s, size_t i, uint32_t * cp) {
    const unsigned char c = (unsigned char) s[i];
    if (c < 0x80) { *cp = c; return 1; }

    int n = 0;
    uint32_t v = 0;
    if      ((c & 0xE0) == 0xC0) { n = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; v = c & 0x07; }
    else { *cp = '?'; return 1; }

    if (i + (size_t) n >= s.size()) { *cp = '?'; return 1; }
    for (int k = 1; k <= n; ++k) {
        const unsigned char cc = (unsigned char) s[i + k];
        if ((cc & 0xC0) != 0x80) { *cp = '?'; return 1; }
        v = (v << 6) | (cc & 0x3F);
    }
    *cp = v;
    return n + 1;
}

// terminal cells a code point occupies
static int cp_width(uint32_t cp) {
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;               // control
    if (cp >= 0x0300 && cp <= 0x036F) return 0;                        // combining
    if (cp == 0x200B || (cp >= 0x200C && cp <= 0x200F)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||                              // hangul jamo
        (cp >= 0x2E80 && cp <= 0xA4CF) ||                              // cjk, kana
        (cp >= 0xAC00 && cp <= 0xD7A3) ||                              // hangul
        (cp >= 0xF900 && cp <= 0xFAFF) ||                              // cjk compat
        (cp >= 0xFE30 && cp <= 0xFE6F) ||                              // cjk forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||                              // fullwidth
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F9FF)) {                            // emoji
        return 2;
    }
    return 1;
}

static int disp_width(const std::string & s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        uint32_t cp = 0;
        i += utf8_decode(s, i, &cp);
        w += cp_width(cp);
    }
    return w;
}

// cut to at most n display columns, keep the tail if keep_tail
static std::string clip(const std::string & s, int n, bool keep_tail) {
    if (n <= 0) {
        return "";
    }
    if (disp_width(s) <= n) {
        return s;
    }

    if (!keep_tail) {
        std::string out;
        int w = 0;
        for (size_t i = 0; i < s.size(); ) {
            uint32_t cp = 0;
            const int len = utf8_decode(s, i, &cp);
            const int cw = cp_width(cp);
            if (w + cw > n) {
                break;
            }
            out.append(s, i, len);
            w += cw;
            i += len;
        }
        return out;
    }

    // keep the tail: walk backwards, then trim to a code point boundary
    size_t start = s.size();
    int w = 0;
    while (start > 0) {
        size_t begin = start - 1;
        while (begin > 0 && ((unsigned char) s[begin] & 0xC0) == 0x80) {
            --begin;
        }
        uint32_t cp = 0;
        utf8_decode(s, begin, &cp);
        const int cw = cp_width(cp);
        if (w + cw > n) {
            break;
        }
        w += cw;
        start = begin;
    }
    return s.substr(start);
}

static void out(const char * fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
}

// paint one line padded to the terminal width so leftovers disappear
// drop control characters, turn tabs into spaces and replace invalid utf-8 bytes,
// so a garbage token cannot move the cursor or print broken glyphs
static std::string sanitize(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char) s[i];
        if (c == '\t') {
            out += "    ";
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += (char) c;
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const int len = utf8_decode(s, i, &cp);
        if (len == 1) {
            out += '?';   // invalid lead or truncated sequence
            ++i;
        } else {
            out.append(s, i, len);
            i += len;
        }
    }
    return out;
}

static void paint_row(int row, const std::string & text) {
    out("\x1b[%d;1H\x1b[2K%s", row, clip(sanitize(text), T_COLS, false).c_str());
}

static void paint_all() {
    std::lock_guard<std::recursive_mutex> lk(g_paint_mtx);

    const int n_line  = (int) g_log.size();
    const int i_start = n_line > LOG_ROWS ? n_line - LOG_ROWS : 0;
    for (int i = 0; i < LOG_ROWS; ++i) {
        const int idx = i_start + i;
        paint_row(1 + i, idx < n_line ? g_log[idx] : std::string());
    }

    paint_row(LOG_ROWS + 1, "Q: " + clip(g_q, T_COLS - 3, false));
    paint_row(LOG_ROWS + 2, "A: " + clip(g_a, T_COLS - 3, true));
    paint_row(LOG_ROWS + 3, clip(g_status, T_COLS, false));

    fflush(stdout);
}

// add a log line, wrapping it to the terminal width
static void logf(const char * fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::recursive_mutex> lk(g_paint_mtx);

    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }

    const int width = std::max(8, T_COLS - 1);

    // wrap on code point boundaries and real column width
    std::string cur;
    int cur_w = 0;
    for (size_t i = 0; i < s.size(); ) {
        uint32_t cp = 0;
        const int len = utf8_decode(s, i, &cp);
        const int cw  = cp_width(cp);
        if (cur_w + cw > width && !cur.empty()) {
            g_log.push_back(cur);
            cur.clear();
            cur_w = 0;
        }
        cur.append(s, i, len);
        cur_w += cw;
        i += len;
    }
    if (!cur.empty() || s.empty()) {
        g_log.push_back(cur);
    }
    while ((int) g_log.size() > LOG_ROWS) {
        g_log.pop_front();
    }

    static FILE * g_logfile = nullptr;
    static bool g_logfile_tried = false;
    if (!g_logfile_tried) {
        g_logfile_tried = true;
        const char * lp = getenv("LLAMA_DISK_LOG");
        if (lp != nullptr) {
            g_logfile = fopen(lp, "w");
        }
        if (g_logfile == nullptr) {
            const char * home = getenv("HOME");
            if (home != nullptr) {
                const std::string p = std::string(home) + "/diskinfer.log";
                g_logfile = fopen(p.c_str(), "w");
            }
        }
    }
    if (g_logfile != nullptr) {
        fputs(s.c_str(), g_logfile);
        fputc('\n', g_logfile);
        fflush(g_logfile);
    }
}

static void set_status(const std::string & s) {
    std::lock_guard<std::recursive_mutex> lk(g_paint_mtx);
    g_status = s;
}

static int64_t now_ms();

// ---- live progress, painted by a helper thread while a decode is running
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_quit{false};
static std::atomic<int>  g_live_cur{0};
static std::atomic<int>  g_live_tot{0};
static std::mutex        g_live_mtx;
static char              g_live_piece[64] = {0};
static const char *      g_live_phase = "pp";
static int64_t           g_live_t0 = 0;
static unsigned long long g_live_io0 = 0;

static void paint_status_only() {
    std::lock_guard<std::recursive_mutex> lk(g_paint_mtx);
    paint_row(LOG_ROWS + 3, clip(g_status, T_COLS, false));
    fflush(stdout);
}

static void live_begin(const char * phase, int cur, int tot, const char * piece) {
    llama_disk::llama_disk_stats s;
    llama_disk::get_stats(&s);

    {
        std::lock_guard<std::mutex> lk(g_live_mtx);
        snprintf(g_live_piece, sizeof(g_live_piece), "%s", piece);
    }
    g_live_phase = phase;
    g_live_cur   = cur;
    g_live_tot   = tot;
    g_live_io0   = s.io_us;
    g_live_t0    = now_ms();
    g_busy       = true;
}

static void live_end() {
    g_busy = false;
}

static std::thread * g_painter = nullptr;

static void tui_exit(int code) {
    g_quit = true;
    if (g_painter != nullptr && g_painter->joinable()) {
        g_painter->join();
    }
    out("\x1b[?25h\x1b[?1049l");
    fflush(stdout);
    exit(code);
}

static void painter_main() {
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!g_busy) {
            continue;
        }

        llama_disk::llama_disk_stats s;
        llama_disk::get_stats(&s);

        const double el  = (now_ms() - g_live_t0) / 1000.0;
        const double io  = (s.io_us - g_live_io0) / 1e6;
        const double cmp = el - io;

        char piece[64];
        {
            std::lock_guard<std::mutex> lk(g_live_mtx);
            snprintf(piece, sizeof(piece), "%s", g_live_piece);
        }

        char buf[190];
        snprintf(buf, sizeof(buf), "%s %d/%d '%s' L%02d io %.1f/%.1fs ld%llu now %llu cmp %.1fs",
                 g_live_phase, g_live_cur.load(), g_live_tot.load(), piece,
                 s.layer + 1, io, s.io_us / 1e6, s.loads,
                 s.clock_us / 1000000, cmp);

        set_status(buf);
        paint_status_only();
    }
}

// route the llama.cpp log into the log area, drop the noisy ones
static void llama_log_cb(enum ggml_log_level level, const char * text, void * /*ud*/) {
    if (text == nullptr) {
        return;
    }
    std::string s(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    if (s.empty()) {
        return;
    }

    // skip the per layer kv/recurrent device dumps and the metadata key/value dump
    if (s.find("llama_kv_cache: layer") != std::string::npos ||
        s.find("llama_memory_recurrent, layer") != std::string::npos ||
        s.find("llama_memory_recurrent: layer") != std::string::npos ||
        s.find("is used by node") != std::string::npos ||
        s.find("create_tensor: loading tensor") != std::string::npos) {
        return;
    }

    if (level == GGML_LOG_LEVEL_ERROR) {
        s = "! " + s;
    }

    logf("%s", s.c_str());
}

static void disk_msg_cb(const char * text) {
    if (text == nullptr) {
        return;
    }
    logf("%s", text);
}

// ---------------------------------------------------------------- engine

static int64_t now_ms() {
#if defined(_WIN32)
    return 0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static std::string fmt_clock(int64_t ms) {
    char buf[32];
    const int64_t s = ms / 1000;
    snprintf(buf, sizeof(buf), "%2lld:%02lld", (long long) (s / 60), (long long) (s % 60));
    return buf;
}

struct opts {
    std::string model;
    std::string question;
    int n_ctx     = 256;
    int n_ubatch  = 4;
    int n_threads = 4;
    int n_predict = 256;
    float temp    = 1.0f;
    int top_k     = 20;
    float top_p   = 0.95f;
    bool  raw     = true;
    std::string sysmsg;
};

int main(int argc, char ** argv) {
    opts o;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-m" && i + 1 < argc)         { o.model = argv[++i]; }
        else if (a == "-c" && i + 1 < argc)    { o.n_ctx = atoi(argv[++i]); }
        else if (a == "-ub" && i + 1 < argc)   { o.n_ubatch = atoi(argv[++i]); }
        else if (a == "-t" && i + 1 < argc)    { o.n_threads = atoi(argv[++i]); }
        else if (a == "-n" && i + 1 < argc)    { o.n_predict = atoi(argv[++i]); }
        else if (a == "--temp" && i + 1 < argc){ o.temp = (float) atof(argv[++i]); }
        else if (a == "--top-k" && i + 1 < argc){ o.top_k = atoi(argv[++i]); }
        else if (a == "--top-p" && i + 1 < argc){ o.top_p = (float) atof(argv[++i]); }
        else if (a == "-q" && i + 1 < argc)    { o.question = argv[++i]; }
        else if (a == "--debug")                { setenv("LLAMA_DISK_DEBUG", "1", 1); }
        else if (a == "--raw")                  { o.raw = true; }
        else if (a == "--chat")                 { o.raw = false; }
        else if (a == "--sys" && i + 1 < argc)  { o.sysmsg = argv[++i]; }
        else if (!a.empty() && a[0] != '-')    { o.question = a; }
    }

    if (o.model.empty()) {
        fprintf(stderr, "usage: disktui -m model.gguf [-c 256] [-ub 4] [-t 4] [-n 256] [-q question]\n");
        return 1;
    }

    // defaults for the disk streaming layer, the caller can override any of them
    auto set_default_env = [](const char * k, const char * v) {
        if (getenv(k) == nullptr) {
            setenv(k, v, 1);
        }
    };
    set_default_env("LLAMA_DISK_ODIRECT",   "1");
    set_default_env("LLAMA_DISK_MLOCK",     "1");
    set_default_env("LLAMA_DISK_PREFETCH",  "0");
    set_default_env("LLAMA_DISK_WINDOW",    "0");
    set_default_env("LLAMA_DISK_OUT_CHUNK", "0");
    set_default_env("GGML_CPU_DISABLE_FUSION", "1");

    term_size();

    // alternate screen, hide cursor
    out("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l");
    fflush(stdout);

    llama_log_set(llama_log_cb, nullptr);
    llama_disk::set_msg_cb(disk_msg_cb);

    set_status("loading model ...");
    paint_all();

    g_painter = new std::thread(painter_main);

    logf("diskinfer  %s", o.model.c_str());
    logf("ctx %d  prefill ub %d  threads %d  temp %.2f  top_k %d  top_p %.2f  single turn",
         o.n_ctx, o.n_ubatch, o.n_threads, o.temp, o.top_k, o.top_p);

    llama_backend_init();

    const int64_t t_start = now_ms();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;

    // check the file before handing it to the loader: a bad path would crash inside
    // llama.cpp's error path instead of reporting anything useful
    {
        FILE * f = fopen(o.model.c_str(), "rb");
        if (f == nullptr) {
            logf("! cannot open '%s'", o.model.c_str());
            paint_all();
            tui_exit(1);
        }
        char magic[4] = { 0, 0, 0, 0 };
        const size_t n = fread(magic, 1, 4, f);
        fclose(f);
        if (n != 4 || memcmp(magic, "GGUF", 4) != 0) {
            logf("! '%s' is not a GGUF file", o.model.c_str());
            paint_all();
            tui_exit(1);
        }
    }

    llama_model * model = nullptr;
    try {
        model = llama_model_load_from_file(o.model.c_str(), mp);
    } catch (const std::exception & e) {
        logf("! %s", e.what());
    } catch (...) {
        logf("! model load threw");
    }
    if (model == nullptr) {
        logf("! model load failed");
        paint_all();
        tui_exit(1);
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);

    {
        char desc[512];
        llama_model_desc(model, desc, sizeof(desc));
        logf("[%5.1fs] model ready: %s", (now_ms() - t_start) / 1000.0, desc);
        logf("size %.2f GiB  params %.2f B  n_layer %d  n_embd %d  n_ctx_train %d",
             llama_model_size(model) / 1073741824.0,
             llama_model_n_params(model) / 1e9,
             llama_model_n_layer(model),
             llama_model_n_embd(model),
             llama_model_n_ctx_train(model));
        logf("vocab %u tokens  rss %ld MiB",
             llama_vocab_n_tokens(vocab), rss_kb() / 1024);
    }

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx      = o.n_ctx;
    cp.n_batch    = o.n_ubatch;
    cp.n_ubatch   = o.n_ubatch;
    cp.n_threads  = o.n_threads;
    cp.n_threads_batch = o.n_threads;
    cp.no_perf    = false;

    llama_context * ctx = llama_init_from_model(model, cp);
    if (ctx == nullptr) {
        logf("! context creation failed");
        paint_all();
        tui_exit(1);
    }
    logf("[%5.1fs] context ready  rss %ld MiB", (now_ms() - t_start) / 1000.0, rss_kb() / 1024);
    set_status("ready. type a question, Enter to run");
    paint_all();

    // samplers: top_k -> top_p -> temp -> dist
    auto sp = llama_sampler_chain_default_params();
    llama_sampler * smpl = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(o.top_k));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(o.top_p, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(o.temp));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    llama_batch batch = llama_batch_init(o.n_ubatch, 0, 1);

    // ------------------------------------------------ one single turn
    std::string q = o.question;

    for (;;) {
        if (q.empty()) {
            set_status("type a question, Enter to run  (Ctrl-C to quit)");
            g_a.clear();
            paint_all();

            // let the terminal echo the input on the Q row
            out("\x1b[%d;1H\x1b[2KQ: \x1b[?25h", LOG_ROWS + 1);
            fflush(stdout);

            char line[1024];
            if (fgets(line, sizeof(line), stdin) == nullptr) {
                break;
            }
            out("\x1b[?25l");
            q = line;
            while (!q.empty() && (q.back() == '\n' || q.back() == '\r')) {
                q.pop_back();
            }
            if (q.empty()) {
                continue;
            }
        }

        g_q = q;
        g_a.clear();
        set_status("preparing ...");
        paint_all();

        logf("---- turn ----");
        logf("prompt: %s", q.c_str());

        // raw completion by default for base style checkpoints, chat template with --chat
        std::string prompt;
        if (o.raw) {
            prompt = q;
        } else {
            prompt = "";
            if (!o.sysmsg.empty()) {
                prompt += "<|im_start|>system\n" + o.sysmsg + "<|im_end|>\n";
            }
            prompt += "<|im_start|>user\n" + q + "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
        }
        logf("prompt mode: %s", o.raw ? "raw completion" : "chat template (thinking off)");

        const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
        if (n_prompt <= 0) {
            logf("! tokenize failed");
            q.clear();
            continue;
        }
        if (n_prompt + o.n_predict >= o.n_ctx) {
            o.n_predict = std::max(1, o.n_ctx - n_prompt - 1);
            logf("ctx full: n_predict limited to %d", o.n_predict);
        }

        std::vector<llama_token> toks(n_prompt);
        if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), toks.data(), toks.size(), true, true) < 0) {
            logf("! tokenize failed");
            q.clear();
            continue;
        }

        logf("prompt = %d tokens  (%s)", n_prompt, o.raw ? "raw completion" : "chat template, thinking off");

        // context is cleared for every turn: single turn only
        llama_memory_clear(llama_get_memory(ctx), true);

        int n_past = 0;
        int64_t t_pp0 = now_ms();
        int64_t io_pp = 0;

        set_status("prompt processing ...");
        paint_all();

        for (int i = 0; i < n_prompt; i += o.n_ubatch) {
            const int n = std::min(o.n_ubatch, n_prompt - i);

            batch.n_tokens = n;
            for (int j = 0; j < n; ++j) {
                batch.token[j]    = toks[i + j];
                batch.pos[j]      = i + j;
                batch.n_seq_id[j] = 1;
                batch.seq_id[j][0] = 0;
                batch.logits[j]   = (i + j == n_prompt - 1);
            }

            char piece[256] = {0};
            int np = llama_token_to_piece(vocab, toks[i], piece, sizeof(piece), 0, false);
            if (np <= 0) {
                snprintf(piece, sizeof(piece), "#%d", (int) toks[i]);
                np = (int) strlen(piece);
            }

            llama_disk::llama_disk_stats s0, s1;
            llama_disk::get_stats(&s0);
            const int64_t t0 = now_ms();

            live_begin("pp", i + 1, n_prompt, piece);
            const int rc = llama_decode(ctx, batch);
            live_end();

            const int64_t t1 = now_ms();
            llama_disk::get_stats(&s1);

            const double io_s = (s1.io_us - s0.io_us) / 1e6;
            const double tot_s = (t1 - t0) / 1000.0;
            io_pp += (int64_t) (s1.io_us - s0.io_us);

            if (rc != 0) {
                logf("! decode failed at %d (rc %d)", i, rc);
                break;
            }

            logf("[%s] pp %d-%d/%d  io %.1fs  cmp %.1fs  '%.*s'",
                 fmt_clock(t1 - t_pp0).c_str(), i + 1, i + n, n_prompt,
                 io_s, tot_s - io_s, np, piece);

            n_past = i + n;
        }

        const int64_t t_pp1 = now_ms();
        const double pp_s = (t_pp1 - t_pp0) / 1000.0;
        logf("[%s] prompt done: %d tok in %.1fs (%.1f s/tok), io %.1fs",
             fmt_clock(t_pp1 - t_pp0).c_str(), n_prompt, pp_s, pp_s / n_prompt, io_pp / 1e6);
        paint_all();

        // ------------------------------------------------ generation
        const int64_t t_gen0 = now_ms();
        int n_gen = 0;
        int64_t io_gen = 0;

        for (int step = 0; step < o.n_predict; ++step) {
            const int64_t t0 = now_ms();
            const llama_token tok = llama_sampler_sample(smpl, ctx, -1);

            if (llama_vocab_is_eog(vocab, tok)) {
                logf("[%s] eog after %d tokens", fmt_clock(now_ms() - t_gen0).c_str(), n_gen);
                break;
            }

            char piece[256] = {0};
            int np = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
            if (np < 0) {
                np = 0;
            }
            g_a.append(piece, np);
            paint_all();

            if (n_past + 1 >= o.n_ctx) {
                logf("ctx full at %d tokens", n_gen + 1);
                break;
            }

            batch.n_tokens = 1;
            batch.token[0] = tok;
            batch.pos[0]   = n_past;
            batch.n_seq_id[0] = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0] = 1;
            n_past++;

            llama_disk::llama_disk_stats s0, s1;
            llama_disk::get_stats(&s0);

            live_begin("t", n_gen + 1, o.n_predict, piece);
            const int rc = llama_decode(ctx, batch);
            live_end();

            const int64_t t1 = now_ms();
            llama_disk::get_stats(&s1);

            const double io_s = (s1.io_us - s0.io_us) / 1e6;
            const double tot_s = (t1 - t0) / 1000.0;
            io_gen += (int64_t) (s1.io_us - s0.io_us);
            n_gen++;

            if (rc != 0) {
                logf("! decode failed at token %d (rc %d)", n_gen, rc);
                break;
            }

            logf("[%s] t%-3d io %5.1fs  cmp %5.1fs  tot %5.1fs  %s",
                 fmt_clock(t1 - t_gen0).c_str(), n_gen, io_s, tot_s - io_s, tot_s,
                 ("'" + std::string(piece, np) + "'").c_str());

            set_status("generating ... (Ctrl-C to stop)");
        }

        const int64_t t_gen1 = now_ms();
        const double gen_s = (t_gen1 - t_gen0) / 1000.0;

        logf("---- done ----");
        logf("gen %d tok in %.1fs (%.1f s/tok)  io %.1fs  rss %ld MiB",
             n_gen, gen_s, n_gen > 0 ? gen_s / n_gen : 0.0, io_gen / 1e6, rss_kb() / 1024);

        // full answer wrapped into the log so it stays readable
        logf("answer: %s", g_a.c_str());

        {
            const char * home = getenv("HOME");
            if (home != nullptr) {
                std::string p = std::string(home) + "/last_answer.txt";
                if (FILE * fa = fopen(p.c_str(), "w")) {
                    fputs(g_a.c_str(), fa);
                    fclose(fa);
                    logf("saved to %s", p.c_str());
                }
            }
        }

        set_status("done. Enter for a new question (context cleared), Ctrl-C to quit");
        paint_all();

        if (!o.question.empty()) {
            break; // one shot mode
        }
        q.clear();
    }

    llama_batch_free(batch);
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);

    tui_exit(0);
}
