// diskc: streaming chat driver for the disk-streaming build (GUI friendly)
//
// A normal chat assistant on top of the disk-streaming engine: the whole
// conversation is rendered from the streamed text, while every generated token
// also reports where the engine is (which layer is being paged in/computed
// right now), how long the disk read took and how long the compute took.
//
// Protocol: one JSON object per line on stdout, llama.cpp logs stay on stderr.
//
//   request (stdin):
//     {"cmd":"chat","messages":[{"role":"system","content":"..."},
//                               {"role":"user","content":"..."}],
//      "n_predict":128,"temp":0.7,"top_k":40,"top_p":0.95}
//     {"cmd":"reset"}      drop the KV cache / start a new conversation
//     {"cmd":"cancel"}     stop the running generation (kept alive)
//     {"cmd":"quit"}       exit
//
//   events (stdout):
//     {"ev":"ready","model":...,"n_layer":24,"n_ctx":...,
//      "arena_mib":...,"direct_io":1,"mlock":0,"prefetch":0,
//      "tensors":320,"registered_mib":590,"max_tensor":"...","max_tensor_mib":198}
//     {"ev":"progress","phase":"prefill|gen","layer":12,"n_layer":24,
//      "done":3,"total":12,"loads":...,"read_mib":...,"evictions":...,
//      "elapsed_s":1.2}
//     {"ev":"token","n":1,"id":11751,"piece":" Paris","io_s":0.30,"cmp_s":0.02,
//      "tot_s":0.33,"layer":23,"n_layer":24,"tok_s":3.03}
//     {"ev":"done","tokens":8,"io_s":2.6,"cmp_s":0.4,"tot_s":3.0,"tok_s":2.7,
//      "text":" Paris.","stop":"eog|length|ctx|cancelled","n_past":...,"n_ctx":...}
//     {"ev":"error","msg":"..."}
//
// The engine keeps the llama_context alive between requests and reuses the KV
// cache of the common prompt prefix, so later turns do not re-prefill the whole
// conversation.

#include "llama.h"
#include "llama-disk.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

// ------------------------------------------------------------------ output

std::mutex g_out_mtx;

void emit(json j) {
    std::lock_guard<std::mutex> lk(g_out_mtx);
    std::string line = j.dump(-1, ' ', false, json::error_handler_t::replace);
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void emit_error(const std::string & msg) {
    emit(json{{"ev", "error"}, {"msg", msg}});
}

uint64_t now_us() {
    return (uint64_t) ggml_time_us();
}

// ------------------------------------------------------- live progress state

std::atomic<int>      g_phase{0};        // 0 idle, 1 prefill, 2 generate
std::atomic<int>      g_done{0};
std::atomic<int>      g_total{0};
std::atomic<uint64_t> g_turn_t0{0};
std::atomic<bool>     g_cancel{false};
std::atomic<bool>     g_stop_watcher{false};

void watcher_main(int n_layer_model) {
    while (!g_stop_watcher.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (g_stop_watcher.load()) {
            break;
        }
        const int phase = g_phase.load();
        if (phase == 0) {
            continue;
        }

        llama_disk::llama_disk_stats s;
        llama_disk::get_stats(&s);

        const uint64_t t0 = g_turn_t0.load();
        const double   el = t0 ? (now_us() - t0) / 1e6 : 0.0;

        emit(json{
            {"ev", "progress"},
            {"phase", phase == 1 ? "prefill" : "gen"},
            {"layer", s.layer},
            {"n_layer", n_layer_model},
            {"seen_layer", s.n_layer},
            {"done", g_done.load()},
            {"total", g_total.load()},
            {"loads", s.loads},
            {"read_mib", s.bytes >> 20},
            {"evictions", s.evictions},
            {"elapsed_s", el},
        });
    }
}

// ------------------------------------------------------------- request queue

std::mutex              g_q_mtx;
std::condition_variable g_q_cv;
std::deque<json>        g_q;
bool                    g_q_eof = false;

void reader_main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        json j;
        try {
            j = json::parse(line);
        } catch (const std::exception & e) {
            emit_error(std::string("bad json: ") + e.what());
            continue;
        }
        const std::string cmd = j.value("cmd", "");
        if (cmd == "cancel") {
            g_cancel.store(true);
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_q_mtx);
            g_q.push_back(std::move(j));
        }
        g_q_cv.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        g_q_eof = true;
    }
    g_q_cv.notify_one();
}

bool pop_request(json & out) {
    std::unique_lock<std::mutex> lk(g_q_mtx);
    g_q_cv.wait(lk, [] { return !g_q.empty() || g_q_eof; });
    if (g_q.empty()) {
        return false;
    }
    out = std::move(g_q.front());
    g_q.pop_front();
    return true;
}

