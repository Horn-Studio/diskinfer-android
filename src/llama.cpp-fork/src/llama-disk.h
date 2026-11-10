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

// tiny arena mode: the arena is smaller than the largest weight tensor, so big tensors are read
// in row/view chunks and nothing has to be resident as a whole. auto-enabled when the requested
// arena is smaller than the largest registered tensor (LLAMA_DISK_TINY_ARENA=0 disables)
LLAMA_API bool tiny_arena_mode();

// split every big matmul into row chunks (auto in tiny arena mode, LLAMA_DISK_SPLIT_MM overrides)
LLAMA_API bool split_all_matmul();

// row chunk for one matmul weight: the configured LLAMA_DISK_OUT_CHUNK, or a chunk of roughly
// a quarter of a tiny arena. 0 means "do not chunk". At most 64 chunks per tensor, so the graph
// stays within the tensor-object budget of llama.cpp.
LLAMA_API int matmul_chunk_rows(int64_t ne1, size_t row_bytes);

// a tensor this big does not fit comfortably into the arena, split its matmul (tiny arena only)
LLAMA_API int64_t split_threshold_bytes();

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
    int                tiny;             // 1 when the tiny-arena mode is active (chunked reads)
    int                chunk_rows;       // row chunks used for big matmuls in tiny-arena mode
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
