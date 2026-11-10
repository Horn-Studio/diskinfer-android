// api.cpp: OpenAI compatible subset on port 4000 (no authentication)
//
// GET  /v1/models
// POST /v1/chat/completions   {"messages":[...], "max_tokens":N, "temperature":T, "stream":false}
// POST /v1/completions        {"prompt":"...",  "max_tokens":N, "temperature":T, "stream":false}
// GET  /health
//
// concurrency: the engine has exactly one inference slot, so a request that
// arrives while another one (or the TUI) is decoding gets 429 immediately.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "api.h"

#include "engine.h"
#include "i18n.h"

#include "httplib.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace diskui {
namespace api {

namespace {

std::string dump_json(const json & j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

void reply_json(httplib::Response & res, int status, const json & j) {
    res.status = status;
    res.set_content(dump_json(j), "application/json");
}

void reply_error(httplib::Response & res, int status, const char * type, const std::string & msg) {
    reply_json(res, status, json{ { "error", json{ { "message", msg }, { "type", type } } } });
}

std::string random_id(const char * prefix) {
    static std::atomic<unsigned> counter{ 0 };
    char buf[80];
    snprintf(buf, sizeof(buf), "%s-%08llx%04x", prefix,
             (unsigned long long) time(nullptr), counter.fetch_add(1) & 0xffffu);
    return buf;
}

const char * finish_reason(const std::string & f) {
    if (f == "length") {
        return "length";
    }
    return "stop";   // stop, eog and cancelled all end a normal answer
}

// the model id the API exposes: the file name (basename) of the loaded model
std::string model_id() {
    engine & eng = engine::get();
    if (!eng.loaded()) {
        return std::string();
    }
    return eng.basename();
}

json models_json(long long created) {
    const std::string id = model_id();
    json data = json::array();
    if (!id.empty()) {
        data.push_back(json{
            { "id", id },
            { "object", "model" },
            { "created", created },
            { "owned_by", "diskinfer" },
        });
    }
    return json{ { "object", "list" }, { "data", data } };
}

bool parse_messages(const json & body, std::vector<chat_message> & out, std::string & err) {
    if (!body.contains("messages") || !body["messages"].is_array() || body["messages"].empty()) {
        err = "requests needs a non-empty 'messages' array";
        return false;
    }
    for (const auto & m : body["messages"]) {
        if (!m.is_object()) {
            err = "'messages' must contain objects";
            return false;
        }
        chat_message cm;
        cm.role = m.value("role", "user");
        if (m.contains("content") && m["content"].is_string()) {
            cm.content = m["content"].get<std::string>();
        } else if (m.contains("content") && m["content"].is_array()) {
            // OpenAI content parts: keep the text parts
            for (const auto & part : m["content"]) {
                if (part.is_object() && part.value("type", std::string()) == "text") {
                    cm.content += part.value("text", std::string());
                }
            }
        } else {
            err = "message 'content' must be a string (or a text-parts array)";
            return false;
        }
        out.push_back(std::move(cm));
    }
    return true;
}

gen_params params_from(const json & body, const options & opt) {
    gen_params gp;
    gp.n_ctx     = opt.n_ctx;
    gp.n_predict = body.value("max_tokens", opt.n_predict);
    if (body.contains("temperature") && body["temperature"].is_number()) {
        gp.temp = body["temperature"].get<float>();
    } else {
        gp.temp = opt.temp;
    }
    gp.top_k        = opt.top_k;
    gp.top_p        = opt.top_p;
    gp.n_threads    = opt.n_threads;
    gp.n_ubatch     = opt.n_ubatch;
    gp.auto_fit_ctx = true;   // the API should not fail just because ctx was set small
    return gp;
}

struct slot_guard {
    ~slot_guard() { engine::get().release(); }
};

} // namespace

// --------------------------------------------------------------------- impl

struct server::impl {
    options            opt;
    httplib::Server *  svr = nullptr;
    std::thread        thread;
    std::atomic<bool>  running{ false };
    std::atomic<bool>  started{ false };
    std::mutex         start_mtx;
    std::condition_variable start_cv;
    long long          created = 0;

