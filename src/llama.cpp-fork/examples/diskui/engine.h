// engine: model loading, single slot inference, cancellation and live stats
//
// shared by the interactive TUI (diskui.cpp) and the HTTP API (api.cpp) so that
// both drive exactly the same llama_context setup and the same disk counters.
//
// the engine owns at most one llama_model at a time and allows at most one
// inference at a time: acquire() is a try_lock, so the TUI and the API can never
// run a decode at the same time. the loser gets false and reports 429 / "busy".
#pragma once

#include "llama.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace diskui {

struct chat_message {
    std::string role;
    std::string content;
};

struct gen_params {
    int   n_ctx     = 16;
    int   n_predict = 256;
    float temp      = 0.70f;
    int   top_k     = 40;
    float top_p     = 0.95f;
    int   n_ubatch  = 4;
    int   n_threads = 4;
    // API mode: grow n_ctx so prompt + n_predict fits (capped by n_ctx_train),
    // the TUI keeps the context size the user picked and shortens n_predict instead
    bool  auto_fit_ctx = false;
};

enum class owner { none, tui, api };

// live view of the running inference, polled by the TUI painter thread
struct live_state {
    bool  active = false;
    owner who    = owner::none;
    std::string who_label;   // e.g. "API /v1/chat/completions"
    int   phase  = 0;        // 0 idle, 1 loading, 2 prefill, 3 generate
    int   done   = 0;
    int   total  = 0;
    std::string piece;       // most recent decoded token
    std::string answer;      // generated text so far
    std::string request;     // short prompt preview
    int   n_prompt = 0;
    int   n_gen    = 0;
    double pp_s    = 0.0;
    double gen_s   = 0.0;
    double io_s    = 0.0;    // disk read time accumulated in this run
    double cmp_s   = 0.0;    // compute time accumulated in this run
    unsigned long long loads     = 0;
    unsigned long long bytes     = 0;
    unsigned long long evictions = 0;
    int layer   = -1;
    int n_layer = 0;
    int moe     = 0;
    unsigned long long moe_nodes        = 0;
    unsigned long long moe_experts      = 0;
    unsigned long long moe_expert_bytes = 0;
};

// estimate of the RAM the next run will need, filled by engine::probe_memory().
// base_rss/arena/model metadata are measured, kv/recurrent/compute are read from the
// memory sizes llama.cpp reports while a throw-away context with the same parameters
// is created (so they are exact for the current model + settings).
struct mem_estimate {
    bool               ok = false;
    std::string        err;
    unsigned long long base_rss  = 0;   // current process RSS after the model is loaded
    unsigned long long model     = 0;   // model metadata / tokenizer (RSS delta at load)
    unsigned long long arena     = 0;   // weight arena
    unsigned long long kv        = 0;   // attention KV cache (n_ctx dependent)
    unsigned long long recurrent = 0;   // linear/SSM state (context independent)
    unsigned long long compute   = 0;   // graph compute buffer (n_ubatch dependent)
    unsigned long long scratch   = 0;   // batch inputs + logits/output buffers
    unsigned long long resident  = 0;   // base_rss + kv + recurrent
    unsigned long long peak      = 0;   // resident + compute + scratch
};

struct gen_result {
    bool        ok = false;
    std::string error;
    std::string text;
    std::string finish = "stop";   // stop | length | cancelled | error
    int    n_prompt = 0;
    int    n_gen    = 0;
    double pp_s     = 0.0;
    double gen_s    = 0.0;
};

class engine {
public:
    static engine & get();

    // log routing: the TUI funnels this into its scrolling log area, the
    // headless API mode prints it to stderr. thread safe.
    void set_log_sink(std::function<void(const std::string &)> sink);
    void log(const std::string & line);

    // ---- single inference slot ------------------------------------------------
    bool  acquire(owner who, const std::string & label);
    void  release();
    bool  busy() const;
    owner busy_owner() const;
    std::string busy_label() const;