// ------------------------------------------------------------------- options

struct options {
    std::string model;
    int         n_ctx     = 2048;
    int         n_batch   = 4;
    int         n_ubatch  = 4;
    int         n_threads = 4;
    float       temp      = 0.7f;
    int         top_k     = 40;
    float       top_p     = 0.95f;
    int         n_predict = 256;
};

bool parse_args(int argc, char ** argv, options & o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next_int = [&](int def) -> int {
            if (i + 1 < argc) {
                return atoi(argv[++i]);
            }
            return def;
        };
        auto next_float = [&](float def) -> float {
            if (i + 1 < argc) {
                return (float) atof(argv[++i]);
            }
            return def;
        };
        if (a == "-m") { if (i + 1 < argc) { o.model = argv[++i]; } }
        else if (a == "-c") { o.n_ctx = next_int(o.n_ctx); }
        else if (a == "-b") { o.n_batch = next_int(o.n_batch); }
        else if (a == "-ub") { o.n_ubatch = next_int(o.n_ubatch); }
        else if (a == "-t") { o.n_threads = next_int(o.n_threads); }
        else if (a == "--temp") { o.temp = next_float(o.temp); }
        else if (a == "--top-k") { o.top_k = next_int(o.top_k); }
        else if (a == "--top-p") { o.top_p = next_float(o.top_p); }
        else if (a == "-n") { o.n_predict = next_int(o.n_predict); }
        else {
            fprintf(stderr, "diskc: unknown argument '%s'\n", a.c_str());
            return false;
        }
    }
    return !o.model.empty();
}

std::string join_messages_fallback(const json & messages) {
    std::string out;
    for (const auto & m : messages) {
        const std::string role    = m.value("role", "user");
        const std::string content = m.value("content", "");
        out += role + ": " + content + "\n";
    }
    out += "assistant: ";
    return out;
}

// apply the model's chat template to the whole conversation
std::string apply_chat_template(const llama_model * model, const json & messages,
                                bool no_think, bool * out_think_closed) {
    if (out_think_closed) {
        *out_think_closed = false;
    }
    const char * tmpl = llama_model_chat_template(model, nullptr);

    std::vector<std::string>        store;
    std::vector<llama_chat_message> cmsgs;
    store.reserve(messages.size());
    cmsgs.reserve(messages.size());
    for (const auto & m : messages) {
        store.push_back(m.value("role", "user"));
        store.push_back(m.value("content", ""));
        cmsgs.push_back(llama_chat_message{store[store.size() - 2].c_str(), store[store.size() - 1].c_str()});
    }

    std::string prompt;
    bool templated = false;
    if (tmpl != nullptr && !cmsgs.empty()) {
        const int32_t need = llama_chat_apply_template(tmpl, cmsgs.data(), cmsgs.size(), true, nullptr, 0);
        if (need > 0) {
            std::string buf((size_t) need, '\0');
            const int32_t n = llama_chat_apply_template(tmpl, cmsgs.data(), cmsgs.size(), true,
                                                        buf.data(), (int32_t) buf.size());
            if (n > 0) {
                buf.resize((size_t) n);
                prompt  = buf;
                templated = true;
            }
        }
    }
    if (!templated) {
        prompt = join_messages_fallback(messages);
    }

    // Qwen3-style templates keep the thinking block for the assistant; when the UI
    // asks for plain chat, close it so the model answers directly
    if (no_think && templated && tmpl != nullptr && std::strstr(tmpl, "think") != nullptr &&
        prompt.size() >= 10 && prompt.compare(prompt.size() - 10, 10, "assistant\n") == 0) {
        prompt += "<think>\n\n</think>\n\n";
        if (out_think_closed) {
            *out_think_closed = true;
        }
    }

    return prompt;
}

// tokenize a full prompt
std::vector<llama_token> tokenize_prompt(const llama_vocab * vocab, const std::string & text) {
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
        return "";
    }
    return std::string(buf, (size_t) n);
}

// ---------------------------------------------------------------------- main