    void handle_models(const httplib::Request & req, httplib::Response & res) {
        engine::get().log(i18n::trf("api.log_get_fmt", req.path.c_str(), req.remote_addr.c_str()));
        reply_json(res, 200, models_json(created));
    }

    void handle_health(const httplib::Request &, httplib::Response & res) {
        const std::string id = model_id();
        reply_json(res, 200, json{
            { "status", "ok" },
            { "model", id },
            { "busy", engine::get().busy() },
            { "port", opt.port },
        });
    }

    void handle_chat(const httplib::Request & req, httplib::Response & res) {
        engine & eng = engine::get();
        eng.log(i18n::trf("api.log_post_chat_fmt", req.remote_addr.c_str(), req.body.size()));

        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception & e) {
            reply_error(res, 400, "invalid_request_error", std::string("invalid JSON body: ") + e.what());
            return;
        }
        if (body.value("stream", false)) {
            reply_error(res, 400, "invalid_request_error", "stream:true is not supported, send stream:false");
            return;
        }
        std::vector<chat_message> msgs;
        std::string err;
        if (!parse_messages(body, msgs, err)) {
            reply_error(res, 400, "invalid_request_error", err);
            return;
        }
        if (!eng.loaded()) {
            reply_error(res, 503, "service_unavailable", "no model loaded");
            return;
        }
        if (!eng.acquire(owner::api, "API /v1/chat/completions")) {
            reply_error(res, 429, "rate_limit_error", "busy");
            return;
        }
        slot_guard guard;

        const gen_params gp = params_from(body, opt);
        eng.log(i18n::trf("api.log_chat_req_fmt", msgs.size(), gp.n_predict, gp.temp));
        const gen_result r = eng.generate_chat(msgs, gp);
        if (!r.ok) {
            reply_error(res, 500, "internal_error", r.error);
            return;
        }

        json resp{
            { "id", random_id("chatcmpl") },
            { "object", "chat.completion" },
            { "created", (long long) time(nullptr) },
            { "model", model_id() },
            { "choices", json::array({ json{
                { "index", 0 },
                { "message", json{ { "role", "assistant" }, { "content", r.text } } },
                { "finish_reason", finish_reason(r.finish) },
            } }) },
            { "usage", json{
                { "prompt_tokens", r.n_prompt },
                { "completion_tokens", r.n_gen },
                { "total_tokens", r.n_prompt + r.n_gen },
            } },
        };
        reply_json(res, 200, resp);
    }

    void handle_completion(const httplib::Request & req, httplib::Response & res) {
        engine & eng = engine::get();
        eng.log(i18n::trf("api.log_post_comp_fmt", req.remote_addr.c_str()));

        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception & e) {
            reply_error(res, 400, "invalid_request_error", std::string("invalid JSON body: ") + e.what());
            return;
        }
        if (body.value("stream", false)) {
            reply_error(res, 400, "invalid_request_error", "stream:true is not supported, send stream:false");
            return;
        }
        std::string prompt;
        if (body.contains("prompt") && body["prompt"].is_string()) {
            prompt = body["prompt"].get<std::string>();
        } else if (body.contains("prompt") && body["prompt"].is_array() && !body["prompt"].empty()) {
            for (const auto & p : body["prompt"]) {
                if (p.is_string()) {
                    prompt += p.get<std::string>();
                }
            }
        }
        if (prompt.empty()) {
            reply_error(res, 400, "invalid_request_error", "requests needs a non-empty 'prompt' string");
            return;
        }
        if (!eng.loaded()) {
            reply_error(res, 503, "service_unavailable", "no model loaded");
            return;
        }
        if (!eng.acquire(owner::api, "API /v1/completions")) {
            reply_error(res, 429, "rate_limit_error", "busy");
            return;
        }
        slot_guard guard;

