// engine.cpp: model loading, single turn inference, cancellation, live stats
//
// the heavy lifting is the same llama.cpp sequence the old single-turn TUI used:
// tokenize, prefill in ubatches, sample a token, feed it back, repeat. what is
// new is that everything is observable (live_state) and that two front ends
// share it: the TUI and the HTTP API.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "engine.h"

#include "i18n.h"

#include "llama-disk.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/stat.h>
#endif

namespace diskui {

namespace {

// resident set size of this process right now (0 when the platform has no /proc)
unsigned long long rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (unsigned long long) pmc.WorkingSetSize;
    }
    return 0;
#else
    FILE * f = fopen("/proc/self/status", "r");
    if (f == nullptr) {
        return 0;
    }
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = strtoull(line + 6, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return kb * 1024ull;
#endif
}

// MemAvailable in bytes (0 when unknown)
unsigned long long mem_available_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        return (unsigned long long) ms.ullAvailPhys;
    }
    return 0;
#else
    FILE * f = fopen("/proc/meminfo", "r");
    if (f == nullptr) {
        return 0;
    }
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            kb = strtoull(line + 13, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return kb * 1024ull;
#endif
}

unsigned long long file_bytes(const std::string & path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER sz;
    unsigned long long out = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        out = (unsigned long long) sz.QuadPart;
    }
    CloseHandle(h);
    return out;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size < 0) {
        return 0;
    }
    return (unsigned long long) st.st_size;
#endif
}

} // namespace

// ------------------------------------------------------------------ small utils

int64_t now_us() {
    return (int64_t) ggml_time_us();
}

std::string human_bytes(unsigned long long n) {
    char buf[64];
    if (n >= (1ull << 30)) {
        snprintf(buf, sizeof(buf), "%.2f GiB", (double) n / (double) (1ull << 30));
    } else if (n >= (1ull << 20)) {
        snprintf(buf, sizeof(buf), "%.1f MiB", (double) n / (double) (1ull << 20));
    } else if (n >= (1ull << 10)) {
        snprintf(buf, sizeof(buf), "%.1f KiB", (double) n / (double) (1ull << 10));
    } else {
        snprintf(buf, sizeof(buf), "%llu B", n);
    }
    return buf;
}

std::string fmt_seconds(double s) {
    char buf[32];
    if (s < 60.0) {
        snprintf(buf, sizeof(buf), "%.1fs", s);
    } else {
        const int m = (int) (s / 60.0);
        snprintf(buf, sizeof(buf), "%d:%04.1f", m, s - 60.0 * m);
    }
    return buf;
}

