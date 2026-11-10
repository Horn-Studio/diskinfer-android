// api: OpenAI compatible subset served on port 4000 from the same process
//
// no authentication, exactly one inference at a time (the engine slot), so a
// second concurrent request gets 429 with {"error":{"message":"busy"}}.
#pragma once

#include <string>

namespace diskui {
namespace api {

struct options {
    std::string host      = "0.0.0.0";
    int         port      = 4000;
    int         n_ctx     = 256;    // context used for API requests
    int         n_predict = 256;    // max_tokens when the request does not set one
    int         n_threads = 4;
    int         n_ubatch  = 4;
    float       temp      = 0.70f;
    int         top_k     = 40;
    float       top_p     = 0.95f;
};

class server {
public:
    server();
    ~server();
    server(const server &) = delete;
    server & operator=(const server &) = delete;

    // bind and start serving in a background thread; false + err when the port
    // cannot be bound
    bool start(const options & opt, std::string & err);
    void stop();
    bool running() const;

private:
    struct impl;
    impl * m_impl = nullptr;
};

} // namespace api
} // namespace diskui
