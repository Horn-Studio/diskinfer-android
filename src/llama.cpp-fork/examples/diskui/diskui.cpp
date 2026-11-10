// disktui: settings page + run page TUI for the disk streaming build
//
// the same binary serves an OpenAI compatible, no-auth API on port 4000:
//
//   disktui -m model.gguf              interactive TUI (model picker, settings, run page)
//   disktui -m model.gguf --api-only   headless API server (no TTY needed)
//   disktui --help                     usage
//
// pages:
//   model picker   -> models found in $DISKINFER_MODEL_DIRS, ~/models, /sdcard/models
//   settings page  -> context length, temperature, chat template, MoE, API switch
//   run page       -> prompt editor, prefill/generation progress, streaming answer, log
//
// the engine (engine.cpp) does the llama work and exposes a live_state the run
// page polls; the API (api.cpp) shares the same engine and the same single
// inference slot, so TUI and API can never decode at the same time.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "engine.h"
#include "api.h"
#include "i18n.h"

#include "llama-disk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#else
#include <conio.h>
#include <io.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;




namespace {

// ===========================================================================
// text: utf-8 aware width / clip / wrap
// ===========================================================================

int utf8_decode(const std::string & s, size_t i, uint32_t * cp) {
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

int cp_width(uint32_t cp) {
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp >= 0x0300 && cp <= 0x036F) return 0;
    if (cp == 0x200B || (cp >= 0x200C && cp <= 0x200F)) return 0;
    if (cp == 0xFE0F) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F9FF)) {
        return 2;
    }
    return 1;
}

int disp_width(const std::string & s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        i += utf8_decode(s, i, &cp);
        w += cp_width(cp);
    }
    return w;
}

std::string clip(const std::string & s, int n, bool keep_tail = false) {
    if (n <= 0) {
        return "";
    }
    if (disp_width(s) <= n) {
        return s;
    }
    if (!keep_tail) {
        std::string out;
        int w = 0;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = 0;
            const int len = utf8_decode(s, i, &cp);
            const int cw = cp_width(cp);
            if (w + cw > n) {
                break;
            }
            out.append(s, i, (size_t) len);
            w += cw;
            i += (size_t) len;
        }
        return out;
    }
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

// drop control characters, turn tabs into spaces, repair invalid utf-8 bytes
std::string sanitize(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
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
            out += '?';
            ++i;
        } else {
            out.append(s, i, (size_t) len);
            i += (size_t) len;
        }
    }
    return out;
}

std::string pad_to(const std::string & s, int w) {
    const int d = w - disp_width(s);
    return d > 0 ? s + std::string((size_t) d, ' ') : s;
}

std::string pad_between(const std::string & left, const std::string & right, int total) {
    const int gap = total - disp_width(left) - disp_width(right);
    if (gap <= 0) {
        return clip(left, total, false);
    }
    return left + std::string((size_t) gap, ' ') + right;
}

std::vector<std::string> wrap_text(const std::string & text, int width) {
    std::vector<std::string> out;
    if (width <= 1) {
        width = 2;
    }
    std::string cur;
    int cur_w = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const int len = utf8_decode(text, i, &cp);
        if (cp == '\n') {
            out.push_back(cur);
            cur.clear();
            cur_w = 0;
            i += (size_t) len;
            continue;
        }
        if (cp == '\r') {
            i += (size_t) len;
            continue;
        }
        const int cw = cp_width(cp);
        if (cur_w + cw > width && !cur.empty()) {
            out.push_back(cur);
            cur.clear();
            cur_w = 0;
        }
        cur.append(text, i, (size_t) len);
        cur_w += cw;
        i += (size_t) len;
    }
    out.push_back(cur);
    return out;
}

// ===========================================================================
// terminal
// ===========================================================================

int  g_rows = 24;
int  g_cols = 80;
bool g_compact = false;    // contract: rows < 20 || cols < 60 => compact layout
bool g_narrow  = false;    // cols < 60: no side-by-side hint columns
bool g_short   = false;    // rows < 20: drop sections to keep the essentials

enum style_t {
    S_NORM = 0, S_BOLD, S_DIM, S_CYAN, S_CYAN_BOLD, S_SEL, S_RED, S_GREEN, S_YELLOW, S_BAR,
};

const char * sgr(style_t s) {
    switch (s) {
        case S_BOLD:      return "\x1b[1m";
        case S_DIM:       return "\x1b[2m";
        case S_CYAN:      return "\x1b[36m";
        case S_CYAN_BOLD: return "\x1b[1;36m";
        case S_SEL:       return "\x1b[1;33m";
        case S_RED:       return "\x1b[1;31m";
        case S_GREEN:     return "\x1b[32m";
        case S_YELLOW:    return "\x1b[33m";
        case S_BAR:       return "\x1b[7m";
        default:          return "";
    }
}

struct row {
    std::string text;
    style_t     style = S_NORM;
};

std::mutex       g_paint_mtx;
std::vector<row> g_prev_frame;
bool             g_term_active = false;

#if defined(_WIN32)
HANDLE g_hout = INVALID_HANDLE_VALUE;
HANDLE g_hin  = INVALID_HANDLE_VALUE;
DWORD  g_old_mode = 0;
DWORD  g_old_in_mode = 0;
bool   g_mode_saved = false;
bool   g_in_mode_saved = false;
UINT   g_old_out_cp = 0;
UINT   g_old_in_cp = 0;
#else
struct termios g_old_termios;
bool           g_termios_saved = false;
#endif

void out(const char * fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
}

void term_size() {
#if defined(_WIN32)
    if (g_hout != INVALID_HANDLE_VALUE && g_hout != NULL) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(g_hout, &csbi)) {
            const int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            const int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            if (rows > 0 && cols > 0) {
                g_rows = rows;
                g_cols = cols;
            }
        }
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col;
    }
#endif
    g_compact = (g_rows < 20 || g_cols < 60);
    g_narrow  = (g_cols < 60);
    g_short   = (g_rows < 20);
}

void term_enter() {
#if defined(_WIN32)
    g_old_out_cp = GetConsoleOutputCP();
    g_old_in_cp  = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hout != INVALID_HANDLE_VALUE && g_hout != NULL) {
        DWORD mode = 0;
        if (GetConsoleMode(g_hout, &mode)) {
            g_old_mode = mode;
            g_mode_saved = true;
            SetConsoleMode(g_hout, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    g_hin = GetStdHandle(STD_INPUT_HANDLE);
    if (g_hin != INVALID_HANDLE_VALUE && g_hin != NULL) {
        DWORD mode = 0;
        if (GetConsoleMode(g_hin, &mode)) {
            g_old_in_mode = mode;
            g_in_mode_saved = true;
            SetConsoleMode(g_hin, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
        }
    }
#else
    if (tcgetattr(STDIN_FILENO, &g_old_termios) == 0) {
        g_termios_saved = true;
        struct termios t = g_old_termios;
        t.c_lflag &= ~(ICANON | ECHO | IEXTEN);
        t.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP);
        t.c_oflag &= ~(OPOST);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
#endif
    out("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l");
    fflush(stdout);
    g_term_active = true;
    term_size();
}

void term_leave() {
    if (!g_term_active) {
        return;
    }
#if defined(_WIN32)
    DWORD n = 0;
    if (g_hout != INVALID_HANDLE_VALUE && g_hout != NULL) {
        WriteFile(g_hout, "\x1b[?25h\x1b[?1049l", 17, &n, NULL);
    }
    if (g_in_mode_saved) { SetConsoleMode(g_hin, g_old_in_mode); }
    if (g_mode_saved)    { SetConsoleMode(g_hout, g_old_mode); }
    if (g_old_out_cp != 0) { SetConsoleOutputCP(g_old_out_cp); }
    if (g_old_in_cp != 0)  { SetConsoleCP(g_old_in_cp); }
#else
    out("\x1b[?25h\x1b[?1049l");
    fflush(stdout);
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios);
    }
#endif
    g_term_active = false;
}

void invalidate() {
    std::lock_guard<std::mutex> lk(g_paint_mtx);
    g_prev_frame.clear();
    if (g_term_active) {
        out("\x1b[2J\x1b[H");
        fflush(stdout);
    }
}

void paint_frame(const std::vector<row> & rows) {
    std::lock_guard<std::mutex> lk(g_paint_mtx);

    std::string buf;
    const int n = (int) rows.size();

    for (int i = 0; i < n; ++i) {
        const std::string text = clip(sanitize(rows[(size_t) i].text), g_cols, false);
        if (i < (int) g_prev_frame.size() &&
            g_prev_frame[(size_t) i].text == text &&
            g_prev_frame[(size_t) i].style == rows[(size_t) i].style) {
            continue;
        }
        char pos[32];
        snprintf(pos, sizeof(pos), "\x1b[%d;1H", i + 1);
        buf += pos;
        buf += sgr(rows[(size_t) i].style);
        buf += text;
        buf += "\x1b[0m\x1b[K";
    }
    for (int i = n; i < (int) g_prev_frame.size(); ++i) {
        char pos[32];
        snprintf(pos, sizeof(pos), "\x1b[%d;1H\x1b[K", i + 1);
        buf += pos;
    }

    g_prev_frame.clear();
    g_prev_frame.reserve(rows.size());
    for (const auto & r : rows) {
        g_prev_frame.push_back(row{ clip(sanitize(r.text), g_cols, false), r.style });
    }

    if (!buf.empty()) {
        fwrite(buf.data(), 1, buf.size(), stdout);
        fflush(stdout);
    }
}

// ---- key input -------------------------------------------------------------

enum {
    K_NONE = -1,
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BACKSPACE, K_TAB,
    K_PGUP, K_PGDN, K_HOME, K_END, K_DEL,
};

#if !defined(_WIN32)

bool read_byte(char & c, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd      = STDIN_FILENO;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    const int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) {
        return false;
    }
    return read(STDIN_FILENO, &c, 1) == 1;
}