namespace {

void set_env(const char * k, const char * v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

void set_env_default(const char * k, const char * v) {
    if (getenv(k) == nullptr) {
        set_env(k, v);
    }
}

void unset_env(const char * k) {
#if defined(_WIN32)
    _putenv_s(k, "");   // empty value removes the variable from the CRT environment
#else
    unsetenv(k);
#endif
}

bool ends_with(const std::string & s, const char * suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::vector<llama_token> tokenize(const llama_vocab * vocab, const std::string & text) {
    int n = -llama_tokenize(vocab, text.data(), (int32_t) text.size(), nullptr, 0, true, true);
    if (n <= 0) {
        return {};
    }
    std::vector<llama_token> toks((size_t) n);
    if (llama_tokenize(vocab, text.data(), (int32_t) text.size(), toks.data(), toks.size(), true, true) < 0) {
        return {};
    }
    return toks;
}

std::string token_piece(const llama_vocab * vocab, llama_token id) {
    char buf[256];
    const int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, (size_t) n);
}

// internal finish reason -> localised text for the [gen] log line
const char * finish_text(const std::string & f) {
    if (f == "length")    return i18n::tr("eng.finish_length");
    if (f == "cancelled") return i18n::tr("eng.finish_cancelled");
    if (f == "error")     return i18n::tr("eng.finish_error");
    return i18n::tr("eng.finish_stop");
}

// apply the model's chat template to a conversation. models whose template keeps
// a thinking block (qwen3 style) get it closed so the answer starts right away.
std::string format_chat(const llama_model * model, const std::vector<chat_message> & msgs) {
    const char * tmpl = llama_model_chat_template(model, nullptr);

    std::string prompt;
    bool templated = false;

    if (tmpl != nullptr) {
        std::vector<llama_chat_message> cmsgs;
        cmsgs.reserve(msgs.size());
        for (const auto & m : msgs) {
            cmsgs.push_back(llama_chat_message{ m.role.c_str(), m.content.c_str() });
        }
        if (!cmsgs.empty()) {
            const int32_t need = llama_chat_apply_template(tmpl, cmsgs.data(), cmsgs.size(), true, nullptr, 0);
            if (need > 0) {
                std::string buf((size_t) need, '\0');
                const int32_t n = llama_chat_apply_template(tmpl, cmsgs.data(), cmsgs.size(), true,
                                                            buf.data(), (int32_t) buf.size());
                if (n > 0) {
                    buf.resize((size_t) n);
                    prompt    = buf;
                    templated = true;
                }
            }
        }
    }

    if (!templated) {
        // no usable template: a plain role: content transcript still works for
        // instruct-style checkpoints and keeps the API usable
        prompt.clear();
        for (const auto & m : msgs) {
            prompt += m.role;
            prompt += ": ";
            prompt += m.content;
            prompt += "\n";
        }
        prompt += "assistant: ";
        return prompt;
    }

    if (std::strstr(tmpl, "think") != nullptr && ends_with(prompt, "assistant\n")) {
        prompt += "<think>\n\n</think>\n\n";
    }
    return prompt;
}

} // namespace

// --------------------------------------------------------------------- engine

engine::engine() {
    std::setlocale(LC_NUMERIC, "C");
    // remember the launcher / --arena value so the TUI can switch back to "auto"
    if (const char * v = getenv("LLAMA_DISK_STREAM_MB")) {
        m_env_stream_orig     = v;
        m_env_stream_orig_set = true;
    }
    llama_backend_init();
}

engine::~engine() {
    unload();
    llama_backend_free();
}

engine & engine::get() {
    static engine e;
    return e;
}

void engine::set_log_sink(std::function<void(const std::string &)> sink) {
    std::lock_guard<std::mutex> lk(m_log_mtx);
    m_log_sink = std::move(sink);
}

void engine::log(const std::string & line) {
    std::function<void(const std::string &)> sink;
    {
        std::lock_guard<std::mutex> lk(m_log_mtx);
        sink = m_log_sink;
    }
    if (sink) {
        sink(line);
    }
}

// ---- slot ------------------------------------------------------------------

bool engine::acquire(owner who, const std::string & label) {
    if (!m_slot.try_lock()) {
        return false;
    }
    m_owner.store((int) who);
    {
        std::lock_guard<std::mutex> lk(m_owner_mtx);
        m_owner_label = label;
    }
    return true;
}

void engine::release() {
    m_owner.store((int) owner::none);
    {
        std::lock_guard<std::mutex> lk(m_owner_mtx);
        m_owner_label.clear();
    }
    m_slot.unlock();
}

bool engine::busy() const {
    if (m_slot.try_lock()) {
        m_slot.unlock();
        return false;
    }
    return true;
}

owner engine::busy_owner() const {
    if (!busy()) {
        return owner::none;
    }
    return (owner) m_owner.load();
}

std::string engine::busy_label() const {
    std::lock_guard<std::mutex> lk(m_owner_mtx);
    return m_owner_label;
}

// ---- cancel ----------------------------------------------------------------

void engine::request_cancel() {
    m_cancel.store(true);
}

void engine::clear_cancel() {
    m_cancel.store(false);
}

bool engine::cancel_requested() const {
    return m_cancel.load();
}

// ---- model -----------------------------------------------------------------

bool engine::load(const std::string & path, bool moe, std::string & err, int arena_mb) {
    unload();

    const bool odirect_user = getenv("LLAMA_DISK_ODIRECT") != nullptr;
    const unsigned long long rss_before = rss_bytes();

    // defaults for the disk streaming layer, the caller can override any of them
    set_env_default("LLAMA_DISK_ODIRECT",   "1");
    set_env_default("LLAMA_DISK_MLOCK",     "1");
    set_env_default("LLAMA_DISK_PREFETCH",  "0");
    set_env_default("LLAMA_DISK_WINDOW",    "0");
    set_env_default("LLAMA_DISK_OUT_CHUNK", "0");
    set_env_default("GGML_CPU_DISABLE_FUSION", "1");

    // the TUI can override the arena size per load; 0 means keep the value the
    // launcher / --arena put into the environment (auto)
    m_arena_mb = arena_mb > 0 ? arena_mb : 0;
    if (m_arena_mb > 0) {
        char abuf[32];
        snprintf(abuf, sizeof(abuf), "%d", m_arena_mb);
        set_env("LLAMA_DISK_STREAM_MB", abuf);
    } else if (m_env_stream_orig_set) {
        set_env("LLAMA_DISK_STREAM_MB", m_env_stream_orig.c_str());
    } else {
        unset_env("LLAMA_DISK_STREAM_MB");
    }

    // the MoE fast path reads its switch before the model is registered, so it
    // has to be set here and not at generation time
    set_env("LLAMA_DISK_MOE", moe ? "1" : "0");

    // check the file before handing it to the loader: a bad path would crash
    // inside llama.cpp's error path instead of reporting anything useful
    FILE * f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        err = i18n::trf("eng.cannot_open_fmt", path.c_str());
        return false;
    }
    char magic[4] = { 0, 0, 0, 0 };
    const size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n != 4 || memcmp(magic, "GGUF", 4) != 0) {
        err = i18n::trf("eng.not_gguf_fmt", path.c_str());
        return false;
    }

    // read path: buffered reads let the page cache absorb the repeated full-model scans
    // (once the model is warm nothing touches the flash any more, which is both much
    // faster and much easier on the storage); direct io avoids filling the RAM when the
    // model does not fit anyway. LLAMA_DISK_ODIRECT=0/1 in the environment overrides this.
    if (!odirect_user) {
        const unsigned long long avail = mem_available_bytes();
        const unsigned long long fsize = file_bytes(path);
        const bool cache = fsize > 0 && avail > 0 && fsize <= avail * 45 / 100;
        set_env("LLAMA_DISK_ODIRECT", cache ? "0" : "1");
        log(cache ? i18n::trf("eng.cache_on_fmt", human_bytes(fsize).c_str(), human_bytes(avail).c_str())
                  : i18n::trf("eng.cache_off_fmt", human_bytes(fsize).c_str(), human_bytes(avail).c_str()));
    }

    const int64_t t0 = now_us();
    log(i18n::trf("eng.load_path_fmt", path.c_str()));
    log(i18n::tr(moe ? "eng.moe_on" : "eng.moe_off"));

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;

    llama_model * model = nullptr;
    try {
        model = llama_model_load_from_file(path.c_str(), mp);
    } catch (const std::exception & e) {
        err = i18n::trf("eng.load_threw_fmt", e.what());
    } catch (...) {
        err = i18n::tr("eng.load_threw");
    }
    if (model == nullptr) {
        if (err.empty()) {
            err = i18n::tr("eng.load_failed");
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        m_model       = model;
        m_vocab       = llama_model_get_vocab(model);
        m_path        = path;
        m_n_ctx_train = (int) llama_model_n_ctx_train(model);
        m_n_layer     = (int) llama_model_n_layer(model);
        m_moe_loaded  = moe;
        m_base_rss    = rss_bytes();
        m_model_ram   = m_base_rss > rss_before ? m_base_rss - rss_before : 0;
        m_mem_cache   = mem_estimate();
        m_mem_cache_ctx = -1;
        m_mem_cache_ub  = -1;
        m_mem_cache_model = nullptr;
    }

    llama_disk::llama_disk_stats s;
    llama_disk::get_stats(&s);

    // on some platforms (windows) the arena is committed during the load, so the RSS delta
    // already contains it; strip it from the model-metadata figure in that case. on
    // linux/android the arena pages are lazy, so the delta is the metadata/tokenizer size
    if (s.arena_bytes > 0 && m_model_ram > s.arena_bytes + (8ull << 20)) {
        m_model_ram -= s.arena_bytes;
    }

    char desc[512] = { 0 };
    llama_model_desc(model, desc, sizeof(desc));
    log(i18n::trf("eng.ready_fmt", fmt_seconds((now_us() - t0) / 1e6).c_str(), desc));
    log(i18n::trf("eng.info_fmt", m_n_ctx_train, m_n_layer.load(),
                  (int) llama_vocab_n_tokens(m_vocab),
                  human_bytes(s.arena_bytes).c_str(), s.direct_io,
                  llama_disk::enabled() ? i18n::tr("common.on") : i18n::tr("common.off")));
    return true;
}

