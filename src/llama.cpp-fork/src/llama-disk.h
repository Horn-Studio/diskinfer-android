#pragma once

// experimental: keep the model weights on disk and stream them into a fixed RAM arena
//
// the model is loaded with no weight allocation at all (llama_model_loader::no_alloc),
// then each graph node faults in the tensors it needs, evicting least recently used ones.
// enable with LLAMA_DISK_STREAM_MB=<arena size in MiB>

#include <cstddef>

struct ggml_tensor;

namespace llama_disk {

bool enabled();

// number of output rows to multiply at a time, 0 disables the chunked output layer
int out_chunk_rows();

// allocate the arena and install the ggml hooks
void init();

// register a weight tensor: it will be read from `fd` at `off` when first needed
void register_tensor(ggml_tensor * t, int fd, size_t off, size_t nbytes);

// validate the arena against the largest tensor and log a summary
void finish_register();


// live counters for the UI, thread safe
struct llama_disk_stats {
    unsigned long long io_us;      // total time spent inside pread, microseconds
    unsigned long long loads;
    unsigned long long bytes;      // bytes streamed from disk
    unsigned long long evictions;
    unsigned long long clock_us;   // diagnostics: monotonic clock now, microseconds
    int                layer;      // last layer index touched
    int                n_layer;    // highest layer index seen
};

void get_stats(struct llama_disk_stats * out);

// route the status messages to a callback instead of stderr (used by the TUI)
void set_msg_cb(void (*cb)(const char *));
// drop everything, called after the model is freed
void cleanup();

}
