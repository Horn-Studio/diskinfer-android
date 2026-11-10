#pragma once

// experimental: keep the model weights on disk and stream them into a fixed RAM arena
//
// the model is loaded with no weight allocation at all (llama_model_loader::no_alloc),
// then each graph node faults in the tensors it needs, evicting least recently used ones.
// enable with LLAMA_DISK_STREAM_MB=<arena size in MiB>

#include <cstddef>

// LLAMA_API: on windows a shared llama.dll only exports what is decorated, and the
// disk API is used by the examples too (diskui / diskrun)
#include "llama.h"

struct ggml_tensor;

namespace llama_disk {

LLAMA_API bool enabled();

// MoE fast path: only the experts selected by the router are read into the arena
// (LLAMA_DISK_MOE=1, needs disk streaming; off by default). Ids of -1 are the upstream
// "skip this slot" marker: they are passed through without streaming a slice for them.
LLAMA_API bool moe_enabled();

// number of output rows to multiply at a time, 0 disables the chunked output layer
LLAMA_API int out_chunk_rows();

// allocate the arena and install the ggml hooks
LLAMA_API void init();

// register a weight tensor: it will be read from `fd` at `off` when first needed
LLAMA_API void register_tensor(ggml_tensor * t, int fd, size_t off, size_t nbytes);

// validate the arena against the largest tensor and log a summary
LLAMA_API void finish_register();


// live counters for the UI, thread safe
struct llama_disk_stats {
    unsigned long long io_us;      // total time spent inside pread, microseconds
    unsigned long long loads;
    unsigned long long bytes;      // bytes streamed from disk
    unsigned long long evictions;
    unsigned long long clock_us;   // diagnostics: monotonic clock now, microseconds
    int                layer;      // last layer index touched
    int                n_layer;    // highest layer index seen
    unsigned long long arena_bytes;      // size of the weight arena
    unsigned long long registered_bytes; // bytes of the model held on disk
    int                tensors;          // number of streamed tensors
    int                direct_io;        // 1 when unbuffered reads are in use
    int                mlock;            // 1 when the arena is locked in RAM
    int                prefetch;         // 1 when the prefetch reader is running
    // MoE fast path: only the experts selected by the router are streamed in
    int                moe;              // 1 = the MoE fast path is enabled for this run
    unsigned long long moe_nodes;        // MUL_MAT_ID nodes served from an expert window
    unsigned long long moe_experts;      // expert slices read in (deduplicated, cumulative)
    unsigned long long moe_expert_bytes; // bytes read for those expert slices
};

LLAMA_API void get_stats(struct llama_disk_stats * out);

// route the status messages to a callback instead of stderr (used by the TUI)
LLAMA_API void set_msg_cb(void (*cb)(const char *));
// drop everything, called after the model is freed
LLAMA_API void cleanup();

}
