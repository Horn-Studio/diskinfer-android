// minimal driver for the disk-streaming experiment: no common/ dependencies, prints RSS as it goes

#include "llama.h"
#include "ggml-cpu.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static long rss_kb() {
#if defined(_WIN32)
    return 0;
#else
    FILE * f = fopen("/proc/self/statm", "r");
    if (!f) {
        return -1;
    }
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

static void print_rss(const char * tag) {
    fprintf(stderr, "diskrun: rss %-22s = %ld MiB\n", tag, rss_kb() / 1024);
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::string model_path;
    std::string prompt     = "Hello";
    int n_predict          = 16;
    int n_ctx              = 128;
    int n_batch            = 1;
    int n_ubatch           = 1;
    int n_threads          = 4;
    int ngl                = 0;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](int def) -> int {
            if (i + 1 < argc) {
                return atoi(argv[++i]);
            }
            return def;
        };
        if      (a == "-m")       { if (i + 1 < argc) model_path = argv[++i]; }
        else if (a == "-n")       { n_predict = next(n_predict); }
        else if (a == "-c")       { n_ctx     = next(n_ctx); }
        else if (a == "-b")       { n_batch   = next(n_batch); }
        else if (a == "-ub")      { n_ubatch  = next(n_ubatch); }
        else if (a == "-t")       { n_threads = next(n_threads); }
        else if (a == "-ngl")     { ngl       = next(ngl); }
        else if (a == "-p")       { if (i + 1 < argc) prompt = argv[++i]; }
        else if (a == "--prompt-tokens") {
            // remaining args are token ids
            std::vector<llama_token> ids;
            while (i + 1 < argc) {
                ids.push_back(atoi(argv[++i]));
            }
            prompt.clear();
            for (size_t k = 0; k < ids.size(); ++k) {
                if (k) prompt += ",";
                prompt += std::to_string(ids[k]);
            }
        }
        else {
            // bare argument: prompt words
            if (i == 1 || a[0] != '-') {
                prompt = a;
                for (i++; i < argc; i++) {
                    prompt += " ";
                    prompt += argv[i];
                }
            }
        }
    }

    if (model_path.empty()) {
        fprintf(stderr, "usage: %s -m model.gguf [-n n_predict] [-c n_ctx] [-b n_batch] [-ub n_ubatch] [-t threads] [prompt]\n", argv[0]);
        return 1;
    }

    llama_backend_init();

    print_rss("start");

    fprintf(stderr, "diskrun: cpu features: neon = %d, dotprod = %d, matmul_int8 = %d\n",
            ggml_cpu_has_neon(), ggml_cpu_has_dotprod(), ggml_cpu_has_matmul_int8());

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = ngl;

    const int64_t t_load = ggml_time_us();
    llama_model * model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (model == nullptr) {
        fprintf(stderr, "diskrun: failed to load model\n");
        return 1;
    }
    fprintf(stderr, "diskrun: model loaded in %.1f s\n", (ggml_time_us() - t_load) / 1e6);
    print_rss("after model load");

    const llama_vocab * vocab = llama_model_get_vocab(model);

    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    std::vector<llama_token> tokens(n_prompt);
    if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), tokens.data(), tokens.size(), true, true) < 0) {
        fprintf(stderr, "diskrun: failed to tokenize\n");
        return 1;
    }
    fprintf(stderr, "diskrun: prompt has %d tokens:", n_prompt);
    for (auto id : tokens) {
        fprintf(stderr, " %d", id);
    }
    fprintf(stderr, "\n");

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = n_ctx;
    cparams.n_batch   = n_batch;
    cparams.n_ubatch  = n_ubatch;
    cparams.n_threads = n_threads;
    cparams.n_threads_batch = n_threads;
    cparams.no_perf   = false;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (ctx == nullptr) {
        fprintf(stderr, "diskrun: failed to create context\n");
        return 1;
    }
    print_rss("after context");

    // greedy, we only care that the thing runs
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler * smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    llama_batch batch = llama_batch_init(n_batch, 0, 1);

    auto decode_one = [&](llama_token id, llama_pos pos, bool logits) -> bool {
        batch.n_tokens = 1;
        batch.token[0]  = id;
        batch.pos[0]    = pos;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = logits;
        return llama_decode(ctx, batch) == 0;
    };

    int n_pos = 0;
    const int64_t t_prefill = ggml_time_us();
    for (int i = 0; i < n_prompt; ++i) {
        if (!decode_one(tokens[i], n_pos++, i == n_prompt - 1)) {
            fprintf(stderr, "diskrun: prefill failed at %d\n", i);
            return 1;
        }
        fprintf(stderr, "diskrun: prefill token %d/%d done, rss = %ld MiB\n", i + 1, n_prompt, rss_kb() / 1024);
    }
    fprintf(stderr, "diskrun: prefill took %.1f s\n", (ggml_time_us() - t_prefill) / 1e6);
    print_rss("after prefill");

    printf("%s", prompt.c_str());
    fflush(stdout);

    const int64_t t_gen = ggml_time_us();
    int n_gen = 0;

    for (int i = 0; i < n_predict; ++i) {
        const int64_t t0 = ggml_time_us();
        const llama_token cur = llama_sampler_sample(smpl, ctx, -1);

        if (llama_vocab_is_eog(vocab, cur)) {
            fprintf(stderr, "diskrun: eog after %d tokens\n", n_gen);
            break;
        }

        char buf[256];
        const int n = llama_token_to_piece(vocab, cur, buf, sizeof(buf), 0, true);
        if (n > 0) {
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
        n_gen++;

        // feed the sampled token back, logits are needed for the next sample
        if (!decode_one(cur, n_pos++, true)) {
            fprintf(stderr, "diskrun: decode failed at %d\n", i);
            return 1;
        }

        fprintf(stderr, "\ndiskrun: token %d id=%d sample=%.3f s, rss = %ld MiB\n",
                n_gen, cur, (ggml_time_us() - t0) / 1e6, rss_kb() / 1024);
    }

    printf("\n");
    fprintf(stderr, "diskrun: generated %d tokens in %.1f s (%.2f s/token)\n",
            n_gen, (ggml_time_us() - t_gen) / 1e6, n_gen > 0 ? (ggml_time_us() - t_gen) / 1e6 / n_gen : 0.0);

    llama_perf_context_print(ctx);

    llama_batch_free(batch);
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);

    return 0;
}