        const gen_params gp = params_from(body, opt);
        eng.log(i18n::trf("api.log_comp_req_fmt", prompt.size(), gp.n_predict));
        const gen_result r = eng.generate_raw(prompt, gp);
        if (!r.ok) {
            reply_error(res, 500, "internal_error", r.error);
            return;
        }

        json resp{
            { "id", random_id("cmpl") },
            { "object", "text_completion" },
            { "created", (long long) time(nullptr) },
            { "model", model_id() },
            { "choices", json::array({ json{
                { "index", 0 },
                { "text", r.text },
                { "finish_reason", finish_reason(r.finish) },
            } }) },
            { "usage", json{
                { "prompt_tokens", r.n_prompt },
                { "completion_tokens", r.n_gen },
                { "total_tokens", r.n_prompt + r.n_gen },
            } },
        };
        reply_json(res, 200, resp);
    }
};

// ------------------------------------------------------------------- server

server::server() : m_impl(new impl()) {}

server::~server() {
    stop();
    delete m_impl;
    m_impl = nullptr;
}

bool server::start(const options & opt, std::string & err) {
    if (m_impl->running.load()) {
        return true;
    }
    impl * m = m_impl;
    m->opt = opt;
    m->created = (long long) time(nullptr);
    m->svr = new httplib::Server();

    m->svr->set_read_timeout(60, 0);
    m->svr->set_write_timeout(600, 0);
    m->svr->set_payload_max_length(64u << 20);
    m->svr->new_task_queue = [] { return new httplib::ThreadPool(4); };

    m->svr->Get("/v1/models", [m](const httplib::Request & req, httplib::Response & res) {
        m->handle_models(req, res);
    });
    m->svr->Get("/health", [m](const httplib::Request & req, httplib::Response & res) {
        m->handle_health(req, res);
    });
    m->svr->Post("/v1/chat/completions", [m](const httplib::Request & req, httplib::Response & res) {
        m->handle_chat(req, res);
    });
    m->svr->Post("/v1/completions", [m](const httplib::Request & req, httplib::Response & res) {
        m->handle_completion(req, res);
    });

    m->svr->set_error_handler([](const httplib::Request & req, httplib::Response & res) {
        if (res.status == 404) {
            reply_error(res, 404, "invalid_request_error", "unknown endpoint: " + req.method + " " + req.path);
        }
    });
    m->svr->set_exception_handler([](const httplib::Request & req, httplib::Response & res, std::exception_ptr ep) {
        std::string what = "unknown error";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            what = e.what();
        } catch (...) {
        }
        engine::get().log(i18n::trf("api.log_exception_fmt", req.path.c_str(), what.c_str()));
        reply_error(res, 500, "internal_error", what);
    });
    m->svr->set_start_handler([m] {
        {
            std::lock_guard<std::mutex> lk(m->start_mtx);
            m->started.store(true);
        }
        m->start_cv.notify_all();
    });

    if (!m->svr->bind_to_port(opt.host, opt.port)) {
        err = i18n::trf("api.bind_failed_fmt", opt.host.c_str(), opt.port);
        delete m->svr;
        m->svr = nullptr;
        return false;
    }

    m->running.store(true);
    m->thread = std::thread([m] {
        m->svr->listen_after_bind();
        m->running.store(false);
    });

    // wait until the accept loop is actually running so the first curl does not
    // race the listener setup
    {
        std::unique_lock<std::mutex> lk(m->start_mtx);
        m->start_cv.wait_for(lk, std::chrono::seconds(5), [m] { return m->started.load(); });
    }

    engine::get().log(i18n::trf("api.log_listening_fmt", opt.host.c_str(), opt.port));
    return true;
}

void server::stop() {
    impl * m = m_impl;
    if (m == nullptr || m->svr == nullptr) {
        return;
    }
    m->svr->stop();
    if (m->thread.joinable()) {
        m->thread.join();
    }
    delete m->svr;
    m->svr = nullptr;
    m->running.store(false);
    m->started.store(false);
}

bool server::running() const {
    return m_impl != nullptr && m_impl->running.load();
}

} // namespace api
} // namespace diskui