    // ---- cancellation ---------------------------------------------------------
    void request_cancel();
    void clear_cancel();
    bool cancel_requested() const;

    // ---- model ----------------------------------------------------------------
    // arena_mb: 0 = keep whatever the environment / launcher configured (auto),
    // > 0 = force the disk arena size in MiB for this load
    bool load(const std::string & path, bool moe, std::string & err, int arena_mb = 0);
    void unload();
    bool loaded() const;
    bool loaded_with_moe() const;
    const std::string & path() const;
    std::string basename() const;
    std::string desc() const;
    int  n_ctx_train() const;
    int  n_layer() const;
    // requested disk arena size in MiB for the current model, 0 = auto
    int  requested_arena_mb() const;

    // RAM estimate for the next run with these settings (requires a loaded model).
    // kv/recurrent/compute come from llama.cpp's own size report of a temporary context,
    // base_rss/model/arena are measured; the result is cached per parameter set.
    mem_estimate probe_memory(int n_ctx, int n_ubatch);
    unsigned long long model_ram() const;

    // route llama.cpp's log (the TUI installs its callback here so that probe_memory()
    // can capture the memory-size lines)
    using llama_logger_t = void (*)(enum ggml_log_level, const char *, void *);
    void set_llama_logger(llama_logger_t cb, void * ud);

    // false when the GGUF carries no chat template
    bool has_chat_template() const;
    // extra tokens a single turn chat template adds on top of the bare message,
    // -1 when it cannot be measured (no template / tokenize failure)
    int chat_template_extra_tokens();

    // both require the slot to be held (acquire() returned true)
    gen_result generate_chat(const std::vector<chat_message> & msgs, const gen_params & gp);
    gen_result generate_raw(const std::string & prompt, const gen_params & gp);

    live_state snapshot() const;

private:
    engine();
    ~engine();
    engine(const engine &) = delete;
    engine & operator=(const engine &) = delete;

    gen_result generate_impl(const std::string & prompt, const gen_params & gp);

    void live_begin(owner who, const std::string & label, int total);
    void live_end();
    void live_phase(int phase, int done, int total, const std::string & piece);

    mutable std::mutex  m_model_mtx;     // guards the model pointers
    mutable std::mutex  m_live_mtx;      // guards m_live
    mutable std::mutex  m_log_mtx;
    mutable std::mutex  m_slot;          // the single inference slot
    std::atomic<bool>   m_cancel{false};

    // who holds the slot (read by both the TUI painter and the API handlers)
    std::atomic<int>    m_owner{ (int) owner::none };
    mutable std::mutex  m_owner_mtx;     // guards m_owner_label

    llama_model *       m_model = nullptr;
    const llama_vocab * m_vocab = nullptr;
    std::string         m_path;
    int                 m_n_ctx_train = 0;
    std::atomic<int>    m_n_layer{ 0 };
    std::atomic<bool>   m_moe_loaded{ false };
    int                 m_arena_mb = 0;          // requested arena, 0 = auto
    unsigned long long  m_model_ram = 0;         // RSS delta of llama_model_load_from_file()
    unsigned long long  m_base_rss  = 0;         // process RSS right after the model load
    mem_estimate        m_mem_cache;
    int                 m_mem_cache_ctx  = -1;
    int                 m_mem_cache_ub   = -1;
    const void *        m_mem_cache_model = nullptr;
    llama_logger_t      m_llama_logger = nullptr;
    void *              m_llama_logger_ud = nullptr;
    std::string         m_env_stream_orig;       // launcher value to restore for auto
    bool                m_env_stream_orig_set = false;

    live_state m_live;
    int64_t    m_run_t0   = 0;
    double     m_io_acc   = 0.0;
    double     m_cmp_acc  = 0.0;

    std::string m_owner_label;
    std::function<void(const std::string &)> m_log_sink;
};

// helpers used by both front ends
int64_t now_us();
std::string human_bytes(unsigned long long n);
std::string fmt_seconds(double s);

} // namespace diskui