void engine::unload() {
    llama_model * model = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        model       = m_model;
        m_model     = nullptr;
        m_vocab     = nullptr;
        m_path.clear();
        m_n_ctx_train = 0;
        m_n_layer     = 0;
        m_moe_loaded  = false;
    }
    if (model != nullptr) {
        llama_model_free(model);   // also calls llama_disk::cleanup()
    }
}

bool engine::loaded() const {
    std::lock_guard<std::mutex> lk(m_model_mtx);
    return m_model != nullptr;
}

bool engine::loaded_with_moe() const {
    return m_moe_loaded.load();
}

const std::string & engine::path() const {
    return m_path;   // stable: only the main thread loads/unloads
}

std::string engine::basename() const {
    std::string p = m_path;
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string engine::desc() const {
    std::lock_guard<std::mutex> lk(m_model_mtx);
    if (m_model == nullptr) {
        return i18n::tr("eng.no_model");
    }
    char buf[512] = { 0 };
    llama_model_desc(m_model, buf, sizeof(buf));
    return buf;
}

int engine::n_ctx_train() const {
    return m_n_ctx_train;
}

int engine::n_layer() const {
    return m_n_layer.load();
}

int engine::requested_arena_mb() const {
    return m_arena_mb;
}

unsigned long long engine::model_ram() const {
    return m_model_ram;
}

void engine::set_llama_logger(llama_logger_t cb, void * ud) {
    m_llama_logger    = cb;
    m_llama_logger_ud = ud;
    llama_log_set(cb, ud);
}

// the size lines llama.cpp prints while a context is created are the exact numbers for
// KV cache / recurrent state / graph compute buffer; parse them out of a throw-away
// context and cache the result per (model, n_ctx, n_ubatch)
mem_estimate engine::probe_memory(int n_ctx, int n_ubatch) {
    mem_estimate e;

    llama_model * model = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        model = m_model;
        if (m_mem_cache.ok && m_mem_cache_model == model &&
                m_mem_cache_ctx == n_ctx && m_mem_cache_ub == n_ubatch) {
            return m_mem_cache;
        }
    }
    if (model == nullptr) {
        e.err = "no model loaded";
        return e;
    }

    n_ctx    = std::max(n_ctx, 4);
    n_ubatch = std::max(n_ubatch, 1);

    std::vector<std::string> lines;
    static auto capture_cb = [](enum ggml_log_level, const char * text, void * ud) {
        if (text != nullptr) {
            static_cast<std::vector<std::string> *>(ud)->push_back(text);
        }
    };
    llama_log_set(capture_cb, &lines);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx    = (uint32_t) n_ctx;
    cp.n_batch  = (uint32_t) n_ubatch;
    cp.n_ubatch = (uint32_t) n_ubatch;
    cp.n_threads = 4;
    cp.no_perf   = true;
    llama_context * ctx = llama_init_from_model(model, cp);

    llama_log_set(m_llama_logger, m_llama_logger_ud);

    if (ctx == nullptr) {
        e.err = "context creation failed";
        return e;
    }
    llama_free(ctx);

    auto parse_size = [&lines](const char * needle) -> unsigned long long {
        for (const auto & l : lines) {
            const char * p = strstr(l.c_str(), needle);
            if (p == nullptr) {
                continue;
            }
            p = strchr(p, '=');
            if (p == nullptr) {
                continue;
            }
            const double mib = atof(p + 1);
            if (mib > 0.0) {
                return (unsigned long long) (mib * 1024.0 * 1024.0);
            }
        }
        return 0;
    };

    e.kv        = parse_size("llama_kv_cache: size =");
    e.recurrent = parse_size("llama_memory_recurrent: size =");
    e.compute   = parse_size("CPU compute buffer size =");
    if (e.compute == 0) {
        e.compute = parse_size("CPU compute buffer size is");
    }

    llama_disk::llama_disk_stats ds;
    llama_disk::get_stats(&ds);

    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        e.model  = m_model_ram;
        e.arena  = ds.arena_bytes;
        e.base_rss = m_base_rss > 0 ? m_base_rss : rss_bytes();
    }

    const int n_vocab = m_vocab != nullptr ? (int) llama_vocab_n_tokens(m_vocab) : 0;
    const int n_embd  = (int) llama_model_n_embd(model);

    // prefill batches hold the token ids / embeddings and one logits row per output token
    e.scratch = (unsigned long long) n_ubatch * (unsigned long long) n_embd * 4ull
              + (unsigned long long) n_vocab * 4ull
              + (2ull << 20);

    // the arena is allocated but only touched as the weights are streamed: at most the
    // whole arena (or the whole model, whichever is smaller) becomes resident
    const unsigned long long arena_used = ds.registered_bytes > 0
        ? std::min<unsigned long long>(ds.arena_bytes, ds.registered_bytes)
        : ds.arena_bytes;
    e.resident = e.model + e.kv + e.recurrent + arena_used;
    e.peak     = e.resident + e.compute + e.scratch;

    e.ok = true;

    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        m_mem_cache       = e;
        m_mem_cache_ctx   = n_ctx;
        m_mem_cache_ub    = n_ubatch;
        m_mem_cache_model = model;
    }
    return e;
}