int read_key(int timeout_ms) {
    char c = 0;
    if (!read_byte(c, timeout_ms)) {
        return K_NONE;
    }
    if (c == '\x1b') {
        char a = 0;
        if (!read_byte(a, 25)) {
            return K_ESC;
        }
        if (a == '[' || a == 'O') {
            char b = 0;
            if (!read_byte(b, 25)) {
                return K_ESC;
            }
            switch (b) {
                case 'A': return K_UP;
                case 'B': return K_DOWN;
                case 'C': return K_RIGHT;
                case 'D': return K_LEFT;
                case 'H': return K_HOME;
                case 'F': return K_END;
                case 'Z': return K_TAB;
                case '3': { char t = 0; read_byte(t, 10); return K_DEL; }
                case '5': { char t = 0; read_byte(t, 10); return K_PGUP; }
                case '6': { char t = 0; read_byte(t, 10); return K_PGDN; }
                default:  return K_NONE;
            }
        }
        return K_ESC;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 127 || c == 8)     return K_BACKSPACE;
    if (c == '\t')              return K_TAB;
    if (c == 3)                 return K_ESC;
    return (unsigned char) c;
}

#else

bool win_kbhit(int timeout_ms) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        if (_kbhit()) {
            return true;
        }
        if ((int) (GetTickCount() - t0) >= timeout_ms) {
            return false;
        }
        Sleep(10);
    }
}

int read_key(int timeout_ms) {
    if (!win_kbhit(timeout_ms)) {
        return K_NONE;
    }
    const int c = _getch();
    if (c == 0 || c == 224) {
        const int e = _getch();
        switch (e) {
            case 72: return K_UP;
            case 80: return K_DOWN;
            case 75: return K_LEFT;
            case 77: return K_RIGHT;
            case 71: return K_HOME;
            case 79: return K_END;
            case 73: return K_PGUP;
            case 81: return K_PGDN;
            case 83: return K_DEL;
            default: return K_NONE;
        }
    }
    if (c == '\r') return K_ENTER;
    if (c == '\b') return K_BACKSPACE;
    if (c == 27)   return K_ESC;
    if (c == 3)    return K_ESC;
    return c;
}

#endif

// ---- platform helpers ------------------------------------------------------

#if defined(_WIN32)
UINT g_saved_out_cp = 0;
UINT g_saved_in_cp  = 0;

// the strings are UTF-8; on Windows the console has to be told so, otherwise
// cmd/powershell shows mojibake. every step is non-fatal when redirected.
void win_console_enter() {
    g_saved_out_cp = GetConsoleOutputCP();
    g_saved_in_cp  = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void win_console_leave() {
    if (g_saved_out_cp != 0) { SetConsoleOutputCP(g_saved_out_cp); }
    if (g_saved_in_cp != 0)  { SetConsoleCP(g_saved_in_cp); }
}
#endif

// stdin and stdout must both be real terminals for the interactive TUI
bool tty_available() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
}

// ---- signals ---------------------------------------------------------------

// the console control handler runs on a separate thread, the POSIX signal
// handler must only touch a sig_atomic_t; both are plain volatile integers here
#if defined(_WIN32)
volatile LONG g_signal = 0;
#else
volatile sig_atomic_t g_signal = 0;
#endif

#if defined(_WIN32)
BOOL WINAPI console_ctrl_handler(DWORD /*type*/) {
    g_signal = 1;
    return TRUE;
}
#else
void posix_signal_handler(int sig) {
    g_signal = sig;
}
#endif

void install_signal_handlers() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
#endif
}

// ===========================================================================
// log routing
// ===========================================================================

std::mutex              g_log_mtx;
std::atomic<bool>       g_cancel_in_progress{ false };
std::deque<std::string> g_logbuf;
bool                    g_headless = false;
const size_t            g_log_max = 500;

void route_log(const std::string & line) {
    if (line.empty()) {
        return;
    }
    if (g_headless) {
        fputs(line.c_str(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return;
    }
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_logbuf.push_back(line);
    while (g_logbuf.size() > g_log_max) {
        g_logbuf.pop_front();
    }
}

void llama_log_cb(enum ggml_log_level level, const char * text, void * /*ud*/) {
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
    // keep the log readable: errors and the disk engine status are useful, the
    // loader / graph dump chatter is not
    if (level == GGML_LOG_LEVEL_ERROR) {
        // an abort callback makes llama_decode return an error; that is the user
        // cancelling, not a real failure, so keep the log clean
        if (g_cancel_in_progress.load()) {
            return;
        }
        route_log("! " + s);
        return;
    }
    if (s.rfind("llama-disk:", 0) == 0 || s.find("warning:") != std::string::npos ||
        s.rfind("[load]", 0) == 0 || s.rfind("[api]", 0) == 0) {
        route_log(s);
    }
}

void disk_msg_cb(const char * text) {
    if (text != nullptr) {
        route_log(text);
    }
}

std::vector<std::string> log_tail(int max_lines, int width) {
    std::vector<std::string> wrapped;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        for (const auto & line : g_logbuf) {
            for (auto & w : wrap_text(line, width)) {
                wrapped.push_back(std::move(w));
            }
        }
    }
    if (max_lines <= 0) {
        return {};
    }
    if ((int) wrapped.size() <= max_lines) {
        return wrapped;
    }
    return std::vector<std::string>(wrapped.end() - max_lines, wrapped.end());
}

// ===========================================================================
// model discovery
// ===========================================================================

struct model_entry {
    std::string        path;
    std::string        name;
    unsigned long long size = 0;
};

std::string home_dir() {
    const char * home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return home;
    }
#if defined(_WIN32)
    const char * up = getenv("USERPROFILE");
    if (up != nullptr && up[0] != '\0') {
        return up;
    }
#endif
    return std::string();
}