int run(options o) {
    std::setlocale(LC_NUMERIC, "C");

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    llama_model * model = llama_model_load_from_file(o.model.c_str(), mparams);
    if (model == nullptr) {
        emit_error("failed to load model: " + o.model);
        return 1;
    }
    const llama_vocab * vocab  = llama_model_get_vocab(model);
    const int           n_layer = (int) llama_model_n_layer(model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = (uint32_t) o.n_ctx;
    cparams.n_batch         = (uint32_t) o.n_batch;
    cparams.n_ubatch        = (uint32_t) o.n_ubatch;
    cparams.n_threads       = (uint32_t) o.n_threads;
    cparams.n_threads_batch = (uint32_t) o.n_threads;
    cparams.no_perf         = false;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (ctx == nullptr) {
        emit_error("failed to create llama_context");
        llama_model_free(model);
        return 1;
    }

    llama_memory_t mem = llama_get_memory(ctx);

    // engine metadata for the UI
    {
        llama_disk::llama_disk_stats s;
        llama_disk::get_stats(&s);
        char desc[512] = {0};
        llama_model_desc(model, desc, sizeof(desc));
        emit(json{
            {"ev", "ready"},
            {"model", desc},
            {"n_layer", n_layer},
            {"n_ctx", o.n_ctx},
            {"n_batch", o.n_batch},
            {"n_ubatch", o.n_ubatch},
            {"threads", o.n_threads},
            {"arena_mib", (unsigned long long) (s.arena_bytes >> 20)},
            {"direct_io", s.direct_io},
            {"mlock", s.mlock},
            {"prefetch", s.prefetch},
            {"tensors", s.tensors},
            {"registered_mib", (unsigned long long) (s.registered_bytes >> 20)},
        });
    }

    std::thread watcher(watcher_main, n_layer);
    std::thread reader(reader_main);

    // KV cache of the prompt part of the previous turn, used for prefix reuse
    std::vector<llama_token> prev_tokens;

    bool quit = false;
    while (!quit) {
        json req;
        if (!pop_request(req)) {
            break;
        }
        const std::string cmd = req.value("cmd", "");

        if (cmd == "quit") {
            break;
        }
        if (cmd == "reset") {
            llama_memory_clear(mem, true);
            prev_tokens.clear();
            emit(json{{"ev", "reset_done"}});
            continue;
        }
        if (cmd != "chat") {
            emit_error("unknown cmd: " + cmd);
            continue;
        }

        if (!req.contains("messages") || !req["messages"].is_array() || req["messages"].empty()) {
            emit_error("chat request needs a non-empty 'messages' array");
            continue;
        }

        const int   n_predict = req.value("n_predict", o.n_predict);
        const float temp      = req.value("temp", o.temp);
        const int   top_k     = req.value("top_k", o.top_k);
        const float top_p     = req.value("top_p", o.top_p);

        const std::string prompt = apply_chat_template(model, req["messages"], req.value("no_think", true), nullptr);
        std::vector<llama_token> toks = tokenize_prompt(vocab, prompt);
        if (toks.empty()) {
            emit_error("failed to tokenize the conversation");
            continue;
        }
        if ((int) toks.size() + n_predict > o.n_ctx) {
            emit_error("context full: prompt " + std::to_string(toks.size()) + " tok + n_predict " +
                       std::to_string(n_predict) + " > n_ctx " + std::to_string(o.n_ctx));
            continue;
        }

        // reuse the KV cache of the shared prompt prefix from the previous turn.
        // hybrid models (attention + recurrent/SSM layers, e.g. qwen35) cannot erase the
        // end of a sequence, so fall back to a full reset and re-prefill in that case.
        size_t common = 0;
        while (common < prev_tokens.size() && common < toks.size() && prev_tokens[common] == toks[common]) {
            common++;
        }
        bool reused = false;
        if (common > 0) {
            reused = llama_memory_seq_rm(mem, 0, (llama_pos) common, -1);
        }
        if (!reused) {
            if (common > 0) {
                llama_memory_clear(mem, true);
            }
            common = 0;
        }
        emit(json{{"ev", "info"}, {"msg", "prefix"},
                  {"common", (unsigned long long) common},
                  {"prev", (unsigned long long) prev_tokens.size()},
                  {"new", (unsigned long long) toks.size()},
                  {"reused", reused}});
        int n_past = (int) common;

        g_cancel.store(false);
        g_phase.store(1);
        g_done.store(n_past);
        g_total.store((int) toks.size());
        g_turn_t0.store(now_us());

        // ---- prefill the new part of the prompt
        bool failed = false;
        llama_batch batch = llama_batch_init((int32_t) std::max(1, o.n_ubatch), 0, 1);
        for (size_t i = (size_t) n_past; i < toks.size(); ) {
            if (g_cancel.load()) {
                failed = true;
                break;
            }
            const int n = (int) std::min<size_t>((size_t) o.n_ubatch, toks.size() - i);
            batch.n_tokens = n;
            for (int j = 0; j < n; ++j) {
                batch.token[j]     = toks[i + (size_t) j];
                batch.pos[j]       = (llama_pos) (i + (size_t) j);
                batch.n_seq_id[j]  = 1;
                batch.seq_id[j][0] = 0;
                batch.logits[j]    = (i + (size_t) j == toks.size() - 1) ? 1 : 0;
            }
            if (llama_decode(ctx, batch) != 0) {
                emit_error("prefill decode failed at token " + std::to_string(i));
                failed = true;
                break;
            }
            i += (size_t) n;
            n_past = (int) i;
            g_done.store(n_past);
        }
        if (failed) {
            llama_batch_free(batch);
            g_phase.store(0);
            prev_tokens.clear();
            llama_memory_clear(mem, true);
            emit(json{{"ev", "done"}, {"tokens", 0}, {"stop", "cancelled"}, {"io_s", 0.0},
                      {"cmp_s", 0.0}, {"tot_s", 0.0}, {"tok_s", 0.0}, {"text", ""}});
            continue;
        }

        // ---- sampler for this request
        llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
        llama_sampler * smpl = llama_sampler_chain_init(sparams);
        if (temp <= 0.0f) {
            llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
        } else {
            llama_sampler_chain_add(smpl, llama_sampler_init_top_k(top_k));
            llama_sampler_chain_add(smpl, llama_sampler_init_top_p(top_p, 1));
            llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        }

        // ---- generation
        g_phase.store(2);
        g_done.store(0);
        g_total.store(n_predict);

        std::string text;
        double io_total = 0.0, cmp_total = 0.0, tot_total = 0.0;
        int    n_gen = 0;
        const char * stop_reason = "length";

        for (int i = 0; i < n_predict; ++i) {
            if (g_cancel.load()) {
                stop_reason = "cancelled";
                break;
            }
            if (n_past + 1 >= o.n_ctx) {
                stop_reason = "ctx";
                break;
            }

            llama_sampler * cur = smpl;
            const llama_token id = llama_sampler_sample(cur, ctx, -1);
            if (llama_vocab_is_eog(vocab, id)) {
                stop_reason = "eog";
                break;
            }

            const std::string piece = token_piece(vocab, id);
            text += piece;

            llama_disk::llama_disk_stats s0, s1;
            llama_disk::get_stats(&s0);
            const uint64_t t_dec0 = now_us();

            batch.n_tokens = 1;
            batch.token[0]     = id;
            batch.pos[0]       = (llama_pos) n_past;
            batch.n_seq_id[0]  = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0]    = 1;
            const int rc = llama_decode(ctx, batch);
            const uint64_t t_dec1 = now_us();

            if (rc != 0) {
                emit_error("decode failed at token " + std::to_string(i));
                stop_reason = "error";
                break;
            }
            n_past++;
            n_gen++;
            g_done.store(n_gen);

            llama_disk::get_stats(&s1);
            const double io_s  = (s1.io_us - s0.io_us) / 1e6;
            const double tot_s = (t_dec1 - t_dec0) / 1e6;
            const double cmp_s = tot_s > io_s ? tot_s - io_s : 0.0;
            io_total  += io_s;
            cmp_total += cmp_s;
            tot_total += tot_s;

            emit(json{
                {"ev", "token"},
                {"n", n_gen},
                {"id", id},
                {"piece", piece},
                {"io_s", io_s},
                {"cmp_s", cmp_s},
                {"tot_s", tot_s},
                {"layer", s1.layer},
                {"n_layer", n_layer},
                {"tok_s", tot_s > 0.0 ? 1.0 / tot_s : 0.0},
            });
        }

        llama_sampler_free(smpl);
        llama_batch_free(batch);

        g_phase.store(0);

        const double el = g_turn_t0.load() ? (now_us() - g_turn_t0.load()) / 1e6 : tot_total;

        emit(json{
            {"ev", "done"},
            {"tokens", n_gen},
            {"io_s", io_total},
            {"cmp_s", cmp_total},
            {"tot_s", el},
            {"tok_s", el > 0.0 ? n_gen / el : 0.0},
            {"text", text},
            {"stop", stop_reason},
            {"n_past", n_past},
            {"n_ctx", o.n_ctx},
        });

        // the prompt tokens of this turn are the prefix of the next turn's prompt
        prev_tokens = toks;

        if (g_cancel.load()) {
            llama_memory_clear(mem, true);
            prev_tokens.clear();
        }
    }

    g_stop_watcher.store(true);
    if (watcher.joinable()) {
        watcher.join();
    }
    if (reader.joinable()) {
        reader.detach();  // blocked on stdin; the process is going away
    }

    llama_free(ctx);
    llama_model_free(model);
    return 0;
}

} // namespace

int main(int argc, char ** argv) {
    options o;
    if (!parse_args(argc, argv, o)) {
        fprintf(stderr, "usage: %s -m model.gguf [-c 2048] [-b 4] [-ub 4] [-t 4] "
                        "[--temp 0.7] [--top-k 40] [--top-p 0.95] [-n 256]\n"
                        "(JSON-lines protocol on stdin/stdout, see header of diskc.cpp)\n", argv[0]);
        return 1;
    }
    return run(o);
}