bool engine::has_chat_template() const {
    std::lock_guard<std::mutex> lk(m_model_mtx);
    if (m_model == nullptr) {
        return false;
    }
    return llama_model_chat_template(m_model, nullptr) != nullptr;
}

int engine::chat_template_extra_tokens() {
    llama_model * model = nullptr;
    const llama_vocab * vocab = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        model = m_model;
        vocab = m_vocab;
    }
    if (model == nullptr || vocab == nullptr || llama_model_chat_template(model, nullptr) == nullptr) {
        return -1;
    }

    const std::vector<chat_message> one = { { "user", "hi" } };
    const std::string templated = format_chat(model, one);
    const std::string bare      = "hi";

    const std::vector<llama_token> a = tokenize(vocab, templated);
    const std::vector<llama_token> b = tokenize(vocab, bare);
    if (a.empty() || b.empty()) {
        return -1;
    }
    const int extra = (int) a.size() - (int) b.size();
    return extra > 0 ? extra : 0;
}

// ---- live state ------------------------------------------------------------

live_state engine::snapshot() const {
    std::lock_guard<std::mutex> lk(m_live_mtx);
    return m_live;
}

void engine::live_begin(owner who, const std::string & label, int total) {
    std::lock_guard<std::mutex> lk(m_live_mtx);
    m_live = live_state();
    m_live.active   = true;
    m_live.who      = who;
    m_live.who_label = label;
    m_live.total    = total;
    m_live.n_prompt = total;
    m_live.n_layer  = m_n_layer.load();
    m_live.moe      = m_moe_loaded.load() ? 1 : 0;
    m_live.phase    = 2;
    m_run_t0   = now_us();
    m_io_acc   = 0.0;
    m_cmp_acc  = 0.0;
}