std::vector<std::string> split_dirs(const std::string & s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        const bool sep = (c == ':') ||
#if defined(_WIN32)
                         (c == ';') ||
#endif
                         false;
        if (sep) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

std::vector<std::string> model_dirs() {
    std::vector<std::string> dirs;

    const char * env = getenv("DISKINFER_MODEL_DIRS");
    if (env != nullptr && env[0] != '\0') {
        for (auto & d : split_dirs(env)) {
            dirs.push_back(d);
        }
    }
    const std::string home = home_dir();
    if (!home.empty()) {
        dirs.push_back(home + "/models");
    }
    dirs.push_back("/storage/emulated/0/models");
    dirs.push_back("/data/local/tmp/llm/models");
    dirs.push_back("/data/local/tmp/llm");

    std::vector<std::string> out;
    for (const auto & d : dirs) {
        bool dup = false;
        for (const auto & e : out) {
            if (e == d) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        std::error_code ec;
        if (fs::is_directory(d, ec)) {
            out.push_back(d);
        }
    }
    return out;
}

std::vector<model_entry> scan_models() {
    std::vector<model_entry> out;
    std::vector<std::string> seen_paths;    // canonical paths already listed
    std::vector<std::string> seen_files;    // "name|size" fallback for odd filesystems
    for (const auto & dir : model_dirs()) {
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            continue;
        }
        for (const auto & de : it) {
            std::error_code ec2;
            if (!de.is_regular_file(ec2) || ec2) {
                continue;
            }
            std::string ext = de.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            if (ext != ".gguf") {
                continue;
            }
            // the same file can be reachable through several model dirs (symlinks,
            // bind mounts); list it once
            std::error_code ec3;
            const fs::path canon = fs::canonical(de.path(), ec3);
            const std::string key = ec3 ? fs::absolute(de.path(), ec3).string() : canon.string();
            bool dup = false;
            for (const auto & s : seen_paths) {
                if (s == key) { dup = true; break; }
            }
            if (dup) {
                continue;
            }
            model_entry m;
            m.path = de.path().string();
            m.name = de.path().filename().string();
            m.size = (unsigned long long) de.file_size(ec2);
            const std::string file_key = m.name + "|" + std::to_string(m.size);
            for (const auto & s : seen_files) {
                if (s == file_key) { dup = true; break; }
            }
            if (dup) {
                continue;
            }
            seen_paths.push_back(key);
            seen_files.push_back(file_key);
            out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const model_entry & a, const model_entry & b) { return a.name < b.name; });
    return out;
}

// ===========================================================================
// command line / UI state
// ===========================================================================

struct cli_options {
    std::string model;
    std::string prompt;   // empty by default: the user types it on the run page
    std::string sysmsg;
    bool  prompt_set = false;
    bool  api_only   = false;
    bool  chat       = false;
    bool  moe        = false;
    bool  api        = false;
    bool  n_ctx_set  = false;
    int   n_ctx      = 16;
    int   n_predict  = 256;
    int   n_threads  = 4;
    int   n_ubatch   = 4;
    float temp       = 0.70f;
    int   top_k      = 40;
    float top_p      = 0.95f;
    int   port       = 4000;
    bool  help       = false;
    bool  mem_probe  = false;   // --mem-probe: print the RAM estimate and exit
};

struct gen_job {
    std::thread             thread;
    std::atomic<bool>       done{ false };
    std::mutex              mtx;
    diskui::gen_result      result;
    bool                    running = false;

    void join_and_collect(diskui::gen_result & out) {
        if (thread.joinable()) {
            thread.join();
        }
        std::lock_guard<std::mutex> lk(mtx);
        out = result;
        running = false;
        done.store(false);
    }
};

enum class page_t { model, settings, run };

struct ui_state {
    page_t page = page_t::model;

    std::vector<model_entry> models;
    int               model_sel = 0;
    bool              model_fixed = false;
    std::string       model_path;

    int   sel     = 0;
    int   n_ctx   = 16;
    float temp    = 0.70f;
    bool  chat    = false;
    bool  moe     = false;
    bool  api_on  = false;
    int   arena_mb = 0;      // disk arena size in MiB, 0 = auto (launcher / LLAMA_DISK_STREAM_MB)
    int   ctx_max = 4096;
    int   tmpl_extra = -1;
    bool  settings_ready = false;
    int   n_ctx_before_chat = 16;

    std::string prompt;
    std::string sysmsg;
    bool        editing = false;
    std::string edit_backup;

    gen_job            job;
    diskui::gen_result last;
    std::string        status;
    bool               status_error = false;
    bool               have_run = false;

    // cached RAM estimate for the settings/run pages (key = ctx|ubatch|arena|loaded)
    diskui::mem_estimate mem_est;
    std::string          mem_key;
};

ui_state       g_ui;
cli_options    g_cli;
diskui::api::server g_api;

// ---- frame helpers ---------------------------------------------------------

void frame_pad(std::vector<row> & f, const std::vector<row> & footer) {
    const int max_body = std::max(0, g_rows - (int) footer.size());
    if ((int) f.size() > max_body) {
        f.resize((size_t) max_body);
    }
    while ((int) f.size() < max_body) {
        f.push_back(row{ "", S_NORM });
    }
    for (const auto & r : footer) {
        f.push_back(r);
    }
}

row rule_row(const std::string & title, int indent = 1) {
    std::string s((size_t) indent, ' ');
    s += "── " + title + " ";
    int w = disp_width(s);
    while (w < g_cols - 1) {
        s += "─";
        ++w;
    }
    return row{ s, S_CYAN };
}

std::string bar_text(double frac, int width) {
    if (width < 4) {
        width = 4;
    }
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    const int full = (int) (frac * width + 0.5);
    std::string s;
    for (int i = 0; i < width; ++i) {
        s += (i < full) ? "█" : "░";
    }
    return s;
}

std::string on_off(bool b) {
    return i18n::tr(b ? "common.on" : "common.off");
}

// internal finish reason ("stop"/"length"/"cancelled"/"error") -> UI text
std::string finish_label(const std::string & f) {
    if (f == "length")    return i18n::tr("common.finish_length");
    if (f == "cancelled") return i18n::tr("common.finish_cancelled");
    if (f == "error")     return i18n::tr("common.finish_error");
    return i18n::tr("common.finish_stop");
}

// engine gen_result.error is kept English (it is also returned in the HTTP JSON
// message field); translate the known ones for the TUI status line
std::string error_label(const std::string & e) {
    if (e == "no model loaded")                    return i18n::tr("eng.no_model_loaded");
    if (e == "failed to tokenize the prompt")      return i18n::tr("eng.tokenize_failed");
    if (e == "context is too small for this prompt") return i18n::tr("eng.ctx_too_small");
    if (e == "failed to create a llama_context")   return i18n::tr("eng.ctx_create_failed");
    if (e == "generation threw")                   return i18n::tr("st.gen_threw");
    if (e.rfind("prefill decode failed at token ", 0) == 0) {
        return i18n::trf("eng.prefill_failed_fmt", atoi(e.c_str() + 28));
    }
    if (e.rfind("decode failed at token ", 0) == 0) {
        return i18n::trf("eng.decode_failed_fmt", atoi(e.c_str() + 23));
    }
    if (e.rfind("busy: ", 0) == 0) {
        return i18n::trf("st.busy_fmt", e.c_str() + 6);
    }
    return e;
}

std::string basename_of(const std::string & p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// ===========================================================================
// frames
// ===========================================================================

std::vector<row> frame_model_picker() {
    std::vector<row> f;
    auto push = [&](std::string s, style_t st) { f.push_back(row{ std::move(s), st }); };

    push("", S_NORM);
    push(" " + pad_to(i18n::tr("title.model"), g_cols - 2) + " ", S_BAR);
    push("", S_NORM);

    if (g_ui.models.empty()) {
        push(i18n::tr("pick.no_model"), S_RED);
        push("", S_NORM);
        push(i18n::tr("pick.where"), S_DIM);
        for (const auto & d : model_dirs()) {
            push("    " + clip(d, g_cols - 5), S_DIM);
        }
        if (model_dirs().empty()) {
            push(i18n::tr("pick.none_dirs"), S_DIM);
        }
        push("", S_NORM);
        push(i18n::tr("pick.example"), S_DIM);
        push(i18n::tr("pick.example_cmd"), S_DIM);
        push("", S_NORM);
        push(i18n::tr("pick.or_pass"), S_DIM);
    } else {
        push(i18n::trf("pick.choose_fmt", (int) g_ui.models.size()), S_CYAN_BOLD);
        push("", S_NORM);
        const int list_rows = std::max(3, g_rows - 8);
        int first = std::max(0, g_ui.model_sel - list_rows / 2);
        if (first + list_rows > (int) g_ui.models.size()) {
            first = std::max(0, (int) g_ui.models.size() - list_rows);
        }
        for (int i = first; i < (int) g_ui.models.size() && i < first + list_rows; ++i) {
            const model_entry & m = g_ui.models[(size_t) i];
            const bool sel = (i == g_ui.model_sel);
            std::string left = std::string(sel ? " ▸ " : "   ") + m.name;
            push(pad_between(clip(left, g_cols - 14), diskui::human_bytes(m.size) + " ",
                             g_cols - 1),
                 sel ? S_SEL : S_NORM);
        }
    }

    std::vector<row> footer;
    footer.push_back(row{ "", S_NORM });
    footer.push_back(row{ i18n::tr("pick.footer"), S_DIM });
    frame_pad(f, footer);
    return f;
}

std::vector<row> frame_settings() {
    diskui::engine & eng = diskui::engine::get();
    llama_disk::llama_disk_stats ds;
    llama_disk::get_stats(&ds);

    std::vector<row> f;
    auto push  = [&](std::string s, style_t st) { f.push_back(row{ std::move(s), st }); };
    auto blank = [&] { push("", S_NORM); };

    const std::string base_name = basename_of(g_ui.model_path);

    // header ---------------------------------------------------------------
    push(" " + pad_to(i18n::tr("title.settings"), g_cols - 2) + " ", S_BAR);
    push(i18n::trf("set.model_fmt", clip(base_name, g_cols - 11).c_str()), S_BOLD);
    push(i18n::trf("set.ctx_max_fmt", g_ui.ctx_max, eng.n_layer(),
                   diskui::human_bytes(ds.arena_bytes).c_str()), S_DIM);
    if (!g_short) {
        push(i18n::trf("set.stream_fmt", diskui::human_bytes(ds.registered_bytes).c_str(),
                       ds.tensors, ds.direct_io ? i18n::tr("common.on") : i18n::tr("common.off")),
             S_DIM);
    }
    blank();

    f.push_back(rule_row(i18n::tr("set.section"), 1));
    blank();

    const int label_w = g_narrow ? 17 : 20;
    const int value_w = g_narrow ? 6 : 9;

    char v_temp[32];
    snprintf(v_temp, sizeof(v_temp), "%.2f", g_ui.temp);

    const std::string arena_value = g_ui.arena_mb > 0 ? std::to_string(g_ui.arena_mb)
                                                      : i18n::tr("common.auto");
    const std::string values[7] = {
        std::to_string(g_ui.n_ctx),
        v_temp,
        on_off(g_ui.chat),
        on_off(g_ui.moe),
        arena_value,
        std::to_string(g_cli.n_ubatch),
        on_off(g_ui.api_on),
    };
    const std::string labels[7] = {
        i18n::tr("set.context"),
        i18n::tr("set.temperature"),
        i18n::tr("set.chat_template"),
        i18n::tr("set.moe_path"),
        i18n::tr("set.arena"),
        i18n::tr("set.prefill"),
        i18n::tr("set.api_server"),
    };
    const std::string hints[7] = {
        i18n::trf("set.ctx_hint_fmt", g_ui.ctx_max),
        i18n::tr("set.temp_hint"),
        (g_ui.tmpl_extra >= 0)
            ? i18n::trf("set.chat_hint_fmt", g_ui.tmpl_extra)
            : i18n::tr("set.chat_hint_unknown"),
        i18n::tr("set.moe_hint"),
        i18n::tr("set.arena_hint"),
        i18n::tr("set.prefill_hint"),
        i18n::trf("set.api_hint_fmt", g_cli.port),
    };

    for (int i = 0; i < 7; ++i) {
        const bool sel = (g_ui.sel == i);
        std::string s = "  ";
        s += sel ? "▸ " : "  ";
        s += pad_to(labels[i], label_w);
        s += pad_to(values[i], value_w);
        if (!g_narrow) {
            s += "   ";
            s += hints[i];
        }
        push(s, sel ? S_SEL : S_NORM);
    }

    blank();
    if (g_narrow) {
        push("  " + clip(hints[g_ui.sel], g_cols - 4), S_DIM);
        blank();
    }

    // status ---------------------------------------------------------------
    if (!g_short) {
        f.push_back(rule_row(i18n::tr("set.status_section"), 1));
    } else {
        push(std::string("  ── ") + i18n::tr("set.status_section") + " ──", S_CYAN);
    }
    {
        const bool api_busy = diskui::engine::get().busy_owner() == diskui::owner::api;
        const bool tui_busy = diskui::engine::get().busy_owner() == diskui::owner::tui;
        std::string st = g_ui.status;
        if (api_busy) {
            st = i18n::trf("set.status_busy_fmt", diskui::engine::get().busy_label().c_str());
        } else if (tui_busy) {
            st = i18n::tr("set.tui_running");
        }
        push("  " + clip(st, g_cols - 4),
             api_busy ? S_YELLOW : (g_ui.status_error ? S_RED : S_DIM));
    }
    if (!g_short) {
        push("  " + clip(g_ui.sysmsg.empty()
                             ? i18n::tr("set.system_prompt_none")
                             : i18n::trf("set.system_prompt_fmt", g_ui.sysmsg.c_str()),
                         g_cols - 4),
             S_DIM);
    }

    // RAM estimate of the next run with the current settings -----------------
    {
        diskui::engine & engr = diskui::engine::get();
        const std::string mem_key = std::to_string(g_ui.n_ctx) + "|" +
                                    std::to_string(g_cli.n_ubatch) + "|" +
                                    std::to_string(g_ui.arena_mb) + "|" +
                                    (engr.loaded() ? "1" : "0");
        if (mem_key != g_ui.mem_key) {
            g_ui.mem_key = mem_key;
            if (engr.loaded() && !engr.busy()) {
                g_ui.mem_est = engr.probe_memory(g_ui.n_ctx, g_cli.n_ubatch);
            } else {
                g_ui.mem_est = diskui::mem_estimate();
            }
        }
        if (g_ui.mem_est.ok) {
            push("  " + i18n::trf("set.ram_resident_fmt",
                                  diskui::human_bytes(g_ui.mem_est.resident).c_str(),
                                  diskui::human_bytes(g_ui.mem_est.kv).c_str(),
                                  diskui::human_bytes(g_ui.mem_est.recurrent).c_str(),
                                  diskui::human_bytes(g_ui.mem_est.arena).c_str()), S_DIM);
            push("  " + i18n::trf("set.ram_peak_fmt",
                                  diskui::human_bytes(g_ui.mem_est.peak).c_str(),
                                  diskui::human_bytes(g_ui.mem_est.compute).c_str(),
                                  diskui::human_bytes(g_ui.mem_est.scratch).c_str()), S_DIM);
        } else {
            push(std::string("  ") + i18n::tr("set.ram_note"), S_DIM);
        }
    }

    // footer ---------------------------------------------------------------
    std::vector<row> footer;
    footer.push_back(row{ "", S_NORM });
    footer.push_back(row{ g_narrow ? i18n::tr("set.footer_narrow") : i18n::tr("set.footer_wide"),
                            S_DIM });
    if (g_ui.api_on) {
        footer.push_back(row{ i18n::trf("set.api_footer_fmt", g_cli.port), S_GREEN });
    }

    // the log of the last start up fills the rest of the page, so the settings
    // page does not look empty on a tall phone terminal
    {
        const int rem = g_rows - (int) f.size() - (int) footer.size();
        if (rem >= 4) {
            f.push_back(rule_row(i18n::tr("set.log_section"), 1));
            std::vector<std::string> logs = log_tail(rem - 1, g_cols - 4);
            while ((int) logs.size() < rem - 1) {
                logs.insert(logs.begin(), "");
            }
            for (const auto & l : logs) {
                push("  " + clip(l, g_cols - 4), S_DIM);
            }
        }
    }

    frame_pad(f, footer);
    return f;
}

std::vector<row> frame_run() {
    diskui::engine & eng = diskui::engine::get();
    const diskui::live_state ls  = eng.snapshot();
    const diskui::owner busy_owner = eng.busy_owner();
    const bool api_busy   = (busy_owner == diskui::owner::api);
    const bool tui_running = g_ui.job.running;
    const bool active      = ls.active;
    const int  phase       = ls.phase;

    llama_disk::llama_disk_stats ds;
    llama_disk::get_stats(&ds);

    std::vector<row> f;
    auto push = [&](std::string s, style_t st) { f.push_back(row{ std::move(s), st }); };

    const std::string base_name = basename_of(g_ui.model_path);

    // header ---------------------------------------------------------------
    push(" " + pad_to(i18n::tr("title.run"), g_cols - 2) + " ", S_BAR);
    push("  " + clip(base_name, g_cols - 3), S_BOLD);
    push(clip(i18n::trf("run.meta_fmt", g_ui.n_ctx, g_ui.temp,
                        g_ui.chat ? i18n::tr("common.template") : i18n::tr("common.raw"),
                        on_off(g_ui.moe).c_str(), on_off(g_ui.api_on).c_str()),
              g_cols - 1), S_DIM);

    // prompt ---------------------------------------------------------------
    f.push_back(rule_row(i18n::tr("run.prompt_section"), 1));
    {
        std::string p = "  > " + g_ui.prompt;
        if (g_ui.editing) {
            p += "▌";
        } else if (!tui_running) {
            p += i18n::tr("run.prompt_hint");
        }
        push(clip(p, g_cols - 1), g_ui.editing ? S_YELLOW : S_BOLD);
    }
    if (!g_ui.editing && !tui_running && g_ui.prompt.empty()) {
        push(clip(std::string("  ") + i18n::tr("run.prompt_empty_hint"), g_cols - 1), S_DIM);
    }

    // progress: a fixed height block, so the page never jumps while decoding
    f.push_back(rule_row(i18n::tr("run.progress_section"), 1));
    {
        const int bar_w = g_narrow ? 12 : 22;
        std::vector<row> prog;

        // 1) prefill
        if (active && phase == 2) {
            prog.push_back(row{ clip(i18n::trf("run.prefill_live_fmt",
                                       bar_text((double) ls.done / (double) std::max(1, ls.total), bar_w).c_str(),
                                       ls.done, ls.total, diskui::fmt_seconds(ls.io_s).c_str(),
                                       diskui::fmt_seconds(ls.cmp_s).c_str()),
                                   g_cols - 1), S_NORM });
        } else if (g_ui.have_run) {
            prog.push_back(row{ clip(i18n::trf("run.prefill_done_fmt", g_ui.last.n_prompt,
                                       diskui::fmt_seconds(g_ui.last.pp_s).c_str()),
                                   g_cols - 1), S_DIM });
        } else if (active) {
            prog.push_back(row{ clip(i18n::trf("run.prefill_active_fmt", ls.n_prompt),
                                   g_cols - 1), S_DIM });
        } else {
            prog.push_back(row{ i18n::tr("run.prefill_none"), S_DIM });
        }

        // 2) generate
        if (active && phase == 3) {
            const double tps = ls.gen_s > 0.001 ? ls.done / ls.gen_s : 0.0;
            prog.push_back(row{ clip(i18n::trf("run.gen_live_fmt",
                                       bar_text((double) ls.done / (double) std::max(1, ls.total), bar_w).c_str(),
                                       ls.done, ls.total, tps, ls.layer + 1,
                                       ls.n_layer > 0 ? ls.n_layer : eng.n_layer()),
                                   g_cols - 1), S_NORM });
        } else if (g_ui.have_run) {
            prog.push_back(row{ clip(i18n::trf("run.gen_done_fmt", g_ui.last.n_gen,
                                       g_ui.last.gen_s > 0.001 ? g_ui.last.n_gen / g_ui.last.gen_s : 0.0,
                                       finish_label(g_ui.last.finish).c_str()),
                                   g_cols - 1), S_DIM });
        } else if (active) {
            const double tps = ls.gen_s > 0.001 ? ls.done / ls.gen_s : 0.0;
            prog.push_back(row{ clip(i18n::trf("run.gen_active_fmt", ls.n_gen, tps),
                                   g_cols - 1), S_DIM });
        } else {
            prog.push_back(row{ i18n::tr("run.gen_none"), S_DIM });
        }

        // 3) disk counters
        {
            const unsigned long long bytes = active ? ls.bytes : ds.bytes;
            const unsigned long long loads = active ? ls.loads : ds.loads;
            const unsigned long long evict = active ? ls.evictions : ds.evictions;
            const double io  = active ? ls.io_s  : 0.0;
            const double cmp = active ? ls.cmp_s : 0.0;
            if (active) {
                prog.push_back(row{ clip(i18n::trf("run.disk_live_fmt",
                                           diskui::human_bytes(bytes).c_str(), loads, evict,
                                           diskui::fmt_seconds(io).c_str(),
                                           diskui::fmt_seconds(cmp).c_str()),
                                       g_cols - 1), S_DIM });
            } else {
                prog.push_back(row{ clip(i18n::trf("run.disk_idle_fmt",
                                           diskui::human_bytes(ds.registered_bytes).c_str(),
                                           ds.tensors),
                                       g_cols - 1), S_DIM });
            }
        }

        // 4) state line
        if (api_busy) {
            prog.push_back(row{ clip(i18n::trf("run.api_busy_fmt", eng.busy_label().c_str()),
                                     g_cols - 1), S_YELLOW });
        } else if (tui_running || (active && ls.who == diskui::owner::tui)) {
            prog.push_back(row{ i18n::tr("run.tui_running"), S_GREEN });
        } else if (g_ui.have_run) {
            prog.push_back(row{ clip(i18n::trf("run.idle_last_fmt", g_ui.last.n_gen,
                                       finish_label(g_ui.last.finish).c_str()),
                                   g_cols - 1), S_DIM });
        } else {
            prog.push_back(row{ i18n::tr("run.idle_hint"), S_DIM });
        }

        const int want = g_short ? 3 : 4;
        for (int i = 0; i < want && i < (int) prog.size(); ++i) {
            f.push_back(prog[(size_t) i]);
        }
    }

    // footer ---------------------------------------------------------------
    std::vector<row> footer;
    footer.push_back(row{ "", S_NORM });
    {
        const std::string left = g_narrow ? i18n::tr("run.footer_narrow")
                                          : i18n::tr("run.footer_wide");
        std::string right;
        if (api_busy) {
            right = i18n::tr("run.api_badge_busy");
        } else if (g_ui.api_on) {
            right = i18n::trf("run.api_badge_fmt", g_cli.port);
        }
        footer.push_back(row{ pad_between(left, right, g_cols - 1), S_DIM });
    }

    // answer (or a model summary before the first run) + log fill the rest ----
    const bool show_answer = ls.active || g_ui.have_run;
    const std::string answer = ls.active ? ls.answer
                                         : (g_ui.have_run ? g_ui.last.text : std::string());

    int rem = std::max(0, g_rows - (int) f.size() - (int) footer.size());
    int log_content = g_short ? 2 : (g_narrow ? 6 : 3);
    int a_lines = rem - 2 - log_content;
    while (a_lines < 3 && log_content > 0) {
        --log_content;
        ++a_lines;
    }
    if (a_lines < 1) {
        a_lines = std::max(0, rem - 1);
        log_content = 0;
    }

    f.push_back(rule_row(show_answer ? i18n::tr("run.answer_section")
                                     : i18n::tr("run.model_section"), 1));
    --a_lines;

    if (show_answer) {
        std::vector<std::string> lines;
        if (!answer.empty()) {
            lines = wrap_text(answer, g_cols - 4);
        }
        // keep the tail when the answer scrolls, otherwise start at the top of
        // the section so the first streamed tokens appear right under "Answer"
        if ((int) lines.size() > a_lines) {
            lines = std::vector<std::string>(lines.end() - a_lines, lines.end());
        }
        while ((int) lines.size() < a_lines) {
            lines.push_back("");
        }
        for (int i = 0; i < a_lines; ++i) {
            std::string s = "  " + lines[(size_t) i];
            if (i == a_lines - 1 && ls.active && ls.phase == 3) {
                s += "▌";
            }
            push(clip(s, g_cols - 1), S_NORM);
        }
    } else {
        // before the first run the space shows the loaded model instead of an
        // empty box: this is the information a user opens the app for
        const std::string tmpl_note = g_ui.tmpl_extra >= 0
            ? ("+" + std::to_string(g_ui.tmpl_extra) + " tok")
            : i18n::tr("common.none");

        std::vector<std::string> info;
        info.push_back(i18n::trf("run.info_model_fmt", clip(base_name, g_cols - 12).c_str()));
        info.push_back(i18n::trf("run.info_path_fmt", clip(g_ui.model_path, g_cols - 12).c_str()));
        info.push_back(i18n::trf("run.info_size_fmt",
                                 diskui::human_bytes(ds.registered_bytes).c_str(), ds.tensors));
        if (ds.tiny) {
            info.push_back(i18n::trf("run.info_arena_tiny_fmt",
                                     diskui::human_bytes(ds.arena_bytes).c_str(),
                                     std::to_string(ds.chunk_rows).c_str(),
                                     ds.direct_io ? i18n::tr("common.on") : i18n::tr("common.off")));
        } else {
            info.push_back(i18n::trf("run.info_arena_fmt",
                                     diskui::human_bytes(ds.arena_bytes).c_str(),
                                     ds.direct_io ? i18n::tr("common.on") : i18n::tr("common.off"),
                                     ds.mlock ? i18n::tr("common.on") : i18n::tr("common.off")));
        }
        {
            diskui::engine & engr = diskui::engine::get();
            if (!engr.busy()) {
                g_ui.mem_est = engr.probe_memory(g_ui.n_ctx, g_cli.n_ubatch);
                g_ui.mem_key.clear();
            }
            if (g_ui.mem_est.ok) {
                info.push_back(i18n::trf("run.info_ram_fmt",
                                         diskui::human_bytes(g_ui.mem_est.resident).c_str(),
                                         diskui::human_bytes(g_ui.mem_est.peak).c_str(),
                                         diskui::human_bytes(g_ui.mem_est.kv + g_ui.mem_est.recurrent).c_str()));
            }
        }
        info.push_back(i18n::trf("run.info_context_fmt", g_ui.n_ctx, g_ui.ctx_max,
                                 tmpl_note.c_str()));
        info.push_back(i18n::trf("run.info_runtime_fmt", g_cli.n_threads, g_cli.n_ubatch,
                                 g_cli.n_predict));
        if (g_ui.api_on) {
            info.push_back(i18n::trf("run.info_api_fmt", g_cli.port));
        }
        info.push_back("");
        info.push_back(i18n::tr("run.info_hint"));
        for (int i = 0; i < a_lines; ++i) {
            if (i < (int) info.size()) {
                push(clip("  " + info[(size_t) i], g_cols - 1), i + 1 == (int) info.size() ? S_BOLD : S_DIM);
            } else {
                push("", S_NORM);
            }
        }
    }

    if (log_content > 0) {
        f.push_back(rule_row(i18n::tr("run.log_section"), 1));
        --log_content;
        log_content = std::max(0, log_content);
        std::vector<std::string> logs = log_tail(log_content, g_cols - 4);
        while ((int) logs.size() < log_content) {
            logs.insert(logs.begin(), "");
        }
        for (const auto & l : logs) {
            push("  " + clip(l, g_cols - 4), S_DIM);
        }
    }

    frame_pad(f, footer);
    return f;
}

// ===========================================================================
// generation control
// ===========================================================================

diskui::gen_params make_params() {
    diskui::gen_params gp;
    gp.n_ctx     = g_ui.n_ctx;
    gp.n_predict = g_cli.n_predict;
    gp.temp      = g_ui.temp;
    gp.top_k     = g_cli.top_k;
    gp.top_p     = g_cli.top_p;
    gp.n_threads = g_cli.n_threads;
    gp.n_ubatch  = g_cli.n_ubatch;
    return gp;
}

int current_pid() {
#if defined(_WIN32)
    return (int) GetCurrentProcessId();
#else
    return (int) getpid();
#endif
}

std::vector<diskui::chat_message> make_messages(const std::string & prompt) {
    std::vector<diskui::chat_message> msgs;
    if (!g_ui.sysmsg.empty()) {
        msgs.push_back(diskui::chat_message{ "system", g_ui.sysmsg });
    }
    msgs.push_back(diskui::chat_message{ "user", prompt });
    return msgs;
}

bool load_selected_model();   // defined below, used by the settings actions

void start_inference() {
    diskui::engine & eng = diskui::engine::get();
    if (g_ui.job.running) {
        return;
    }
    if (g_ui.prompt.empty()) {
        g_ui.editing     = true;
        g_ui.edit_backup = g_ui.prompt;
        g_ui.status      = i18n::tr("st.type_prompt");
        g_ui.status_error = false;
        invalidate();
        return;
    }
    if (!eng.loaded()) {
        g_ui.status = i18n::tr("st.no_model");
        g_ui.status_error = false;
        return;
    }

    // the MoE switch and the disk arena size are read when the model is
    // registered, so changing either needs a reload; the settings page says so
    const bool moe_changed   = eng.loaded_with_moe() != g_ui.moe;
    const bool arena_changed = eng.requested_arena_mb() != g_ui.arena_mb;
    if (moe_changed || arena_changed) {
        if (moe_changed) {
            g_ui.status = i18n::tr(g_ui.moe ? "st.reloading_moe_on" : "st.reloading_moe_off");
        } else {
            g_ui.status = i18n::tr("st.reloading_fmt");
        }
        g_ui.status_error = false;
        invalidate();
        paint_frame(frame_run());
        std::string err;
        if (!eng.load(g_ui.model_path, g_ui.moe, err, g_ui.arena_mb)) {
            g_ui.status = i18n::trf("st.reload_failed_fmt", error_label(err).c_str());
            g_ui.status_error = true;
            route_log("! " + i18n::trf("st.reload_failed_fmt", error_label(err).c_str()));
            invalidate();
            return;
        }
        invalidate();
    }

    const diskui::gen_params gp = make_params();
    const std::string     prompt = g_ui.prompt;
    const bool            chat   = g_ui.chat;
    std::vector<diskui::chat_message> msgs;
    if (chat) {
        msgs = make_messages(prompt);
    }
    g_cancel_in_progress.store(false);
    g_ui.job.running = true;
    g_ui.job.done.store(false);
    g_ui.last = diskui::gen_result();
    g_ui.status = i18n::tr("st.running");
    g_ui.status_error = false;
    invalidate();

    gen_job * job = &g_ui.job;
    job->thread = std::thread([job, gp, msgs, prompt, chat, &eng] {
        diskui::gen_result r;
        // the slot is taken inside the worker: a std::mutex has to be unlocked by
        // the thread that locked it
        if (!eng.acquire(diskui::owner::tui, "TUI")) {
            r.ok     = false;
            r.finish = "error";
            r.error  = "busy: " + eng.busy_label();
            {
                std::lock_guard<std::mutex> lk(job->mtx);
                job->result = std::move(r);
            }
            job->done.store(true);
            return;
        }
        try {
            if (chat) {
                r = eng.generate_chat(msgs, gp);
            } else {
                r = eng.generate_raw(prompt, gp);
            }
        } catch (const std::exception & e) {
            r.ok = false;
            r.finish = "error";
            r.error = e.what();
        } catch (...) {
            r.ok = false;
            r.finish = "error";
            r.error = "generation threw";
        }
        {
            std::lock_guard<std::mutex> lk(job->mtx);
            job->result = std::move(r);
        }
        eng.release();
        job->done.store(true);
    });
}

void stop_inference() {
    if (!g_ui.job.running) {
        return;
    }
    g_cancel_in_progress.store(true);
    diskui::engine::get().request_cancel();
    g_ui.status = i18n::tr("st.cancelling");
    g_ui.status_error = false;
}

void poll_generation() {
    if (!g_ui.job.running || !g_ui.job.done.load()) {
        return;
    }
    diskui::gen_result r;
    g_ui.job.join_and_collect(r);
    g_ui.last = std::move(r);
    g_ui.have_run = true;
    g_cancel_in_progress.store(false);
    if (g_ui.last.finish == "cancelled") {
        g_ui.status = i18n::tr("st.cancelled");
        g_ui.status_error = false;
    } else if (g_ui.last.ok) {
        g_ui.status = i18n::tr("st.done");
        g_ui.status_error = false;
    } else {
        g_ui.status = i18n::trf("st.error_fmt", error_label(g_ui.last.error).c_str());
        g_ui.status_error = true;
    }
    invalidate();
}

// ---- settings actions ------------------------------------------------------

int ctx_for_template() {
    const int extra = g_ui.tmpl_extra >= 0 ? g_ui.tmpl_extra : 16;   // 16 = documented fallback
    return std::max(3, std::min(g_ui.ctx_max, 16 + extra));
}

void apply_chat_toggle() {
    if (g_ui.chat) {
        g_ui.n_ctx_before_chat = g_ui.n_ctx;
        g_ui.n_ctx = ctx_for_template();
    } else {
        g_ui.n_ctx = std::max(3, std::min(g_ui.ctx_max, g_ui.n_ctx_before_chat));
    }
}

bool start_api_server() {
    diskui::api::options opt;
    opt.port      = g_cli.port;
    opt.n_ctx     = std::max(256, g_ui.n_ctx);
    opt.n_predict = g_cli.n_predict;
    opt.n_threads = g_cli.n_threads;
    opt.n_ubatch  = g_cli.n_ubatch;
    opt.temp      = g_ui.temp;
    opt.top_k     = g_cli.top_k;
    opt.top_p     = g_cli.top_p;
    std::string err;
    if (!g_api.start(opt, err)) {
        g_ui.status = i18n::trf("st.api_start_failed_fmt", err.c_str());
        g_ui.status_error = true;
        route_log("! " + err);
        return false;
    }
    return true;
}

void set_api_enabled(bool on) {
    if (on == g_ui.api_on) {
        return;
    }
    if (on) {
        g_ui.api_on = start_api_server();
        if (g_ui.api_on) {
            g_ui.status = i18n::trf("st.api_listening_fmt", g_cli.port);
            g_ui.status_error = false;
        }
    } else {
        g_api.stop();
        g_ui.api_on = false;
        g_ui.status = i18n::tr("st.api_stopped");
        g_ui.status_error = false;
    }
}

// ===========================================================================
// pages: model picker / settings / run
// ===========================================================================

bool load_selected_model() {
    diskui::engine & eng = diskui::engine::get();

    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        g_logbuf.clear();
        g_logbuf.push_back(i18n::trf("log.model_switch_fmt", basename_of(g_ui.model_path).c_str()));
    }

    invalidate();
    {
        std::vector<row> f;
        f.push_back(row{ "", S_NORM });
        f.push_back(row{ " " + pad_to(i18n::tr("title.loading"), g_cols - 2) + " ", S_BAR });
        f.push_back(row{ "", S_NORM });
        f.push_back(row{ "  " + clip(g_ui.model_path, g_cols - 4), S_BOLD });
        f.push_back(row{ "", S_NORM });
        f.push_back(row{ i18n::tr("load.registering"), S_DIM });
        frame_pad(f, std::vector<row>{ row{ "", S_NORM },
                                       row{ i18n::tr("load.wait"), S_DIM } });
        paint_frame(f);
    }

    std::string err;
    if (!eng.load(g_ui.model_path, g_ui.moe, err, g_ui.arena_mb)) {
        route_log("! " + i18n::trf("st.load_failed_fmt", err.c_str()));
        std::vector<row> f;
        f.push_back(row{ "", S_NORM });
        f.push_back(row{ " " + pad_to(i18n::tr("title.load_failed"), g_cols - 2) + " ", S_BAR });
        f.push_back(row{ "", S_NORM });
        f.push_back(row{ "  " + clip(err, g_cols - 4), S_RED });
        f.push_back(row{ "", S_NORM });
        for (const auto & l : log_tail(6, g_cols - 4)) {
            f.push_back(row{ "  " + clip(l, g_cols - 4), S_DIM });
        }
        frame_pad(f, std::vector<row>{ row{ "", S_NORM },
                                       row{ i18n::tr("load.press_back"), S_DIM } });
        paint_frame(f);
        read_key(30000);
        invalidate();
        // with a pinned model there is nothing to fall back to
        if (g_ui.model_fixed) {
            g_signal = 1;
        }
        return false;
    }

    g_ui.ctx_max    = std::max(3, eng.n_ctx_train() > 0 ? eng.n_ctx_train() : 4096);
    g_ui.tmpl_extra = eng.chat_template_extra_tokens();
    const bool has_tmpl = eng.has_chat_template();

    std::string note;

    if (!g_ui.settings_ready) {
        // first load of this session: take the defaults / command line overrides
        g_ui.settings_ready = true;
        g_ui.n_ctx = g_cli.n_ctx;
        g_ui.chat  = g_cli.chat && has_tmpl;
        g_ui.n_ctx_before_chat = g_ui.n_ctx;
    } else if (g_ui.chat && !has_tmpl) {
        // the new model cannot do templated turns, switch it off and say so
        g_ui.chat = false;
        note = i18n::tr("st.no_template_new_model");
    }

    // keep the settings of the session across a model switch, only clamp what
    // the new model cannot hold
    {
        const int want = g_ui.n_ctx;
        g_ui.n_ctx = std::max(3, std::min(g_ui.ctx_max, want));
        if (g_ui.n_ctx != want) {
            note = i18n::trf("st.ctx_clamped_fmt", g_ui.n_ctx, g_ui.ctx_max);
        }
        g_ui.n_ctx_before_chat = std::max(3, std::min(g_ui.ctx_max, g_ui.n_ctx_before_chat));
        if (g_ui.chat) {
            // never shrink below what the template itself needs
            g_ui.n_ctx = std::max(g_ui.n_ctx, ctx_for_template());
        }
    }

    // the API keeps serving the newly loaded model when it was on
    if (g_ui.api_on) {
        g_api.stop();
        if (!start_api_server()) {
            g_ui.api_on = false;
            note = note.empty() ? i18n::tr("st.api_restart_failed") : note;
        }
    }

    g_ui.status = note.empty() ? i18n::tr("st.ready") : note;
    g_ui.status_error = false;
    g_ui.page   = page_t::settings;
    invalidate();
    return true;
}


bool start_from_settings() {
    if (!diskui::engine::get().loaded()) {
        if (!load_selected_model()) {
            return true;
        }
    }
    g_ui.page = page_t::run;
    if (g_ui.prompt.empty()) {
        // no canned prompt: put the cursor in the prompt box and let the user type
        g_ui.editing     = true;
        g_ui.edit_backup = g_ui.prompt;
    }
    invalidate();
    return true;
}

// returns false when the application should exit
bool handle_model_picker(int key) {
    if (key == 'q' || key == K_ESC) {
        return false;
    }
    if (key == 'r') {
        g_ui.models = scan_models();
        g_ui.model_sel = 0;
        invalidate();
        return true;
    }
    if (g_ui.models.empty()) {
        return true;
    }
    if (key == K_UP) {
        g_ui.model_sel = (g_ui.model_sel + (int) g_ui.models.size() - 1) % (int) g_ui.models.size();
        invalidate();
    } else if (key == K_DOWN) {
        g_ui.model_sel = (g_ui.model_sel + 1) % (int) g_ui.models.size();
        invalidate();
    } else if (key == K_ENTER || key == 's') {
        g_ui.model_path = g_ui.models[(size_t) g_ui.model_sel].path;
        load_selected_model();
    }
    return true;
}

bool handle_settings(int key) {
    if (key == 'q' || key == K_ESC) {
        return false;
    }
    if (key == K_UP) {
        g_ui.sel = (g_ui.sel + 6) % 7;
        invalidate();
        return true;
    }
    if (key == K_DOWN) {
        g_ui.sel = (g_ui.sel + 1) % 7;
        invalidate();
        return true;
    }

    const bool left  = (key == K_LEFT);
    const bool right = (key == K_RIGHT);
    const bool enter = (key == K_ENTER || key == ' ');

    if (g_ui.sel == 0 && (left || right)) {
        int step = 1;
        if (g_ui.n_ctx > 64) {
            step = 64;
        } else if (g_ui.n_ctx > 16) {
            step = 4;
        }
        g_ui.n_ctx += right ? step : -step;
        g_ui.n_ctx = std::max(3, std::min(g_ui.ctx_max, g_ui.n_ctx));
        return true;
    }
    if (g_ui.sel == 0 && key >= '0' && key <= '9') {
        // typing a number replaces the value; digits typed in quick succession
        // are appended to it
        static int     typed = -1;
        static int64_t typed_at = 0;
        const int64_t now = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (typed < 0 || now - typed_at > 1500) {
            typed = 0;
        }
        typed = typed * 10 + (key - '0');
        typed_at = now;
        if (typed > g_ui.ctx_max) {
            typed = g_ui.ctx_max;
        }
        g_ui.n_ctx = std::max(3, typed);
        return true;
    }
    if (g_ui.sel == 1 && (left || right)) {
        g_ui.temp += right ? 0.05f : -0.05f;
        g_ui.temp = std::max(0.0f, std::min(1.0f, g_ui.temp));
        g_ui.temp = (float) ((int) (g_ui.temp * 100.0f + 0.5f)) / 100.0f;
        return true;
    }
    if (g_ui.sel == 2 && enter) {
        g_ui.chat = !g_ui.chat;
        apply_chat_toggle();
        return true;
    }
    if (g_ui.sel == 3 && enter) {
        g_ui.moe = !g_ui.moe;
        return true;
    }
    if (g_ui.sel == 4) {
        // disk arena size in MiB; 0 = auto (keep the launcher / LLAMA_DISK_STREAM_MB value)
        if (left || right) {
            // 8..2048 MB; below 128 MB the steps halve (128, 64, 32, 16, 8, auto) so the
            // tiny-arena mode (big tensors read in chunks) is reachable from the keyboard
            if (g_ui.arena_mb <= 0) {
                g_ui.arena_mb = right ? 512 : 2048;
            } else if (right) {
                if (g_ui.arena_mb < 128) {
                    g_ui.arena_mb = std::min(128, g_ui.arena_mb * 2);
                } else {
                    g_ui.arena_mb = std::min(2048, g_ui.arena_mb + 128);
                }
            } else if (g_ui.arena_mb > 128) {
                g_ui.arena_mb -= 128;
            } else if (g_ui.arena_mb <= 8) {
                g_ui.arena_mb = 0;   // stepping left from 8 MB goes back to auto
            } else {
                g_ui.arena_mb /= 2;
            }
            return true;
        }
        if (key >= '0' && key <= '9') {
            static int     a_typed = -1;
            static int64_t a_at    = 0;
            const int64_t now = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if (a_typed < 0 || now - a_at > 1500) {
                a_typed = 0;
            }
            if (a_typed < 10000) {
                a_typed = a_typed * 10 + (key - '0');
            }
            a_at = now;
            g_ui.arena_mb = (a_typed == 0) ? 0 : std::max(8, std::min(2048, a_typed));
            return true;
        }
    }
    if (g_ui.sel == 5) {
        // tokens per prefill batch: 1..256. bigger batches mean fewer graph launches and a
        // faster prefill, but they also raise the peak RAM (context + compute buffer)
        if (left || right) {
            int step = 1;
            if (g_cli.n_ubatch >= 64) {
                step = 16;
            } else if (g_cli.n_ubatch >= 8) {
                step = 4;
            }
            g_cli.n_ubatch = std::max(1, std::min(256, g_cli.n_ubatch + (right ? step : -step)));
            return true;
        }
        if (key >= '0' && key <= '9') {
            static int     b_typed = -1;
            static int64_t b_at    = 0;
            const int64_t now = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if (b_typed < 0 || now - b_at > 1500) {
                b_typed = 0;
            }
            if (b_typed < 10000) {
                b_typed = b_typed * 10 + (key - '0');
            }
            b_at = now;
            g_cli.n_ubatch = (b_typed == 0) ? 1 : std::max(1, std::min(256, b_typed));
            return true;
        }
    }
    if (g_ui.sel == 6 && enter) {
        set_api_enabled(!g_ui.api_on);
        return true;
    }
    if (key == 's') {
        return start_from_settings();
    }
    if (key == 'c') {
        if (!g_ui.model_fixed && g_ui.models.size() > 1) {
            // stop serving the old model, but remember that the API was on: the
            // next model loads and the server comes back up on it
            g_api.stop();
            diskui::engine::get().unload();
            g_ui.page = page_t::model;
            invalidate();
        }
        return true;
    }
    return true;
}

bool handle_run(int key) {
    diskui::engine & eng = diskui::engine::get();

    if (g_ui.editing) {
        if (key == K_ENTER) {
            g_ui.editing = false;
            invalidate();
            return true;
        }
        if (key == K_ESC) {
            g_ui.prompt = g_ui.edit_backup;
            g_ui.editing = false;
            invalidate();
            return true;
        }
        if (key == K_BACKSPACE || key == K_DEL) {
            if (!g_ui.prompt.empty()) {
                size_t i = g_ui.prompt.size() - 1;
                while (i > 0 && ((unsigned char) g_ui.prompt[i] & 0xC0) == 0x80) {
                    --i;
                }
                g_ui.prompt.erase(i);
            }
            return true;
        }
        if (key >= 32 && key < 127) {
            if (g_ui.prompt.size() < 200) {
                g_ui.prompt += (char) key;
            }
            return true;
        }
        return true;
    }

    if (key == 'q' || key == K_ESC) {
        return false;
    }
    if (key == 'i') {
        if (!g_ui.job.running) {
            g_ui.editing = true;
            g_ui.edit_backup = g_ui.prompt;
            invalidate();
        }
        return true;
    }
    if (key == 's' && !g_ui.job.running) {
        start_inference();
        return true;
    }
    if (key == 'c') {
        if (g_ui.job.running) {
            stop_inference();
        } else {
            eng.clear_cancel();
            g_ui.page = page_t::settings;
            invalidate();
        }
        return true;
    }
    if (key == K_ENTER && !g_ui.job.running) {
        start_inference();
        return true;
    }
    return true;
}

// ===========================================================================
// TUI main loop
// ===========================================================================

int run_tui() {
    diskui::engine & eng = diskui::engine::get();

    // the terminal is only ours after term_enter(); everything below assumes it
    install_signal_handlers();
    eng.set_log_sink(route_log);
    diskui::engine::get().set_llama_logger(llama_log_cb, nullptr);
    llama_disk::set_msg_cb(disk_msg_cb);

    term_enter();
    term_size();
    g_compact = (g_rows < 20 || g_cols < 60);
    g_narrow  = (g_cols < 60);
    g_short   = (g_rows < 20);

    g_ui.prompt = g_cli.prompt;
    g_ui.sysmsg = g_cli.sysmsg;
    g_ui.temp   = g_cli.temp;
    g_ui.moe    = g_cli.moe;
    g_ui.chat   = g_cli.chat;
    g_ui.n_ctx  = g_cli.n_ctx;

    int rc = 0;

    if (!g_cli.model.empty()) {
        // the launcher pinned the model: load it and go straight to settings
        g_ui.model_path  = g_cli.model;
        g_ui.model_fixed = true;
        g_ui.models      = scan_models();
        if (!load_selected_model()) {
            rc = 1;
        }
    } else {
        g_ui.models = scan_models();
        if (g_ui.models.empty()) {
            g_ui.page = page_t::model;
        } else if (g_ui.models.size() == 1) {
            g_ui.model_path = g_ui.models[0].path;
            if (!load_selected_model()) {
                rc = 1;
            }
        } else {
            g_ui.page = page_t::model;
        }
    }

    if (g_cli.api) {
        // -api: start the server once the model is loaded
        set_api_enabled(true);
    }

    int last_rows = g_rows;
    int last_cols = g_cols;

    while (!g_signal && rc == 0) {

        term_size();
        if (g_rows != last_rows || g_cols != last_cols) {
            last_rows = g_rows;
            last_cols = g_cols;
            invalidate();
        }

        poll_generation();
        switch (g_ui.page) {
            case page_t::model:
                paint_frame(frame_model_picker());
                break;
            case page_t::settings:
                paint_frame(frame_settings());
                break;
            case page_t::run:
                paint_frame(frame_run());
                break;
        }

        const int key = read_key(150);
        if (key == K_NONE) {
            continue;
        }

        bool keep = true;
        switch (g_ui.page) {
            case page_t::model:    keep = handle_model_picker(key); break;
            case page_t::settings: keep = handle_settings(key);     break;
            case page_t::run:      keep = handle_run(key);          break;
        }
        if (!keep) {
            break;
        }
    }

    // ---- shutdown --------------------------------------------------------
    if (g_ui.job.running) {
        g_cancel_in_progress.store(true);
        eng.request_cancel();
        for (int i = 0; i < 400 && !g_ui.job.done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        diskui::gen_result r;
        g_ui.job.join_and_collect(r);
    }
    g_api.stop();
    g_ui.api_on = false;

    diskui::engine::get().set_llama_logger(nullptr, nullptr);
    llama_disk::set_msg_cb(nullptr);
    eng.unload();

    invalidate();
    term_leave();
    return rc;
}

// ===========================================================================
// headless API mode
// ===========================================================================

int run_api_only() {
    g_headless = true;
    install_signal_handlers();
#if !defined(_WIN32)
    // glibc accepts size 0 for line buffering; msVCRT does not (fast fail), and
    // the headless log already flushes after every line
    setvbuf(stdout, nullptr, _IOLBF, 0);
#endif

    diskui::engine & eng = diskui::engine::get();
    eng.set_log_sink([](const std::string & s) {
        fputs(s.c_str(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    });
    // same filter as the TUI: keep errors and the disk engine status, drop the
    // loader / graph dump chatter
    diskui::engine::get().set_llama_logger(llama_log_cb, nullptr);

    auto say = [](const std::string & s) {
        fputs(s.c_str(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    };
    say(i18n::trf("aonly.banner_fmt", current_pid()));

    std::string model_path = g_cli.model;
    if (model_path.empty()) {
        const std::vector<model_entry> models = scan_models();
        if (!models.empty()) {
            model_path = models[0].path;
            say(i18n::trf("aonly.use_first_fmt", models[0].name.c_str()));
        } else {
            std::string dirs;
            for (const auto & d : model_dirs()) {
                if (!dirs.empty()) {
                    dirs += " ";
                }
                dirs += d;
            }
            say(i18n::tr("aonly.no_model_dirs") + dirs);
            say(i18n::tr("aonly.no_model_note"));
        }
    }

    bool have_model = false;
    if (!model_path.empty()) {
        std::string err;
        have_model = eng.load(model_path, g_cli.moe, err);
        if (!have_model) {
            say(i18n::trf("aonly.load_failed_fmt", err.c_str()));
            say(i18n::tr("aonly.continue_fmt"));
        }
    }

    diskui::api::options opt;
    opt.port      = g_cli.port;
    opt.n_ctx     = std::max(16, g_cli.n_ctx_set ? g_cli.n_ctx : 256);
    opt.n_predict = g_cli.n_predict;
    opt.n_threads = g_cli.n_threads;
    opt.n_ubatch  = g_cli.n_ubatch;
    opt.temp      = g_cli.temp;
    opt.top_k     = g_cli.top_k;
    opt.top_p     = g_cli.top_p;

    std::string err;
    if (!g_api.start(opt, err)) {
        say("! " + err);
        eng.unload();
        return 1;
    }

    say(i18n::trf("aonly.listening_fmt", opt.port));
    say(i18n::trf("aonly.model_fmt",
                  have_model ? eng.basename().c_str() : std::string(i18n::tr("common.none")).c_str()));
    say(i18n::trf("aonly.ctx_fmt", opt.n_ctx, opt.n_predict, opt.n_threads, opt.n_ubatch,
                  opt.temp));
    say(i18n::tr("aonly.endpoints"));
    say(i18n::trf("aonly.stop_fmt", current_pid()));

    while (g_signal == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    say(i18n::trf("aonly.signal_fmt", (int) g_signal));
    g_api.stop();
    eng.unload();
    say(i18n::tr("aonly.bye"));
    return 0;
}

// ===========================================================================
// usage / main
// ===========================================================================

void print_usage() {
    fputs(i18n::tr("help.body"), stdout);
}

bool parse_args(int argc, char ** argv) {
    auto need = [&](int & i) -> const char * {
        return (i + 1 < argc) ? argv[++i] : nullptr;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            g_cli.help = true;
        } else if (a == "-m" && i + 1 < argc) {
            g_cli.model = argv[++i];
        } else if (a == "--api-only") {
            g_cli.api_only = true;
        } else if (a == "--api") {
            g_cli.api = true;
        } else if (a == "--moe") {
            g_cli.moe = true;
        } else if (a == "--chat") {
            g_cli.chat = true;
        } else if (a == "--raw") {
            g_cli.chat = false;
        } else if (a == "--sys" && i + 1 < argc) {
            g_cli.sysmsg = argv[++i];
        } else if (a == "-q" && i + 1 < argc) {
            g_cli.prompt = argv[++i];
            g_cli.prompt_set = true;
        } else if (a == "--prompt" && i + 1 < argc) {
            g_cli.prompt = argv[++i];
            g_cli.prompt_set = true;
        } else if ((a == "-c" || a == "--n-ctx") && i + 1 < argc) {
            g_cli.n_ctx = atoi(argv[++i]);
            g_cli.n_ctx_set = true;
        } else if ((a == "-n" || a == "--n-predict") && i + 1 < argc) {
            g_cli.n_predict = atoi(argv[++i]);
        } else if ((a == "-t" || a == "--threads") && i + 1 < argc) {
            g_cli.n_threads = atoi(argv[++i]);
        } else if ((a == "-ub" || a == "--ubatch") && i + 1 < argc) {
            g_cli.n_ubatch = std::max(1, atoi(argv[++i]));
        } else if (a == "--temp" && i + 1 < argc) {
            g_cli.temp = (float) atof(argv[++i]);
        } else if (a == "--top-k" && i + 1 < argc) {
            g_cli.top_k = atoi(argv[++i]);
        } else if (a == "--top-p" && i + 1 < argc) {
            g_cli.top_p = (float) atof(argv[++i]);
        } else if (a == "--port" && i + 1 < argc) {
            g_cli.port = atoi(argv[++i]);
        } else if (a == "--arena" && i + 1 < argc) {
            const int mb = atoi(argv[++i]);
            if (mb > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", mb);
#if defined(_WIN32)
                _putenv_s("LLAMA_DISK_STREAM_MB", buf);
#else
                setenv("LLAMA_DISK_STREAM_MB", buf, 1);
#endif
            }
        } else if (a == "--mem-probe") {
            g_cli.mem_probe = true;
        } else if (a == "--debug") {
#if defined(_WIN32)
            _putenv_s("LLAMA_DISK_DEBUG", "1");
#else
            setenv("LLAMA_DISK_DEBUG", "1", 1);
#endif
        } else if (a == "--api-ctx" && i + 1 < argc) {
            g_cli.n_ctx = atoi(argv[++i]);
            g_cli.n_ctx_set = true;
        } else if (!a.empty() && a[0] != '-') {
            g_cli.prompt = a;
            g_cli.prompt_set = true;
            // swallow the rest as part of the prompt, like the old TUI did
            for (++i; i < argc; ++i) {
                g_cli.prompt += " ";
                g_cli.prompt += argv[i];
            }
            --i;
        } else {
            fprintf(stderr, "%s\n", i18n::trf("misc.unknown_arg_fmt", a.c_str()).c_str());
        }
    }
    (void) need;
    return true;
}

int run_cli_fallback() {
    // called when stdin/stdout is not a terminal: never paint escape codes into a
    // pipe, explain what to do instead and exit with a distinct code
    fputs(i18n::tr("misc.no_tty"), stderr);
    return 2;
}

// --mem-probe: load the model, create a throw-away context with the requested n_ctx /
// prefill batch, report the RAM the next run will need (used by CI / documentation and
// for checking the estimate on a new device without running a full inference)
int run_mem_probe() {
    if (g_cli.model.empty()) {
        fputs("mem-probe: -m PATH is required\n", stderr);
        return 1;
    }
    diskui::engine & eng = diskui::engine::get();
    eng.set_log_sink([](const std::string & s) { fprintf(stderr, "%s\n", s.c_str()); });

    std::string err;
    if (!eng.load(g_cli.model, g_cli.moe, err, 0)) {
        fprintf(stderr, "mem-probe: load failed: %s\n", err.c_str());
        return 1;
    }
    const diskui::mem_estimate e = eng.probe_memory(g_cli.n_ctx, g_cli.n_ubatch);
    if (!e.ok) {
        fprintf(stderr, "mem-probe: %s\n", e.err.c_str());
        return 1;
    }
    auto mib = [](unsigned long long b) { return (double) b / 1024.0 / 1024.0; };
    printf("mem-probe model=%s ctx=%d ubatch=%d\n", g_cli.model.c_str(), g_cli.n_ctx, g_cli.n_ubatch);
    printf("mem-probe arena=%.1fMiB model_meta=%.1fMiB kv=%.1fMiB recurrent=%.1fMiB "
           "compute=%.1fMiB scratch=%.1fMiB\n",
           mib(e.arena), mib(e.model), mib(e.kv), mib(e.recurrent), mib(e.compute), mib(e.scratch));
    printf("mem-probe base_rss=%.1fMiB resident=%.1fMiB peak=%.1fMiB\n",
           mib(e.base_rss), mib(e.resident), mib(e.peak));
    return 0;
}

} // namespace

// ===========================================================================
// entry point
// ===========================================================================

int main(int argc, char ** argv) {
#if defined(_WIN32)
    // utf-8 console for the CLI / headless output, restored at exit
    win_console_enter();
    atexit(win_console_leave);
#endif
    // --lang is parsed and removed here, so the rest of the program (help, usage,
    // TUI and log text) is already in the right language
    i18n::init(argc, argv);
    int argc_eff = 0;
    while (argc_eff < argc && argv[argc_eff] != nullptr) {
        ++argc_eff;
    }
    parse_args(argc_eff, argv);

    if (g_cli.help) {
        print_usage();
        return 0;
    }
    if (g_cli.n_ctx < 3) {
        g_cli.n_ctx = 3;
    }
    if (g_cli.n_predict < 1) {
        g_cli.n_predict = 1;
    }
    if (g_cli.n_threads < 1) {
        g_cli.n_threads = 1;
    }
    if (g_cli.n_ubatch < 1) {
        g_cli.n_ubatch = 1;
    }
    if (g_cli.temp < 0.0f) {
        g_cli.temp = 0.0f;
    }
    if (g_cli.temp > 1.0f) {
        g_cli.temp = 1.0f;
    }

    if (g_cli.mem_probe) {
        return run_mem_probe();
    }

    if (g_cli.api_only) {
        return run_api_only();
    }

    if (!tty_available()) {
        return run_cli_fallback();
    }

    try {
        return run_tui();
    } catch (const std::exception & e) {
        term_leave();
        fprintf(stderr, "%s\n", i18n::trf("misc.fatal_fmt", e.what()).c_str());
        return 1;
    } catch (...) {
        term_leave();
        fprintf(stderr, "%s\n", i18n::tr("misc.fatal_unknown"));
        return 1;
    }
}