void engine::live_end() {
    std::lock_guard<std::mutex> lk(m_live_mtx);
    m_live.active = false;
    m_live.who    = owner::none;
    m_live.phase  = 0;
}

void engine::live_phase(int phase, int done, int total, const std::string & piece) {
    std::lock_guard<std::mutex> lk(m_live_mtx);
    m_live.phase = phase;
    m_live.done  = done;
    m_live.total = total;
    if (!piece.empty()) {
        m_live.piece = piece;
    }
    m_live.io_s  = m_io_acc;
    m_live.cmp_s = m_cmp_acc;
}

// ---- generation ------------------------------------------------------------

gen_result engine::generate_chat(const std::vector<chat_message> & msgs, const gen_params & gp) {
    llama_model * model = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        model = m_model;
    }
    if (model == nullptr) {
        gen_result r;
        r.finish = "error";
        r.error  = "no model loaded";
        return r;
    }
    return generate_impl(format_chat(model, msgs), gp);
}

gen_result engine::generate_raw(const std::string & prompt, const gen_params & gp) {
    return generate_impl(prompt, gp);
}

gen_result engine::generate_impl(const std::string & prompt_text, const gen_params & gp) {
    gen_result res;

    llama_model * model = nullptr;
    const llama_vocab * vocab = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_model_mtx);
        model = m_model;
        vocab = m_vocab;
    }
    if (model == nullptr || vocab == nullptr) {
        res.finish = "error";
        res.error  = "no model loaded";
        return res;
    }

    clear_cancel();

    std::vector<llama_token> toks = tokenize(vocab, prompt_text);
    if (toks.empty()) {
        res.finish = "error";
        res.error  = "failed to tokenize the prompt";
        return res;
    }
    res.n_prompt = (int) toks.size();

    int n_ctx = std::max(gp.n_ctx, 4);
    if (gp.auto_fit_ctx) {
        const int want = (int) toks.size() + gp.n_predict + 8;
        n_ctx = std::max(n_ctx, want);
    }
    const int n_ctx_train = m_n_ctx_train;
    if (n_ctx_train > 0) {
        n_ctx = std::min(n_ctx, n_ctx_train);
    }

    // the prompt has to leave at least one slot for the first generated token
    if ((int) toks.size() + 1 > n_ctx) {
        const int keep = n_ctx - 1;
        if (keep < 1) {
            res.finish = "error";
            res.error  = "context is too small for this prompt";
            return res;
        }
        log(i18n::trf("eng.warn_prompt_trim_fmt", (int) toks.size(), n_ctx, keep));
        toks.erase(toks.begin(), toks.end() - keep);
        res.n_prompt = (int) toks.size();
    }

    int n_predict = gp.n_predict;
    if (n_predict < 1) {
        n_predict = 1;
    }
    if ((int) toks.size() + n_predict > n_ctx) {
        n_predict = n_ctx - (int) toks.size();
        if (n_predict < 1) {
            n_predict = 1;
        }
        log(i18n::trf("eng.warn_npredict_fmt", n_ctx, n_predict));
    }

    // one context per run: cheap, and it lets the TUI change n_ctx without
    // reloading a multi-gigabyte model
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx            = (uint32_t) n_ctx;
    cp.n_batch          = (uint32_t) std::max(gp.n_ubatch, 1);
    cp.n_ubatch         = (uint32_t) std::max(gp.n_ubatch, 1);
    cp.n_threads        = (uint32_t) std::max(gp.n_threads, 1);
    cp.n_threads_batch  = (uint32_t) std::max(gp.n_threads, 1);
    cp.no_perf          = false;

    llama_context * ctx = llama_init_from_model(model, cp);
    if (ctx == nullptr) {
        res.finish = "error";
        res.error  = "failed to create a llama_context";
        return res;
    }

    // cooperative abort: checked by the CPU backend between graph nodes, so a
    // cancel takes effect inside a decode instead of only between decodes
    llama_set_abort_callback(ctx, [](void * ud) -> bool {
        return static_cast<engine *>(ud)->cancel_requested();
    }, this);

    const llama_memory_t mem = llama_get_memory(ctx);
    llama_memory_clear(mem, true);

    const owner who = (owner) m_owner.load();
    std::string owner_label;
    {
        std::lock_guard<std::mutex> lk(m_owner_mtx);
        owner_label = m_owner_label;
    }
    live_begin(who, owner_label, res.n_prompt);

    std::string text;
    std::string finish = "length";
    (void) 0;
    int    n_past = 0;
    int    n_gen  = 0;

    llama_batch batch = llama_batch_init(std::max(gp.n_ubatch, 1), 0, 1);
    llama_sampler * smpl = nullptr;

    auto cleanup = [&]() {
        if (smpl != nullptr) {
            llama_sampler_free(smpl);
        }
        llama_batch_free(batch);
        llama_free(ctx);
    };

    // ---- prefill
    const int64_t t_pp0 = now_us();
    bool failed = false;
    bool cancel = false;
    for (int i = 0; i < (int) toks.size(); i += gp.n_ubatch) {
        if (cancel_requested()) {
            cancel = true;
            break;
        }
        const int n = std::min(gp.n_ubatch, (int) toks.size() - i);
        batch.n_tokens = n;
        for (int j = 0; j < n; ++j) {
            batch.token[j]     = toks[(size_t) (i + j)];
            batch.pos[j]       = (llama_pos) (i + j);
            batch.n_seq_id[j]  = 1;
            batch.seq_id[j][0] = 0;
            batch.logits[j]    = (i + j == (int) toks.size() - 1) ? 1 : 0;
        }

        llama_disk::llama_disk_stats s0, s1;
        llama_disk::get_stats(&s0);
        const int64_t t0 = now_us();
        live_phase(2, i + 1, (int) toks.size(), token_piece(vocab, toks[(size_t) i]));
        const int rc = llama_decode(ctx, batch);
        const int64_t t1 = now_us();
        llama_disk::get_stats(&s1);
        if (rc != 0) {
            if (cancel_requested()) {
                // the abort callback made llama_decode bail out: that is a cancel,
                // not a failure
                cancel = true;
            } else {
                res.error  = "prefill decode failed at token " + std::to_string(i);
                res.finish = "error";
                failed = true;
            }
            break;
        }
        const double io_s  = (s1.io_us - s0.io_us) / 1e6;
        const double tot_s = (t1 - t0) / 1e6;
        m_io_acc  += io_s;
        m_cmp_acc += std::max(0.0, tot_s - io_s);
        live_phase(2, i + n, (int) toks.size(), token_piece(vocab, toks[(size_t) (i + n - 1)]));

        log(i18n::trf("eng.pp_fmt", i + n, (int) toks.size(), io_s,
                      std::max(0.0, tot_s - io_s),
                      token_piece(vocab, toks[(size_t) (i + n - 1)]).c_str()));
        n_past = i + n;
    }

    const double pp_s = (now_us() - t_pp0) / 1e6;
    if (cancel || failed) {
        res.ok     = false;
        res.finish = cancel ? "cancelled" : "error";
        if (cancel) {
            res.error = "";
            log(i18n::tr("eng.cancel_prefill"));
        }
        {
            std::lock_guard<std::mutex> lk(m_live_mtx);
            m_live.pp_s  = pp_s;
            m_live.io_s  = m_io_acc;
            m_live.cmp_s = m_cmp_acc;
        }
        live_end();
        cleanup();
        return res;
    }

    {
        std::lock_guard<std::mutex> lk(m_live_mtx);
        m_live.pp_s  = pp_s;
        m_live.io_s  = m_io_acc;
        m_live.cmp_s = m_cmp_acc;
    }
    log(i18n::trf("eng.pp_done_fmt", res.n_prompt, fmt_seconds(pp_s).c_str(),
                  res.n_prompt > 0 ? pp_s / res.n_prompt : 0.0,
                  fmt_seconds(m_io_acc).c_str()));

    // ---- sampler
    {
        auto sparams = llama_sampler_chain_default_params();
        smpl = llama_sampler_chain_init(sparams);
        if (gp.temp <= 0.0f) {
            llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
        } else {
            llama_sampler_chain_add(smpl, llama_sampler_init_top_k(gp.top_k));
            llama_sampler_chain_add(smpl, llama_sampler_init_top_p(gp.top_p, 1));
            llama_sampler_chain_add(smpl, llama_sampler_init_temp(gp.temp));
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        }
    }

    // ---- generation
    const int64_t t_gen0 = now_us();
    for (int step = 0; step < n_predict; ++step) {
        if (cancel_requested()) {
            finish = "cancelled";
            break;
        }
        if (n_past + 1 >= n_ctx) {
            finish = "length";
            break;
        }

        const llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) {
            finish = "stop";
            break;
        }

        const std::string piece = token_piece(vocab, tok);
        text += piece;

        batch.n_tokens = 1;
        batch.token[0]     = tok;
        batch.pos[0]       = (llama_pos) n_past;
        batch.n_seq_id[0]  = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]    = 1;
        n_past++;
        n_gen++;

        llama_disk::llama_disk_stats s0, s1;
        llama_disk::get_stats(&s0);
        const int64_t t0 = now_us();
        {
            std::lock_guard<std::mutex> lk(m_live_mtx);
            m_live.phase = 3;
            m_live.done  = n_gen;
            m_live.total = n_predict;
            m_live.piece = piece;
            m_live.answer = text;
            m_live.n_gen  = n_gen;
        }
        const int rc = llama_decode(ctx, batch);
        const int64_t t1 = now_us();
        llama_disk::get_stats(&s1);
        if (rc != 0) {
            if (cancel_requested()) {
                finish = "cancelled";
            } else {
                res.error = "decode failed at token " + std::to_string(n_gen);
                finish    = "error";
                failed    = true;
            }
            break;
        }
        const double io_s  = (s1.io_us - s0.io_us) / 1e6;
        const double tot_s = (t1 - t0) / 1e6;
        m_io_acc  += io_s;
        m_cmp_acc += std::max(0.0, tot_s - io_s);
        {
            std::lock_guard<std::mutex> lk(m_live_mtx);
            m_live.io_s  = m_io_acc;
            m_live.cmp_s = m_cmp_acc;
            m_live.layer = s1.layer;
            m_live.n_layer = s1.n_layer;
            m_live.loads     = s1.loads;
            m_live.bytes     = s1.bytes;
            m_live.evictions = s1.evictions;
#if defined(DISKUI_HAVE_MOE_STATS)
            m_live.moe              = s1.moe;
            m_live.moe_nodes        = s1.moe_nodes;
            m_live.moe_experts      = s1.moe_experts;
            m_live.moe_expert_bytes = s1.moe_expert_bytes;
#endif
        }

        if (who == owner::tui) {
            log(i18n::trf("eng.gen_line_fmt", n_gen, io_s,
                          std::max(0.0, tot_s - io_s), tot_s, piece.c_str()));
        }
    }

    const double gen_s = (now_us() - t_gen0) / 1e6;

    res.ok       = !failed;
    res.text     = text;
    res.finish   = failed ? "error" : finish;
    res.n_gen    = n_gen;
    res.pp_s     = pp_s;
    res.gen_s    = gen_s;

    {
        std::lock_guard<std::mutex> lk(m_live_mtx);
        m_live.answer = text;
        m_live.n_gen  = n_gen;
        m_live.gen_s  = gen_s;
        m_live.pp_s   = pp_s;
        m_live.io_s   = m_io_acc;
        m_live.cmp_s  = m_cmp_acc;
    }

    if (finish == "cancelled") {
        // one clean line for a user cancel, no decode error noise
        log(i18n::trf("eng.cancel_gen_fmt", n_gen));
    } else {
        log(i18n::trf("eng.gen_done_fmt", n_gen, fmt_seconds(gen_s).c_str(),
                      gen_s > 0.0 ? n_gen / gen_s : 0.0, finish_text(res.finish),
                      fmt_seconds(m_io_acc).c_str(), fmt_seconds(m_cmp_acc).c_str()));
    }

    live_end();
    cleanup();
    return res;
}

} // namespace diskui
