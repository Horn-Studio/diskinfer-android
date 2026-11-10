#include "llama-disk.h"

#include "ggml.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdarg>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#   define NOMINMAX
#   endif
#   include <windows.h>
#   include <io.h>
#else
#   include <fcntl.h>
#   include <sys/mman.h>
#   include <sys/resource.h>
#   include <sys/stat.h>
#   include <unistd.h>
#endif

namespace llama_disk {

namespace {

constexpr size_t ALIGN = 4096;

// --------------------------------------------------------------- file handles
//
// a `disk_handle` is a handle usable from any thread at any time: every read is a
// positioned read (pread / ReadFile with an OVERLAPPED offset), so no thread can
// disturb another one through a shared file pointer.

#if defined(_WIN32)

typedef HANDLE disk_handle;

static const disk_handle INVALID_DISK_HANDLE = INVALID_HANDLE_VALUE;

static inline bool handle_ok(disk_handle h) {
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static int64_t read_at(disk_handle h, uint8_t * dst, size_t n, size_t off) {
    if (!handle_ok(h)) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }

    // win32 transfers are capped at 1 GiB, the callers already loop over short reads
    if (n > (1u << 30)) {
        n = 1u << 30;
    }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD) ( off        & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD) ((off >> 32) & 0xFFFFFFFFull);

    DWORD got = 0;
    if (!ReadFile(h, dst, (DWORD) n, &got, &ov)) {
        const DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            return 0;
        }
        if (err != ERROR_IO_PENDING) {
            return -1;
        }
        if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
            return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
        }
    }

    return (int64_t) got;
}

static void close_disk_handle(disk_handle h) {
    if (handle_ok(h)) {
        CloseHandle(h);
    }
}

// open an independent, positioned-read handle for the file behind a llama.cpp CRT fd
static disk_handle open_read_handle(int fd) {
    if (fd < 0) {
        return INVALID_DISK_HANDLE;
    }

    HANDLE os = (HANDLE) _get_osfhandle(fd);
    if (os == INVALID_HANDLE_VALUE) {
        return INVALID_DISK_HANDLE;
    }

    wchar_t path[32768];
    const DWORD n = GetFinalPathNameByHandleW(os, path, (DWORD) (sizeof(path) / sizeof(path[0])),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n > 0 && n < (DWORD) (sizeof(path) / sizeof(path[0]))) {
        HANDLE h = CreateFileW(path, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
    }

    // no usable path: a duplicate handle reads the same file, the positioned reads keep
    // the shared file pointer out of the picture
    HANDLE dup = INVALID_HANDLE_VALUE;
    if (DuplicateHandle(GetCurrentProcess(), os, GetCurrentProcess(), &dup, GENERIC_READ, FALSE, 0)) {
        return dup;
    }

    return INVALID_DISK_HANDLE;
}

static size_t file_size_of(int fd) {
    if (fd < 0) {
        return 0;
    }
    HANDLE os = (HANDLE) _get_osfhandle(fd);
    if (os == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(os, &li) || li.QuadPart < 0) {
        return 0;
    }
    return (size_t) li.QuadPart;
}

#else

typedef int disk_handle;

static const disk_handle INVALID_DISK_HANDLE = -1;

static inline bool handle_ok(disk_handle h) {
    return h >= 0;
}

static int64_t read_at(disk_handle h, uint8_t * dst, size_t n, size_t off) {
    return (int64_t) pread(h, dst, n, (off_t) (off));
}

static void close_disk_handle(disk_handle h) {
    if (h >= 0) {
        close(h);
    }
}

static disk_handle open_read_handle(int fd) {
    return dup(fd);
}

static size_t file_size_of(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return 0;
    }
    return (size_t) st.st_size;
}

#endif

enum : int {
    ST_ABSENT = 0,
    ST_QUEUED = 1,   // handed to the reader
    ST_LOADING = 2,  // a thread is inside pread for it
    ST_RESIDENT = 3,
};

struct disk_tensor {
    disk_handle fd   = INVALID_DISK_HANDLE; // direct io handle when available
    disk_handle fd_buf = INVALID_DISK_HANDLE; // plain handle, used when the transfer cannot be aligned
    size_t   off    = 0;
    size_t   size   = 0;
    size_t   fsize  = 0;
    size_t   ord    = 0;  // index in the file-order list, used to drive the prefetch
    int      layer  = -1; // parsed from lk.N. for the UI
    int      state  = ST_ABSENT;
    int64_t  slot   = -1;
};

struct arena_slot {
    size_t   off   = 0;   // offset in the arena, always ALIGN aligned
    size_t   size  = 0;   // reserved size, includes the alignment envelope
    ggml_tensor * owner = nullptr;
    int      pins  = 0;
    uint64_t tick  = 0;
};

struct fd_info {
    disk_handle fd     = INVALID_DISK_HANDLE;
    disk_handle fd_buf = INVALID_DISK_HANDLE;
    size_t fsize  = 0;
};

// MoE fast path: a MUL_MAT_ID weight tensor is streamed as one compact block holding only
// the expert slices selected by the router, next to a copy of the ids renumbered to the
// compact slots. The block lives in the arena and is freed when the last thread is done.
enum : int {
    MOE_WIN_LOADING = 0, // the elected thread is reading the slices
    MOE_WIN_READY   = 1, // published, src[0]/src[2] point into it
};

struct moe_window {
    ggml_tensor *         w        = nullptr;
    ggml_tensor *         ids      = nullptr;
    int64_t               slot     = -1;
    int                   state    = MOE_WIN_LOADING;
    int64_t               ne2      = 0;       // saved ne[2] (number of experts)
    uint8_t *             w_data   = nullptr; // saved data pointers
    uint8_t *             ids_data = nullptr;
};

static uint8_t g_dummy[64];

struct state {
    bool     on       = false;
    bool     fail     = false;
    bool     odirect  = false;
    bool     prefetch = true;
    bool     window = false;   // experimental: row/view windows
    bool     window_rows  = true;  // GET_ROWS row windows
    bool     window_dbg   = false; // LLAMA_DISK_WINDOW_DEBUG=1
    bool     window_views = true;  // view windows
    bool     verbose = false;  // periodic counter dumps
    bool     moe     = false;  // experimental: only the selected experts are streamed
    size_t   align_io = ALIGN;
    size_t   lookahead = 16;
    size_t   arena_size = 0;
    size_t   arena_req  = 0;      // 0 = size it automatically
    bool     mlock     = false;
    uint8_t * arena = nullptr;
    void   * placeholder = nullptr;

    std::map<size_t, size_t> free_blk;

    std::vector<arena_slot> slots;
    std::unordered_map<ggml_tensor *, disk_tensor> tensors;
    std::unordered_map<ggml_tensor *, int64_t> windows; // transient residency of a view of a disk tensor
    std::unordered_map<ggml_tensor *, uint8_t *> window_saved; // data pointer the window overwrote
    // windows whose pins dropped to zero: freed at the start of the next node, after every
    // compute thread has passed the node barrier (some ops are executed by thread 0 only, so
    // the last unpin can happen while the other threads are still waiting for the window)
    std::vector<ggml_tensor *> windows_defer;
    struct window_restore { ggml_tensor * ids = nullptr; std::vector<int32_t> orig; };
    std::unordered_map<ggml_tensor *, window_restore> window_restore_map; // compact rows: ids to put back
    std::unordered_map<ggml_tensor *, moe_window> moe_windows; // compact expert blocks, one per weight tensor
    std::vector<ggml_tensor *> order; // tensors sorted by file offset, drives the prefetch

    std::vector<fd_info> fds;
    std::unordered_map<int, int> fd_map;

    std::mutex              mtx;
    std::condition_variable cv_done;   // a load finished
    std::condition_variable cv_work;   // work for the reader

    std::deque<ggml_tensor *> queue;
    std::thread reader;        // prefetch thread, joined by cleanup()
    std::thread watchdog;      // diagnostics thread, joined by cleanup()
    bool     stop = false;
    bool     reader_running = false;
    bool     drain = false;   // a big tensor is being allocated, the reader must stay out
    int      n_loading = 0;   // loads currently copying into the arena
    size_t   drain_threshold = 48u << 20;

    uint64_t tick      = 0;
    uint64_t n_load    = 0;
    uint64_t n_pf_load = 0;
    uint64_t n_pf_hit  = 0;
    uint64_t n_pf_drop = 0;
    uint64_t n_evict   = 0;
    uint64_t n_cb      = 0;
    uint64_t t_read    = 0;
    uint64_t n_wait    = 0;
    uint64_t bytes_registered = 0;
    uint64_t bytes_loaded     = 0;
    uint64_t bytes_evicted    = 0;

    // MoE fast path counters
    uint64_t n_moe_nodes    = 0; // MUL_MAT_ID nodes served from a compact expert block
    uint64_t n_moe_experts  = 0; // expert slices read in, deduplicated per node
    uint64_t n_moe_bytes    = 0; // bytes read for those slices
    uint64_t n_moe_fallback = 0; // nodes that had to use the whole-tensor load

    size_t max_tensor = 0;
    size_t max_row_bytes = 0;   // widest row among the registered tensors (chunk sizing)
    int    auto_chunk_rows = 0; // row chunk picked to fit ~1/5 of a tiny arena
    bool   tiny = false;        // arena smaller than the largest tensor: read big tensors in chunks
    int    cur_layer = -1;
    int    n_layer   = -1;
    std::string name_max;

    // diagnostics
    bool     debug = false;
    const char * wait_name[8] = { nullptr };
    int      wait_what[8] = { 0 };
};

state S;

// optional sink for status messages, so a UI can draw them instead of letting them hit the tty
void (*g_msg_cb)(const char *) = nullptr;

// slot index of the calling thread for diagnostics
thread_local int tls_slot = -1;
thread_local bool tls_slot_set = false;

// MoE fast path: the compact expert window this compute thread pinned for the node it is
// about to run (the node callbacks are strictly nested per thread: false ... true)
thread_local ggml_tensor * tls_moe_w    = nullptr;
thread_local bool          tls_moe_used = false;

// GET_ROWS rows window: the tensor this compute thread pinned through ensure_rows_window()
// (that window is pinned exactly once here, so the generic src loop must not pin it again)
thread_local ggml_tensor * tls_rows_w   = nullptr;
thread_local ggml_tensor * tls_cur_node = nullptr; // node being prepared by this thread
thread_local std::vector<int32_t> tls_moe_ids;   // ids as read from the router
thread_local std::vector<int32_t> tls_moe_sel;   // deduplicated expert ids
thread_local std::vector<int32_t> tls_moe_remap; // original id -> compact slot

void diag_set(const char * name, int what) {
    if (!tls_slot_set) {
        static std::atomic<int> next { 0 };
        tls_slot = next.fetch_add(1) & 7;
        tls_slot_set = true;
    }
    S.wait_name[tls_slot] = name;
    S.wait_what[tls_slot] = what;
}

void diag_clear() {
    if (tls_slot >= 0) {
        S.wait_name[tls_slot] = nullptr;
        S.wait_what[tls_slot] = 0;
    }
}

void dmsg(const char * fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_msg_cb != nullptr) {
        g_msg_cb(buf);
    } else {
        fputs(buf, stderr);
        fputc('\n', stderr);
    }
}

uint64_t now_us() {
#if defined(_WIN32)
    // the multimedia timer has a 1 ms granularity, qpc is the only clock with the
    // resolution the per tensor io timings need
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (uint64_t) (f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t t = (uint64_t) c.QuadPart;
    return (t / freq) * 1000000ull + ((t % freq) * 1000000ull) / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
#endif
}

size_t align_up(size_t n, size_t a = ALIGN) {
    return (n + a - 1) & ~(a - 1);
}

size_t free_bytes() {
    size_t tot = 0;
    for (const auto & kv : S.free_blk) {
        tot += kv.second;
    }
    return tot;
}

size_t max_free_block() {
    size_t best = 0;
    for (const auto & kv : S.free_blk) {
        best = std::max(best, kv.second);
    }
    return best;
}

size_t pinned_bytes() {
    size_t tot = 0;
    for (const auto & sl : S.slots) {
        if (sl.owner != nullptr && sl.pins > 0) {
            tot += sl.size;
        }
    }
    return tot;
}

void dump_slots() {
    dmsg("llama-disk: arena dump (free=%zu, slots=%zu, windows=%zu)\n",
            free_bytes(), S.slots.size(), S.windows.size());
    for (const auto & sl : S.slots) {
        if (sl.owner == nullptr) {
            continue;
        }
        dmsg("llama-disk:   [%7zu + %7zu] pins=%d %s%s\n",
                sl.off >> 10, sl.size >> 10, sl.pins,
                sl.owner->name, sl.owner->view_src ? " (view)" : "");
    }
}

int n_pinned() {
    int n = 0;
    for (const auto & sl : S.slots) {
        if (sl.owner != nullptr && sl.pins > 0) {
            n++;
        }
    }
    return n;
}

void watchdog_main() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard<std::mutex> lk(S.mtx);
        if (S.stop) {
            return;
        }
        dmsg("llama-disk[wd]: cb=%llu load=%llu pf=%llu drop=%llu q=%zu loading=%d drain=%d "
                        "free=%zu maxblk=%zu pinned=%d pinned_mb=%zu wins=%zu slots=%zu waits=%llu\n",
                (unsigned long long) S.n_cb, (unsigned long long) S.n_load,
                (unsigned long long) S.n_pf_load, (unsigned long long) S.n_pf_drop,
                S.queue.size(), S.n_loading, (int) S.drain,
                free_bytes() >> 20, max_free_block() >> 20, n_pinned(), pinned_bytes() >> 20,
                S.windows.size(), S.slots.size(),
                (unsigned long long) S.n_wait);
        for (int i = 0; i < 8; ++i) {
            if (S.wait_name[i] != nullptr) {
                dmsg("llama-disk[wd]:   thread %d waiting on '%s' (%d)\n", i, S.wait_name[i], S.wait_what[i]);
            }
        }
        dump_slots();
        for (auto & w : S.windows) {
            dmsg("llama-disk[wd]:   window slot=%lld owner=%s\n",
                    (long long) w.second, w.first->name);
        }
    }
}

void free_range(size_t off, size_t len) {
    if (len == 0) {
        return;
    }

    auto it = S.free_blk.lower_bound(off);
    if (it != S.free_blk.end() && it->first == off + len) {
        len += it->second;
        S.free_blk.erase(it);
    }

    auto it2 = S.free_blk.lower_bound(off);
    if (it2 != S.free_blk.begin()) {
        auto prev = std::prev(it2);
        if (prev->first + prev->second == off) {
            off  = prev->first;
            len += prev->second;
            S.free_blk.erase(prev);
        }
    }

    S.free_blk.emplace(off, len);
}

bool take_range(size_t len, size_t * out_off) {
    for (auto it = S.free_blk.begin(); it != S.free_blk.end(); ++it) {
        if (it->second < len) {
            continue;
        }

        const size_t start = it->first;
        const size_t rest  = it->second - len;

        S.free_blk.erase(it);
        if (rest > 0) {
            S.free_blk.emplace(start + len, rest);
        }

        *out_off = start;
        return true;
    }

    return false;
}

bool evict_one() {
    int64_t  best      = -1;
    uint64_t best_tick = UINT64_MAX;

    for (size_t i = 0; i < S.slots.size(); ++i) {
        const arena_slot & sl = S.slots[i];
        if (sl.owner == nullptr || sl.pins > 0) {
            continue;
        }
        if (sl.tick < best_tick) {
            best_tick = sl.tick;
            best      = (int64_t) i;
        }
    }

    if (best < 0) {
        return false;
    }

    arena_slot & sl = S.slots[best];

    auto it = S.tensors.find(sl.owner);
    if (it != S.tensors.end()) {
        it->second.state = ST_ABSENT;
        it->second.slot  = -1;
        sl.owner->data   = S.placeholder;
    }

    S.bytes_evicted += sl.size;
    S.n_evict++;

    free_range(sl.off, sl.size);
    sl.owner = nullptr;

    return true;
}

bool read_exact(disk_handle fd, uint8_t * dst, size_t n, size_t off) {
    size_t done = 0;
    while (done < n) {
        const int64_t r = read_at(fd, dst + done, n - done, off + done);
        if (r <= 0) {
            dmsg("llama-disk: read failed at offset %zu (%zu of %zu bytes, ret = %lld)\n",
                    off + done, done, n, (long long) r);
            return false;
        }
        done += (size_t) r;
    }
    return true;
}

// true when the transfer cannot be expressed as an aligned direct io request
bool needs_buffered(size_t off, size_t end) {
    return (off % S.align_io) != 0 || (end <= off) || ((end - off) % S.align_io) != 0;
}

// geometry of the copy for one tensor, no state involved
struct copy_geom {
    size_t delta    = 0;
    size_t read_off = 0;
    size_t read_len = 0;
    size_t need     = 0;
    disk_handle fd  = INVALID_DISK_HANDLE;
};

copy_geom geom_for(const disk_tensor & dt) {
    copy_geom g;

    g.delta    = S.odirect ? (dt.off & (S.align_io - 1)) : 0;
    g.read_off = S.odirect ? (dt.off - g.delta)            : dt.off;
    size_t read_end = S.odirect ? align_up(dt.off + dt.size, S.align_io) : (dt.off + dt.size);
    // unbuffered reads are only valid on the direct handle, everything else uses the plain one
    g.fd       = S.odirect ? dt.fd : dt.fd_buf;

    if (S.odirect) {
        // the last tensor of the file ends at EOF, the envelope must not cross it
        if (dt.fsize > 0 && read_end > dt.fsize) {
            read_end = dt.fsize;
        }
        if (needs_buffered(g.read_off, read_end)) {
            g.fd       = dt.fd_buf;
            g.delta    = 0;
            g.read_off = dt.off;
            read_end   = dt.off + dt.size;
        }
    }

    g.read_len = read_end - g.read_off;
    g.need     = align_up(g.delta + g.read_len);

    return g;
}

// reserve a slot and copy the tensor into it, called with the lock held, releases it during the read
bool load_into(std::unique_lock<std::mutex> & lk, ggml_tensor * t, disk_tensor & dt, bool allow_wait, bool is_prefetch, int64_t * out_slot = nullptr) {
    // claim the tensor first: a second thread that asks for it must wait for this copy instead of
    // starting another one, there is no room for two copies of a big tensor anyway
    dt.state = ST_LOADING;

    const copy_geom g = geom_for(dt);

    if (g.need > S.arena_size) {
        dmsg("llama-disk: tensor '%s' needs %zu MiB but the arena is only %zu MiB, "
                        "restart with a bigger LLAMA_DISK_STREAM_MB (node %s op %s)\n",
                ggml_get_name(t), g.need >> 20, S.arena_size >> 20,
                tls_cur_node ? ggml_get_name(tls_cur_node) : "?",
                tls_cur_node ? ggml_op_name(tls_cur_node->op) : "?");
        S.fail    = true;
        dt.state  = ST_ABSENT;
        S.cv_done.notify_all();
        return false;
    }

    size_t off = 0;
    int n_stuck = 0;

    // a big tensor needs one contiguous block: stop the prefetcher and let the loads in
    // flight finish, otherwise their pinned slots can split the arena in two halves
    const bool want_drain = g.need >= S.drain_threshold;
    if (want_drain) {
        S.drain = true;
        while (S.n_loading > 0) {
            S.n_wait++;
            S.cv_done.wait(lk);
        }
    }

    while (!take_range(g.need, &off)) {
        if (evict_one()) {
            continue;
        }
        if (!allow_wait) {
            dt.state = ST_ABSENT;
            if (want_drain) {
                S.drain = false;
            }
            S.cv_done.notify_all();
            return false;
        }
        // nothing evictable: the other threads are still using the arena, wait for them
        S.n_wait++;
        if (++n_stuck > 200) {
            dmsg("llama-disk: out of arena for '%s' (%zu KiB needed), everything is pinned\n",
                    ggml_get_name(t), g.need >> 10);
            dump_slots();
            S.fail = true;
            dt.state = ST_ABSENT;
            S.cv_done.notify_all();
            return false;
        }
        S.cv_done.wait(lk);
    }

    if (want_drain) {
        S.drain = false;
    }

    if (S.debug) {
        static int dbg_n = 0;
        if (dbg_n < 6) {
            dmsg("geom %s: off=%zu size=%zu roff=%zu rlen=%zu fsize=%zu dx=%zu odir=%d al=%zu",
                 ggml_get_name(t), dt.off, dt.size, g.read_off, g.read_len, dt.fsize, g.delta,
                 (int) S.odirect, S.align_io);
            dbg_n++;
        }
    }

    int64_t idx = -1;
    for (size_t i = 0; i < S.slots.size(); ++i) {
        if (S.slots[i].owner == nullptr) {
            idx = (int64_t) i;
            break;
        }
    }
    if (idx < 0) {
        S.slots.emplace_back();
        idx = (int64_t) S.slots.size() - 1;
    }

    {
        arena_slot & sl = S.slots[idx];
        sl.off   = off;
        sl.size  = g.need;
        sl.owner = t;
        sl.pins  = 1; // pin while loading so that nobody evicts it
        sl.tick  = ++S.tick;
    }

    S.n_loading++;

    uint8_t * dst = S.arena + off;

    // the copy runs without the lock, the slot is protected by the loading pin
    lk.unlock();
    const uint64_t t0 = now_us();
    const bool ok = read_exact(g.fd, dst, g.read_len, g.read_off);
    const uint64_t dt_us = now_us() - t0;
    lk.lock();

    // the slot vector may have grown while the lock was released, never keep a reference across it
    arena_slot & sl = S.slots[idx];

    S.t_read += dt_us;
    S.n_loading--;

    if (S.debug) {
        static int dbg_t = 0;
        if (dbg_t < 6) {
            dmsg("copy %s: len=%zu dt_us=%llu t_read_now=%llu", ggml_get_name(t), g.read_len,
                 (unsigned long long) dt_us, (unsigned long long) S.t_read);
            dbg_t++;
        }
    }

    if (!ok) {
        S.fail   = true;
        sl.owner = nullptr;
        sl.pins  = 0;
        dt.state = ST_ABSENT;
        free_range(off, g.need);
        S.cv_done.notify_all();
        return false;
    }

    dt.slot  = idx;
    dt.state = ST_RESIDENT;
    if (out_slot != nullptr) {
        *out_slot = idx;
    }
    t->data  = dst + g.delta;
    sl.pins  = 0;

    if (dt.layer >= 0) {
        S.cur_layer = dt.layer;
        S.n_layer   = std::max(S.n_layer, dt.layer);
    }

    S.n_load++;
    if (is_prefetch) {
        S.n_pf_load++;
    }
    S.bytes_loaded += dt.size;

    S.cv_done.notify_all();

    return true;
}

// make sure the tensor is in the arena and pin it, requires the lock
void ensure_locked(std::unique_lock<std::mutex> & lk, ggml_tensor * t) {
    auto it = S.tensors.find(t);
    if (it == S.tensors.end() || S.fail) {
        return;
    }

    disk_tensor & dt = it->second;
    if (S.window_dbg && dt.state != ST_RESIDENT) {
        fprintf(stderr, "llama-disk[win]: full load %s off=%zu size=%zu MiB state=%d\n",
                ggml_get_name(t), dt.off, dt.size >> 20, (int) dt.state);
    }

    if (dt.state != ST_RESIDENT) {
        if (dt.state == ST_LOADING) {
            // somebody (probably the reader) is copying it right now
            diag_set(ggml_get_name(t), 1);
            while (dt.state == ST_LOADING) {
                S.n_wait++;
                S.cv_done.wait(lk);
            }
            diag_clear();
        }

        if (dt.state == ST_QUEUED) {
            // take it back from the reader, it will skip whatever is not queued anymore
            dt.state = ST_ABSENT;
        }

        if (dt.state == ST_ABSENT) {
            diag_set(ggml_get_name(t), 2);
            if (!load_into(lk, t, dt, true, false)) {
                diag_clear();
                return;
            }
            diag_clear();
        }
    }

    arena_slot & sl = S.slots[dt.slot];
    sl.pins++;
    sl.tick = ++S.tick;
}

void unpin(ggml_tensor * t) {
    auto it = S.tensors.find(t);
    if (it == S.tensors.end() || it->second.slot < 0) {
        return;
    }
    arena_slot & sl = S.slots[it->second.slot];
    if (sl.pins > 0) {
        sl.pins--;
    }
}

// prefill GET_ROWS: the ids of a whole ubatch are scattered over the vocab, so the span trick
// would read almost the whole table. gather only the unique rows into a compact block, remap the
// ids in place (restored when the window is finally released) and point the weight at the block.
bool ensure_rows_compact(std::unique_lock<std::mutex> & lk, ggml_tensor * node) {
    ggml_tensor * w  = node->src[0];
    ggml_tensor * ix = node->src[1];
    auto it = S.tensors.find(w);
    if (it == S.tensors.end() || ix == nullptr || ix->data == nullptr || ix->type != GGML_TYPE_I32) {
        return false;
    }
    if (S.windows.count(w) > 0) {
        return false;
    }

    const int64_t n = ggml_nelements(ix);
    const size_t  row_bytes = w->nb[1];
    if (n < 1 || n > 512) {
        return false;
    }

    const int32_t * ids = (const int32_t *) ix->data;
    std::vector<int32_t> uniq;
    uniq.reserve((size_t) n);
    std::vector<int32_t> remap((size_t) n, -1);
    for (int64_t i = 0; i < n; ++i) {
        if (ids[i] < 0 || ids[i] >= w->ne[1]) {
            return false;
        }
        int32_t slot = -1;
        for (size_t j = 0; j < uniq.size(); ++j) {
            if (uniq[j] == ids[i]) {
                slot = (int32_t) j;
                break;
            }
        }
        if (slot < 0) {
            slot = (int32_t) uniq.size();
            uniq.push_back(ids[i]);
        }
        remap[(size_t) i] = slot;
    }

    const size_t total = uniq.size() * row_bytes;
    if (total > S.arena_size / 2) {
        return false;
    }

    const disk_tensor & wdt = it->second;

    // claim + allocate a compact block (same policy as load_into: evict or wait for space)
    S.windows[w] = -2;
    size_t off = 0;
    int    n_stuck = 0;
    while (!take_range(total, &off)) {
        if (evict_one()) {
            continue;
        }
        S.n_wait++;
        if (++n_stuck > 200) {
            S.windows.erase(w);
            S.cv_done.notify_all();
            return false;
        }
        S.cv_done.wait(lk);
    }

    int64_t idx = -1;
    for (size_t i = 0; i < S.slots.size(); ++i) {
        if (S.slots[i].owner == nullptr) {
            idx = (int64_t) i;
            break;
        }
    }
    if (idx < 0) {
        S.slots.emplace_back();
        idx = (int64_t) S.slots.size() - 1;
    }
    {
        arena_slot & sl = S.slots[idx];
        sl.off   = off;
        sl.size  = total;
        sl.owner = w;
        sl.pins  = 1;
        sl.tick  = ++S.tick;
    }

    S.window_saved[w] = (uint8_t *) w->data;

    // read the unique rows into the compact block (buffered reads: rows are scattered)
    for (size_t j = 0; j < uniq.size(); ++j) {
        if (!read_exact(wdt.fd_buf, S.arena + off + j * row_bytes, row_bytes,
                        wdt.off + (size_t) uniq[j] * row_bytes)) {
            free_range(off, total);
            S.slots[idx].owner = nullptr;
            S.window_saved.erase(w);
            S.windows.erase(w);
            S.cv_done.notify_all();
            return false;
        }
    }

    // remap the ids and point the weight at the compact block (row j == compact index j)
    std::vector<int32_t> orig((size_t) n);
    memcpy(orig.data(), ids, (size_t) n * sizeof(int32_t));
    int32_t * ids_mut = (int32_t *) ix->data;
    for (int64_t i = 0; i < n; ++i) {
        ids_mut[i] = remap[(size_t) i];
    }
    state::window_restore rest;
    rest.ids  = ix;
    rest.orig = std::move(orig);
    S.window_restore_map[w] = std::move(rest);

    w->data = S.arena + off;
    S.windows[w] = idx;
    S.slots[idx].pins = 1;
    S.cv_done.notify_all();
    return true;
}

// a GET_ROWS node only reads a handful of rows out of a huge embedding table, load just those
bool ensure_rows_window(std::unique_lock<std::mutex> & lk, ggml_tensor * node) {
    ggml_tensor * w  = node->src[0];
    ggml_tensor * ix = node->src[1];

    if (w == nullptr || ix == nullptr || ix->data == nullptr || ix->type != GGML_TYPE_I32) {
        if (S.window_dbg) fprintf(stderr, "llama-disk[win]: rows skip: bad tensor/ids (%s)\n", ggml_get_name(w ? w : node->src[0]));
        return false;
    }
    if (w->ne[1] <= 1 || S.windows.count(w) > 0) {
        if (S.window_dbg) fprintf(stderr, "llama-disk[win]: rows skip: window_exists=%d ne1=%lld\n",
                                   (int) S.windows.count(w), (long long) w->ne[1]);
        return false;
    }

    auto it = S.tensors.find(w);
    if (it == S.tensors.end()) {
        return false;
    }

    const int64_t n = ggml_nelements(ix);
    if (n < 1) {
        return false;
    }
    if (n > 8) {
        return ensure_rows_compact(lk, node);
    }

    const size_t row_bytes = w->nb[1];
    int64_t rmin = INT64_MAX;
    int64_t rmax = -1;

    const int32_t * ids = (const int32_t *) ix->data;
    for (int64_t i = 0; i < n; ++i) {
        if (ids[i] < 0 || ids[i] >= w->ne[1]) {
            return false;
        }
        rmin = std::min(rmin, (int64_t) ids[i]);
        rmax = std::max(rmax, (int64_t) ids[i]);
    }

    const size_t span = (size_t) (rmax - rmin + 1) * row_bytes;
    if (span > S.arena_size / 2) {
        if (S.window_dbg) fprintf(stderr, "llama-disk[win]: rows span %zu > arena/2, using compact gather\n", span);
        return ensure_rows_compact(lk, node);
    }

    const disk_tensor & wdt = it->second;

    // claim the window before load_into() releases the lock for the pread: another compute
    // thread that asks for the same tensor while it is being installed waits below
    S.windows[w] = -2;

    disk_tensor dt;
    dt.fd     = wdt.fd_buf; // a few rows, buffered is fine
    dt.fd_buf = wdt.fd_buf;
    dt.off    = wdt.off + (size_t) rmin * row_bytes;
    dt.size   = span;
    dt.fsize  = wdt.fsize;

    uint8_t * saved = (uint8_t *) w->data;

    int64_t slot = -1;
    if (!load_into(lk, w, dt, true, false, &slot)) {
        S.windows.erase(w);
        S.cv_done.notify_all();
        return false;
    }

    // remember the pointer we are about to overwrite: the parent may be fully resident as well
    S.window_saved[w] = saved;

    // rows of the table are contiguous, so the window can be shifted under the whole tensor.
    // load_into() already put w->data at the first byte of the requested range (it includes the
    // O_DIRECT alignment delta), so only step back by the rows that precede the first one.
    uint8_t * base = (uint8_t *) w->data;
    w->data = base - (size_t) rmin * row_bytes;

    S.windows[w] = slot;
    S.slots[slot].pins = 1; // pinned for this node, other threads pin when they find the window
    S.cv_done.notify_all();

    if (S.window_dbg) {
        fprintf(stderr, "llama-disk[win]: rows window %s ids=%lld..%lld span=%zu slot=%lld\n",
                ggml_get_name(w), (long long) rmin, (long long) rmax, span, (long long) slot);
    }

    return true;
}

// a view of a disk tensor is loaded as a transient window covering only the bytes the view needs
bool ensure_window(std::unique_lock<std::mutex> & lk, ggml_tensor * t, ggml_tensor * parent) {
    const auto pit = S.tensors.find(parent);
    if (pit == S.tensors.end()) {
        return false;
    }

    const disk_tensor & pdt = pit->second;
    const size_t off  = pdt.off + t->view_offs;
    const size_t size = ggml_nbytes(t);

    if (t->view_offs + size > pdt.size) {
        return false;
    }

    auto wit = S.windows.find(t);
    if (wit != S.windows.end()) {
        int64_t slot = wit->second;
        if (slot == -2) {
            // another compute thread is installing this window right now
            while (true) {
                auto it2 = S.windows.find(t);
                if (it2 == S.windows.end()) {
                    return false; // the installer failed
                }
                if (it2->second != -2) {
                    slot = it2->second;
                    break;
                }
                S.n_wait++;
                S.cv_done.wait(lk);
            }
        }
        S.slots[slot].pins++;
        return true;
    }

    // claim before load_into() releases the lock for the pread
    S.windows[t] = -2;
    uint8_t * saved = (uint8_t *) t->data;

    disk_tensor dt;
    dt.fd     = pdt.fd;
    dt.fd_buf = pdt.fd_buf;
    dt.off    = off;
    dt.size   = size;
    dt.fsize  = pdt.fsize;

    int64_t slot = -1;
    if (!load_into(lk, t, dt, true, false, &slot)) {
        S.windows.erase(t);
        S.cv_done.notify_all();
        return false;
    }

    // remember the pointer we are about to overwrite (it may be a view into a resident parent)
    S.window_saved[t] = saved;

    S.windows[t] = slot;
    S.slots[slot].pins = 1;
    S.cv_done.notify_all();

    return true;
}

// free the windows whose pins reached zero, called with the lock held at the start of a node
void drain_deferred_windows() {
    if (S.windows_defer.empty()) {
        return;
    }
    std::vector<ggml_tensor *> keep;
    keep.reserve(S.windows_defer.size());
    for (ggml_tensor * t : S.windows_defer) {
        auto wit = S.windows.find(t);
        if (wit == S.windows.end() || wit->second < 0) {
            continue;
        }
        const int64_t slot = wit->second;
        if (S.slots[slot].pins > 0) {
            // somebody pinned it again after the last unpin
            keep.push_back(t);
            continue;
        }
        arena_slot & sl = S.slots[slot];
        free_range(sl.off, sl.size);
        sl.owner = nullptr;
        auto sit = S.window_saved.find(t);
        t->data = sit != S.window_saved.end() ? sit->second : S.placeholder;
        if (sit != S.window_saved.end()) {
            S.window_saved.erase(sit);
        }
        auto rit = S.window_restore_map.find(t);
        if (rit != S.window_restore_map.end()) {
            if (rit->second.ids != nullptr && !rit->second.orig.empty()) {
                memcpy(rit->second.ids->data, rit->second.orig.data(),
                       rit->second.orig.size() * sizeof(int32_t));
            }
            S.window_restore_map.erase(rit);
        }
        S.windows.erase(wit);
    }
    S.windows_defer.swap(keep);
}

void ensure_tree(std::unique_lock<std::mutex> & lk, ggml_tensor * t) {
    if (t == nullptr) {
        return;
    }
    auto wit = S.windows.find(t);
    if (wit != S.windows.end()) {
        while (wit != S.windows.end() && wit->second == -2) {
            // another compute thread is installing this window right now
            S.n_wait++;
            S.cv_done.wait(lk);
            wit = S.windows.find(t);
        }
        if (wit != S.windows.end()) {
            if (S.slots[wit->second].owner != t) {
                if (S.window_dbg) {
                    fprintf(stderr, "llama-disk[win]: STALE window entry for %s (slot %lld owner %s)\n",
                            ggml_get_name(t), (long long) wit->second,
                            S.slots[wit->second].owner ? ggml_get_name(S.slots[wit->second].owner) : "(null)");
                }
                S.windows.erase(wit);
            } else {
                S.slots[wit->second].pins++;
                return;
            }
        }
    }
    if (t->view_src != nullptr) {
        if (S.window && S.window_views && S.tensors.count(t->view_src) > 0 && ggml_is_contiguous(t)) {
            if (ensure_window(lk, t, t->view_src)) {
                return;
            }
        }
        if (S.window_dbg) {
            fprintf(stderr, "llama-disk[win]: view %s -> fallback parent %s\n",
                    ggml_get_name(t), ggml_get_name(t->view_src));
        }
        ensure_tree(lk, t->view_src);
        t->data = (uint8_t *) t->view_src->data + t->view_offs;
        return;
    }
    ensure_locked(lk, t);
}

void unpin_tree(ggml_tensor * t) {
    if (t == nullptr) {
        return;
    }
    auto wit = S.windows.find(t);
    if (wit != S.windows.end()) {
        arena_slot & sl = S.slots[wit->second];
        if (S.window_dbg && sl.owner != t) {
            fprintf(stderr, "llama-disk[win]: free %s but slot owner is %s\n",
                    ggml_get_name(t), sl.owner ? ggml_get_name(sl.owner) : "(null)");
        }
        if (sl.pins > 0) {
            sl.pins--;
        }
        if (sl.pins == 0) {
            // the window was only needed by this node. do not free it here: another compute
            // thread may still be waiting for it (some ops run on thread 0 only), so the free
            // is deferred to the start of the next node, after the node barrier
            bool queued = false;
            for (ggml_tensor * d : S.windows_defer) {
                if (d == t) {
                    queued = true;
                    break;
                }
            }
            if (!queued) {
                S.windows_defer.push_back(t);
            }
        }
        return;
    }
    if (t->view_src != nullptr) {
        unpin_tree(t->view_src);
        return;
    }
    unpin(t);
}

// --------------------------------------------------------------- MoE fast path
//
// A MUL_MAT_ID node only multiplies the experts the router picked, but its weight tensor
// holds every expert. Stream just the picked slices into one compact block of the arena,
// renumber the ids to the compact slots and point the weight tensor at the block while
// the node runs (ne[2] is temporarily set to the number of picked experts so that the
// per-expert stride arithmetic of the kernels lines up with the compact block). Every
// assumption is checked; anything unexpected falls back to the whole-tensor load.

bool moe_try_node(std::unique_lock<std::mutex> & lk, ggml_tensor * node) {
    ggml_tensor * w   = node->src[0];
    ggml_tensor * ids = node->src[2];
    if (w == nullptr || ids == nullptr) {
        return false;
    }

    // another thread of this graph already installed (or is installing) the window
    for (;;) {
        auto wit = S.moe_windows.find(w);
        if (wit == S.moe_windows.end()) {
            break;
        }
        if (wit->second.state == MOE_WIN_LOADING) {
            S.n_wait++;
            S.cv_done.wait(lk);
            continue;
        }
        arena_slot & sl = S.slots[wit->second.slot];
        sl.pins++;
        sl.tick      = ++S.tick;
        tls_moe_w    = w;
        tls_moe_used = true;
        return true;
    }

    auto no = [&](const char * why) {
        S.n_moe_fallback++;
        if (S.verbose) {
            dmsg("llama-disk[moe]: '%s' falls back to the whole tensor (%s)\n",
                    ggml_get_name(w), why);
        }
        return false;
    };

    if (w->ne[2] < 2 || w->ne[3] != 1)   return no("weight is not a 3d expert tensor");
    if (w->view_src != nullptr)          return no("weight is a view");
    if (w->nb[0] != ggml_type_size(w->type)) return no("weight is permuted");

    auto it = S.tensors.find(w);
    if (it == S.tensors.end()) return no("weight is not streamed");
    disk_tensor & dt = it->second;
    if (dt.state == ST_LOADING) return no("weight is being loaded right now");
    if (dt.state != ST_ABSENT) {
        // a previous fallback (or the prefetcher) may have left the whole tensor resident.
        // the compact block is a lot smaller, so take the arena block back when nobody uses it
        if (dt.state == ST_RESIDENT && dt.slot >= 0) {
            arena_slot & sl = S.slots[dt.slot];
            if (sl.pins > 0 || sl.owner != w) {
                return no("whole weight is pinned by another node");
            }
            sl.owner = nullptr;
            sl.pins  = 0;
            dt.state = ST_ABSENT;
            dt.slot  = -1;
            w->data  = S.placeholder;
            free_range(sl.off, sl.size);
            S.bytes_evicted += sl.size;
            S.n_evict++;
        } else {
            return no("weight is already queued or resident");
        }
    }
    if ((size_t) w->ne[2] * w->nb[2] != dt.size) return no("unexpected expert stride");
    if (dt.fsize > 0 && dt.off + dt.size > dt.fsize) return no("weight is outside the file");

    // the ids tensor: [n_expert_used, n_tokens], int32. it is usually a view of the argsort
    // result, so the rows keep the full n_expert stride - read and copy through the strides
    if (ids->type != GGML_TYPE_I32) return no("ids are not i32");
    if (ids->data == nullptr)       return no("ids are not computed yet");
    if (ids->ne[2] != 1 || ids->ne[3] != 1) return no("ids are not 2d");
    if (ids->nb[0] != sizeof(int32_t)) return no("ids element size mismatch");
    if (ids->nb[1] < (size_t) ids->ne[0] * sizeof(int32_t)) return no("ids rows are overlapping");
    if ((ids->nb[1] % sizeof(int32_t)) != 0) return no("ids row stride is not i32 aligned");
    if (((uintptr_t) ids->data & (sizeof(int32_t) - 1)) != 0) return no("ids are unaligned");

    const int64_t n_expert = w->ne[2];
    const int64_t n_ids    = ggml_nelements(ids);
    if (n_ids < 1 || n_ids > (int64_t) (1 << 20)) return no("unexpected ids count");

    const char * ids_base = (const char *) ids->data;
    tls_moe_ids.clear();
    tls_moe_ids.reserve((size_t) n_ids);
    for (int64_t i1 = 0; i1 < ids->ne[1]; ++i1) {
        for (int64_t i0 = 0; i0 < ids->ne[0]; ++i0) {
            const int32_t id = *(const int32_t *) (ids_base + i1 * ids->nb[1] + i0 * ids->nb[0]);
            // upstream ggml_mul_mat_id allows -1 to skip a slot (the kernel zeroes that
            // output row), every other id must name one of the experts of the tensor
            if (id < -1 || id >= n_expert) return no("expert id out of range");
            tls_moe_ids.push_back(id);
        }
    }

    // stream only the experts that are actually routed to; -1 is a "skip this slot"
    // marker of the upstream kernel and has no weight slice behind it
    tls_moe_sel = tls_moe_ids;
    tls_moe_sel.erase(std::remove(tls_moe_sel.begin(), tls_moe_sel.end(), -1), tls_moe_sel.end());
    std::sort(tls_moe_sel.begin(), tls_moe_sel.end());
    tls_moe_sel.erase(std::unique(tls_moe_sel.begin(), tls_moe_sel.end()), tls_moe_sel.end());

    const int64_t n_sel = (int64_t) tls_moe_sel.size();
    if (n_sel < 1) {
        // every slot is skipped: nothing to stream, the whole-tensor path stays correct
        return no("all expert slots are skipped (-1)");
    }
    if (n_sel >= n_expert) return no("all experts selected");

    const size_t expert_bytes = (size_t) w->nb[2];
    const size_t data_bytes   = (size_t) n_sel * expert_bytes;
    const size_t ids_off      = align_up(data_bytes, 64);
    const size_t remap_bytes  = (size_t) ids->ne[1] * ids->nb[1]; // including the row padding
    const size_t need         = align_up(ids_off + remap_bytes);

    if (data_bytes > S.arena_size || need > S.arena_size) {
        return no("expert block does not fit in the arena");
    }

    // original id -> compact slot, in the order the kernel reads the ids; -1 stays -1
    tls_moe_remap.resize((size_t) n_ids);
    for (int64_t i = 0; i < n_ids; ++i) {
        const int32_t id = tls_moe_ids[(size_t) i];
        if (id < 0) {
            tls_moe_remap[(size_t) i] = -1;
            continue;
        }
        const auto lb = std::lower_bound(tls_moe_sel.begin(), tls_moe_sel.end(), id);
        tls_moe_remap[(size_t) i] = (int32_t) (lb - tls_moe_sel.begin());
    }

    // same placement loop as load_into(): a big block drains the prefetcher, then evicts
    size_t off = 0;
    int n_stuck = 0;
    const bool want_drain = need >= S.drain_threshold;
    if (want_drain) {
        S.drain = true;
        while (S.n_loading > 0) {
            S.n_wait++;
            S.cv_done.wait(lk);
        }
    }
    while (!take_range(need, &off)) {
        if (evict_one()) {
            continue;
        }
        if (++n_stuck > 200) {
            if (want_drain) {
                S.drain = false;
            }
            return no("no room in the arena");
        }
        S.n_wait++;
        S.cv_done.wait(lk);
    }
    if (want_drain) {
        S.drain = false;
    }

    int64_t idx = -1;
    for (size_t i = 0; i < S.slots.size(); ++i) {
        if (S.slots[i].owner == nullptr) {
            idx = (int64_t) i;
            break;
        }
    }
    if (idx < 0) {
        if (S.slots.size() >= S.slots.capacity()) {
            // would invalidate the index across the unlock below; never expected (reserved in init)
            free_range(off, need);
            return no("slot table is full");
        }
        S.slots.emplace_back();
        idx = (int64_t) S.slots.size() - 1;
    }
    {
        arena_slot & sl = S.slots[idx];
        sl.off   = off;
        sl.size  = need;
        sl.owner = w;
        sl.pins  = 1; // the load pin; it becomes the electing thread's pin
        sl.tick  = ++S.tick;
    }

    S.n_loading++;

    moe_window win;
    win.w        = w;
    win.ids      = ids;
    win.slot     = idx;
    win.state    = MOE_WIN_LOADING;
    win.ne2      = w->ne[2];
    win.w_data   = (uint8_t *) w->data;
    win.ids_data = (uint8_t *) ids->data;
    S.moe_windows.emplace(w, win);

    uint8_t * const base = S.arena + off;

    // the copy runs without the lock (the other threads wait on MOE_WIN_LOADING), the
    // reserved slot is pinned so it cannot be evicted while the slices are streamed in
    lk.unlock();
    const uint64_t t0 = now_us();
    bool ok = true;
    for (int64_t i = 0; ok && i < n_sel; ) {
        int64_t j = i + 1;
        while (j < n_sel && tls_moe_sel[(size_t) j] == tls_moe_sel[(size_t) j - 1] + 1) {
            j++; // adjacent experts: one read for the run
        }
        ok = read_exact(dt.fd_buf, base + (size_t) i * expert_bytes,
                (size_t) (j - i) * expert_bytes,
                dt.off + (size_t) tls_moe_sel[(size_t) i] * expert_bytes);
        i = j;
    }
    const uint64_t dt_us = now_us() - t0;
    lk.lock();

    S.t_read += dt_us;
    S.n_loading--;

    // the slot vector never reallocates (reserved in init), never keep a reference across the unlock
    arena_slot & sl = S.slots[idx];

    if (!ok) {
        // let the regular whole-tensor load below try instead of failing the run
        sl.owner = nullptr;
        sl.pins  = 0;
        free_range(off, need);
        S.moe_windows.erase(w);
        S.cv_done.notify_all();
        return false;
    }

    // compact block is in place: renumber the ids (through the original strides) and publish
    uint8_t * compact_ids = base + ids_off;
    for (int64_t i1 = 0; i1 < ids->ne[1]; ++i1) {
        for (int64_t i0 = 0; i0 < ids->ne[0]; ++i0) {
            const size_t elem = (size_t) (i1 * ids->ne[0] + i0);
            *(int32_t *) (compact_ids + i1 * ids->nb[1] + i0 * ids->nb[0]) = tls_moe_remap[elem];
        }
    }

    w->data   = base;
    w->ne[2]  = n_sel;
    ids->data = (uint8_t *) compact_ids;

    auto wit = S.moe_windows.find(w);
    if (wit == S.moe_windows.end()) {
        // cannot happen while this node is running, but never leave the tensors patched
        w->data   = win.w_data;
        w->ne[2]  = win.ne2;
        ids->data = win.ids_data;
        sl.owner  = nullptr;
        sl.pins   = 0;
        free_range(off, need);
        return false;
    }
    wit->second.state = MOE_WIN_READY;

    S.n_load++;
    S.bytes_loaded += data_bytes;
    S.n_moe_nodes++;
    S.n_moe_experts += (uint64_t) n_sel;
    S.n_moe_bytes   += data_bytes;

    if (S.verbose) {
        dmsg("llama-disk[moe]: '%s' selected %d/%d experts, %zu KiB, %d ids\n",
                ggml_get_name(w), (int) n_sel, (int) n_expert, data_bytes >> 10, (int) n_ids);
    }

    tls_moe_w    = w;
    tls_moe_used = true;

    S.cv_done.notify_all();
    return true;
}

// drop this thread's pin on the compact window, the last one restores the tensors and frees it
void moe_release(ggml_tensor * w) {
    auto it = S.moe_windows.find(w);
    if (it == S.moe_windows.end()) {
        return;
    }

    moe_window & win = it->second;
    if (win.state == MOE_WIN_LOADING) {
        return; // a pin is only handed out once the window is ready
    }

    arena_slot & sl = S.slots[win.slot];
    if (sl.pins > 0) {
        sl.pins--;
    }
    if (sl.pins > 0) {
        return;
    }

    // last user: put the tensors back the way the rest of the engine expects them
    w->data       = win.w_data;
    w->ne[2]      = win.ne2;
    win.ids->data = win.ids_data;

    free_range(sl.off, sl.size);
    sl.owner = nullptr;
    sl.pins  = 0;

    S.moe_windows.erase(it);
    S.cv_done.notify_all();
}

// hand the given tensor to the reader, best effort
void enqueue_locked(ggml_tensor * t) {
    if (!S.prefetch || t == nullptr) {
        return;
    }
    if (t->view_src != nullptr) {
        t = t->view_src;
    }
    auto it = S.tensors.find(t);
    if (it == S.tensors.end()) {
        return;
    }
    if (it->second.state != ST_ABSENT) {
        return;
    }
    if (it->second.size >= S.drain_threshold) {
        // a big tensor cannot be hidden behind a layer worth of compute, do not prefetch it
        return;
    }
    it->second.state = ST_QUEUED;
    S.queue.push_back(t);
    S.cv_work.notify_one();
}

void reader_main() {
    std::unique_lock<std::mutex> lk(S.mtx);

    while (true) {
        S.cv_work.wait(lk, [] { return S.stop || !S.queue.empty(); });
        if (S.stop) {
            return;
        }

        ggml_tensor * t = S.queue.front();
        S.queue.pop_front();

        auto it = S.tensors.find(t);
        if (it == S.tensors.end() || it->second.state != ST_QUEUED) {
            continue;
        }

        disk_tensor & dt = it->second;

        if (S.drain) {
            // a big tensor is being placed, keep the arena free
            dt.state = ST_ABSENT;
            continue;
        }

        // prefetch is best effort: never block the compute threads for arena space
        if (!load_into(lk, t, dt, false, true)) {
            if (!S.fail) {
                dt.state = ST_ABSENT;
                S.n_pf_drop++;
                S.cv_done.notify_all();
            } else {
                S.cv_done.notify_all();
                return;
            }
        }
    }
}

void node_cb(ggml_tensor * node, bool done) {
    if (!S.on || node == nullptr) {
        return;
    }

    // ggml does not compute empty nodes at all (zero tokens, e.g. the unused MTP/nextn block),
    // so their weights are never touched and must not be paged in
    if (ggml_is_empty(node)) {
        return;
    }

    std::unique_lock<std::mutex> lk(S.mtx);

    if (!done) {
        drain_deferred_windows();
        tls_cur_node = node;
        // a new node: forget the window this thread pinned for the previous one
        tls_moe_w    = nullptr;
        tls_moe_used = false;
        tls_rows_w   = nullptr;
    }

    if (!done && S.fail) {
        // keep going, the output will be garbage but we must not leak pins
        return;
    }

    S.n_cb++;

    const bool is_moe_node = S.moe && node->op == GGML_OP_MUL_MAT_ID;

    if (!done && S.prefetch) {
        // the weights are read in file order, so the next tensors of the file are a good
        // guess for what the next nodes will need
        for (int i = 0; i < GGML_MAX_SRC; ++i) {
            ggml_tensor * src = node->src[i];
            if (src == nullptr) {
                continue;
            }
            if (src->view_src != nullptr) {
                src = src->view_src;
            }
            auto it = S.tensors.find(src);
            if (it == S.tensors.end()) {
                continue;
            }
            const size_t ord = it->second.ord;
            for (size_t k = 1; k <= S.lookahead; ++k) {
                if (ord + k >= S.order.size()) {
                    break;
                }
                enqueue_locked(S.order[ord + k]);
            }
        }
    }

    if (!done && S.window && S.window_rows && node->op == GGML_OP_GET_ROWS) {
        if (ensure_rows_window(lk, node)) {
            // ensure_rows_window() already pinned the window for this node; remember it so
            // the generic source loop below does not pin the same tensor a second time
            tls_rows_w = node->src[0];
        }
    }

    // MoE fast path: the expert weights come from a compact window instead of the whole
    // tensor, src[0] (weights) and src[2] (ids) are already prepared by moe_try_node()
    bool moe_handled = false;
    if (is_moe_node) {
        if (!done) {
            moe_handled = moe_try_node(lk, node);
        } else {
            moe_handled = tls_moe_used && tls_moe_w == node->src[0];
            if (moe_handled) {
                moe_release(node->src[0]);
                tls_moe_w    = nullptr;
                tls_moe_used = false;
            }
        }
    }

    for (int i = 0; i < GGML_MAX_SRC; ++i) {
        ggml_tensor * src = node->src[i];
        if (src == nullptr) {
            continue;
        }
        if (moe_handled && (i == 0 || i == 2)) {
            continue; // supplied by the compact expert window
        }

        if (!done) {
            if (src == tls_rows_w) {
                continue;   // already pinned by ensure_rows_window()
            }
            ensure_tree(lk, src);
        } else {
            if (src == tls_rows_w) {
                unpin_tree(src);
                tls_rows_w = nullptr;
                continue;
            }
            unpin_tree(src);
        }
    }

    if (done) {
        S.cv_done.notify_all();
    }

    if (S.n_cb % 4096 == 0 && S.verbose) {
        dmsg("llama-disk: loads = %llu (prefetch %llu, dropped %llu), read = %llu MiB, "
                        "evictions = %llu, io = %llu.%llu s, waits = %llu\n",
                (unsigned long long) S.n_load, (unsigned long long) S.n_pf_load,
                (unsigned long long) S.n_pf_drop,
                (unsigned long long) (S.bytes_loaded >> 20), (unsigned long long) S.n_evict,
                (unsigned long long) (S.t_read / 1000000), (unsigned long long) (S.t_read / 100000 % 10),
                (unsigned long long) S.n_wait);
        if (S.moe) {
            dmsg("llama-disk[moe]: nodes = %llu, experts = %llu, expert read = %llu MiB, fallbacks = %llu\n",
                    (unsigned long long) S.n_moe_nodes, (unsigned long long) S.n_moe_experts,
                    (unsigned long long) (S.n_moe_bytes >> 20),
                    (unsigned long long) S.n_moe_fallback);
        }
    }
}

// open the file again with O_DIRECT / FILE_FLAG_NO_BUFFERING, INVALID_DISK_HANDLE when
// that is not possible. `align_io` receives the alignment the volume requires.
disk_handle open_direct_handle(int fd, size_t * align_io) {
#if defined(_WIN32)
    if (fd < 0) {
        return INVALID_DISK_HANDLE;
    }

    HANDLE os = (HANDLE) _get_osfhandle(fd);
    if (os == INVALID_HANDLE_VALUE) {
        return INVALID_DISK_HANDLE;
    }

    wchar_t path[32768];
    const DWORD n = GetFinalPathNameByHandleW(os, path, (DWORD) (sizeof(path) / sizeof(path[0])),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= (DWORD) (sizeof(path) / sizeof(path[0]))) {
        return INVALID_DISK_HANDLE;
    }

    // the volume sector size decides the alignment of an unbuffered request
    const wchar_t * local = path;
    if (wcsncmp(local, L"\\\\?\\", 4) == 0) {
        local += 4;
    }
    if (wcsncmp(local, L"\\\\", 2) == 0 || local[1] != L':') {
        // UNC path: no cheap way to know a valid alignment, stay buffered
        return INVALID_DISK_HANDLE;
    }

    const wchar_t root[4] = { local[0], local[1], local[2], L'\0' };

    DWORD spc = 0, bps = 0, nfree = 0, nclusters = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &nfree, &nclusters) || bps < 512) {
        return INVALID_DISK_HANDLE;
    }
    if ((bps & (bps - 1)) != 0 || bps > ALIGN) {
        // not a power of two or coarser than the arena alignment
        return INVALID_DISK_HANDLE;
    }

    // synchronous handle + the OVERLAPPED offset in read_at(): the documented positioned
    // read (same thing rust's seek_read does), safe to call from several threads at once
    HANDLE h = CreateFileW(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return INVALID_DISK_HANDLE;
    }

    // probe one aligned sector, some volumes/filesystems refuse unbuffered reads
    LARGE_INTEGER fsize;
    uint8_t * probe = (uint8_t *) _aligned_malloc(bps, bps);
    const bool ok = probe != nullptr && GetFileSizeEx(h, &fsize) && fsize.QuadPart >= (LONGLONG) bps &&
            read_at(h, probe, bps, 0) == (int64_t) bps;
    if (probe != nullptr) {
        _aligned_free(probe);
    }
    if (!ok) {
        CloseHandle(h);
        return INVALID_DISK_HANDLE;
    }

    *align_io = (size_t) bps;
    return h;
#else
    char path[4096];
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0) {
        return INVALID_DISK_HANDLE;
    }
    path[n] = '\0';

    const int dfd = open(path, O_RDONLY | O_DIRECT);
    if (dfd < 0) {
        return INVALID_DISK_HANDLE;
    }
    struct stat st;
    if (fstat(dfd, &st) != 0) {
        close(dfd);
        return INVALID_DISK_HANDLE;
    }
    *align_io = (size_t) (st.st_blksize > 0 ? st.st_blksize : 512);
    return dfd;
#endif
}

} // namespace

bool enabled() {
    static const bool on = [] {
        // disk streaming is the default; LLAMA_DISK_STREAM_MB=0 falls back to the
        // stock mmap / full-load behaviour
        const char * env = getenv("LLAMA_DISK_STREAM_MB");
        return env == nullptr || atoi(env) != 0;
    }();
    return on;
}

bool moe_enabled() {
    static const bool on = [] {
        // only meaningful while the weights are streamed from disk
        if (!enabled()) {
            return false;
        }
        const char * env = getenv("LLAMA_DISK_MOE");
        return env != nullptr && atoi(env) == 1;
    }();
    return on;
}

int out_chunk_rows() {
    if (!enabled()) {
        return 0;
    }
    static const int rows = [] {
        const char * env = getenv("LLAMA_DISK_OUT_CHUNK");
        if (env == nullptr) {
            return 0;
        }
        const int n = atoi(env);
        return n > 0 ? n : 0;
    }();
    if (rows > 0) {
        return rows;
    }
    return S.tiny ? S.auto_chunk_rows : 0;
}

bool tiny_arena_mode() {
    return enabled() && S.tiny;
}

bool split_all_matmul() {
    if (!enabled()) {
        return false;
    }
    static const int env = [] {
        const char * e = getenv("LLAMA_DISK_SPLIT_MM");
        return e != nullptr ? atoi(e) : -1;
    }();
    if (env >= 0) {
        return env == 1;
    }
    return S.tiny;
}

int matmul_chunk_rows(int64_t ne1, size_t row_bytes) {
    if (!enabled() || row_bytes == 0 || ne1 <= 0) {
        return 0;
    }
    static const int rows_env = [] {
        const char * env = getenv("LLAMA_DISK_OUT_CHUNK");
        if (env == nullptr) {
            return 0;
        }
        const int n = atoi(env);
        return n > 0 ? n : 0;
    }();
    if (rows_env > 0) {
        return rows_env;
    }
    if (!S.tiny) {
        return 0;
    }
    // one chunk ~1/4 of the arena, at least 256 rows, at most 64 chunks per tensor so the graph
    // does not explode for large output layers
    int64_t rows = (int64_t) (std::max<size_t>(S.arena_size / 4, 2u << 20) / row_bytes);
    rows = std::max<int64_t>(rows, 256);
    rows = std::max<int64_t>(rows, (ne1 + 63) / 64);
    rows = std::min<int64_t>(rows, 16384);
    return (int) rows;
}

int64_t split_threshold_bytes() {
    if (!S.tiny) {
        return 0;
    }
    // split a weight whose read would not leave room for the other tensors of the node
    return (int64_t) (S.arena_size * 3 / 5);
}

void init() {
    if (!enabled() || S.on) {
        return;
    }

    // the arena is sized and allocated in finish_register(), once the largest
    // tensor is known; 0 means "size it automatically"
    const char * env_mb = getenv("LLAMA_DISK_STREAM_MB");
    S.arena_req = (env_mb != nullptr && atoi(env_mb) > 0) ? ((size_t) atoi(env_mb) << 20) : 0;

    const char * env_mlock = getenv("LLAMA_DISK_MLOCK");
    S.mlock = env_mlock != nullptr && atoi(env_mlock) == 1;

    // direct io is worth it when streaming: it keeps 3+ GB of reads out of the page cache
    const char * env_dio = getenv("LLAMA_DISK_ODIRECT");
    S.odirect = env_dio == nullptr || atoi(env_dio) != 0;

    // prefetching never paid off on the devices we measured, keep it opt in
    const char * env_pf = getenv("LLAMA_DISK_PREFETCH");
    S.prefetch = env_pf != nullptr && atoi(env_pf) != 0;

    const char * env_la = getenv("LLAMA_DISK_LOOKAHEAD");
    if (env_la != nullptr && atoi(env_la) > 0) {
        S.lookahead = (size_t) atoi(env_la);
    }

    const char * env_v = getenv("LLAMA_DISK_VERBOSE");
    S.verbose = env_v != nullptr && atoi(env_v) == 1;

    const char * env_win = getenv("LLAMA_DISK_WINDOW");
    const int    win_mode = env_win != nullptr ? atoi(env_win) : 0;
    const char * env_wdbg = getenv("LLAMA_DISK_WINDOW_DEBUG");
    S.window_dbg = env_wdbg != nullptr && atoi(env_wdbg) == 1;
    S.window = win_mode > 0;
    S.window_rows  = (win_mode == 1 || win_mode == 2);
    S.window_views = (win_mode == 1 || win_mode == 3);

    const char * env_dbg = getenv("LLAMA_DISK_DEBUG");
    S.debug = env_dbg != nullptr && atoi(env_dbg) == 1;

    S.moe = moe_enabled();
    if (S.moe) {
        dmsg("llama-disk: MoE fast path on, only the experts selected by the router are streamed\n");
        if (S.prefetch) {
            // the prefetcher walks the file order and would read the whole expert tensors
            S.prefetch = false;
            dmsg("llama-disk: prefetch disabled while the MoE fast path is on\n");
        }
    }

    // node fusion would skip nodes, and a skipped node never gets its weights paged in
    if (getenv("GGML_CPU_DISABLE_FUSION") == nullptr) {
#if defined(_WIN32)
        _putenv_s("GGML_CPU_DISABLE_FUSION", "1");
#else
        setenv("GGML_CPU_DISABLE_FUSION", "1", 1);
#endif
    }

    // tensors get this as their data pointer while they are registered, before the
    // arena exists; nothing dereferences it, it just has to be non null
    S.placeholder = g_dummy;
    S.slots.reserve(8192); // never reallocate, load_into indexes into it across an unlock
    S.on = true;
}

// one value out of /proc/meminfo, in kB
static size_t meminfo_kb(const char * key) {
#if defined(_WIN32)
    // no /proc/meminfo here, the two keys the caller uses come from the global memory status
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        return 0;
    }
    if (strncmp(key, "MemTotal", 8) == 0) {
        return (size_t) (ms.ullTotalPhys >> 10);
    }
    if (strncmp(key, "MemAvailable", 12) == 0) {
        return (size_t) (ms.ullAvailPhys >> 10);
    }
    return 0;
#else
    FILE * f = fopen("/proc/meminfo", "r");
    if (f == nullptr) {
        return 0;
    }
    const size_t klen = strlen(key);
    char line[256];
    size_t out = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strncmp(line, key, klen) == 0) {
            out = (size_t) strtoull(line + klen, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return out;
#endif
}

// LLAMA_DISK_TINY_ARENA=0 disables the automatic tiny-arena mode, everything else keeps it on
static bool tiny_arena_disabled() {
    static const bool v = [] {
        const char * e = getenv("LLAMA_DISK_TINY_ARENA");
        return e != nullptr && atoi(e) == 0;
    }();
    return v;
}

// size and allocate the arena now that every tensor (and so the biggest one) is known
static bool alloc_arena() {
    const size_t need = align_up(S.max_tensor) + (24u << 20);

    size_t want = S.arena_req;
    const char * why = "requested";
    if (want == 0) {
        const size_t tot_kb = meminfo_kb("MemTotal:");
        want = tot_kb > 0 ? (((tot_kb >> 10) / 8) << 20) : (256u << 20);
        why = "auto, 1/8 of ram";
    }
    // only an explicit request goes below the largest tensor; the automatic 1/8-of-ram size
    // keeps the old behaviour (raised to fit the largest tensor)
    if (want < need && S.arena_req > 0 && !tiny_arena_disabled()) {
        S.tiny = true;
        dmsg("llama-disk: tiny arena mode: arena %zu MiB < largest tensor '%s' (%zu MiB), "
             "big tensors will be read in chunks\n",
                want >> 20, S.name_max.c_str(), S.max_tensor >> 20);
    } else if (want < need) {
        dmsg("llama-disk: arena %zu MiB cannot hold the largest tensor '%s' (%zu MiB), raised to %zu MiB\n",
                want >> 20, S.name_max.c_str(), S.max_tensor >> 20, need >> 20);
        want = need;
        why = "raised to fit the largest tensor";
    }
    const size_t floor_mb = S.tiny ? 8u : 128u;
    if (want < (floor_mb << 20)) {
        want = floor_mb << 20;
        why = S.tiny ? "8 MiB floor (tiny arena)" : "128 MiB floor";
    }
    if (want > (2048u << 20)) {
        want = 2048u << 20;
        why = "2048 MiB cap";
    }

    const size_t avail_kb = meminfo_kb("MemAvailable:");
    if (avail_kb > 0) {
        const size_t avail_mb = avail_kb >> 10;
        size_t cap = avail_mb > 220 ? ((avail_mb - 220) << 20) : 0;
        if (cap < (176u << 20)) {
            cap = 176u << 20;
        }
        if (want > cap && cap >= need) {
            dmsg("llama-disk: arena %zu MiB clamped to %zu MiB, only %zu MiB is free\n",
                    want >> 20, cap >> 20, avail_mb);
            want = cap;
            why = "clamped to what is free";
        }
    }

    S.arena_size = align_up(want);

#if defined(_WIN32)
    S.arena = (uint8_t *) _aligned_malloc(S.arena_size, ALIGN);
#else
    void * p = nullptr;
    if (posix_memalign(&p, ALIGN, S.arena_size) != 0) {
        p = nullptr;
    }
    S.arena = (uint8_t *) p;
#endif

    if (S.arena == nullptr) {
        dmsg("llama-disk: failed to allocate %zu MiB arena\n", S.arena_size >> 20);
        return false;
    }

    bool locked = false;
    if (S.mlock) {
#if defined(_WIN32)
        // the number of pages a process may lock is bounded by its MINIMUM working set,
        // so the arena has to be declared there before VirtualLock can succeed
        SIZE_T ws_min = 0;
        SIZE_T ws_max = 0;
        if (GetProcessWorkingSetSize(GetCurrentProcess(), &ws_min, &ws_max)) {
            const SIZE_T want     = S.arena_size + (32ull << 20);
            const SIZE_T want_max = want + (64ull << 20);
            if (ws_min < want) {
                SetProcessWorkingSetSize(GetCurrentProcess(), want,
                        ws_max > want_max ? ws_max : want_max);
            }
        }
        locked = VirtualLock(S.arena, S.arena_size) != 0;
        if (!locked) {
            dmsg("llama-disk: VirtualLock failed (error %lu), the arena can be paged out; streaming still works\n",
                    (unsigned long) GetLastError());
        }
#else
        struct rlimit rl;
        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_MEMLOCK, &rl);
        }
        locked = mlock(S.arena, S.arena_size) == 0;
        if (!locked) {
            dmsg("llama-disk: mlock failed, the arena can be swapped out\n");
        }
#endif
    }

    S.free_blk.emplace(0, S.arena_size);
    S.placeholder = S.arena;

    dmsg("llama-disk: streaming weights from disk, arena = %zu MiB (%s), direct io = %d, mlock = %d, prefetch = %d (lookahead %zu)\n",
            S.arena_size >> 20, why, (int) S.odirect, (int) locked, (int) S.prefetch, S.lookahead);
    return true;
}

void register_tensor(ggml_tensor * t, int fd, size_t off, size_t nbytes) {
    if (!S.on || t == nullptr || nbytes == 0) {
        return;
    }

    auto it = S.fd_map.find(fd);
    if (it == S.fd_map.end()) {
        fd_info info;

        info.fsize  = file_size_of(fd);
        info.fd_buf = open_read_handle(fd);
        info.fd     = info.fd_buf;

        if (!handle_ok(info.fd_buf)) {
            dmsg("llama-disk: cannot open a read handle for the model file, streaming disabled\n");
            S.fail = true;
            return;
        }

        if (S.odirect) {
            size_t align_io = ALIGN;
            const disk_handle dfd = open_direct_handle(fd, &align_io);
            if (handle_ok(dfd)) {
                S.align_io = std::max(S.align_io, align_io);
                info.fd = dfd;
            } else {
                dmsg("llama-disk: unbuffered reads unavailable on this volume, using buffered reads\n");
                S.odirect = false;
            }
        }

        const int idx = (int) S.fds.size();
        S.fds.push_back(info);
        it = S.fd_map.emplace(fd, idx).first;
    }

    disk_tensor dt;
    dt.fd     = S.fds[it->second].fd;
    dt.fd_buf = S.fds[it->second].fd_buf;
    dt.off    = off;
    dt.size   = nbytes;
    dt.fsize  = S.fds[it->second].fsize;

    if (strncmp(ggml_get_name(t), "blk.", 4) == 0) {
        dt.layer = atoi(ggml_get_name(t) + 4);
    }

    S.tensors.emplace(t, dt);

    t->data = S.placeholder;

    S.bytes_registered += nbytes;
    S.max_row_bytes = std::max(S.max_row_bytes, (size_t) t->nb[1]);
    if (nbytes > S.max_tensor) {
        S.max_tensor = nbytes;
        S.name_max   = ggml_get_name(t);
    }
}

void finish_register() {
    if (!S.on) {
        return;
    }

    dmsg("llama-disk: %zu tensors registered, %llu MiB to stream, largest tensor '%s' %zu MiB\n",
            S.tensors.size(), (unsigned long long) (S.bytes_registered >> 20), S.name_max.c_str(), S.max_tensor >> 20);

    if (!alloc_arena()) {
        S.fail = true;
        return;
    }

    if (S.tiny) {
        if (!S.window) {
            // big tensors have to be read as rows/views: no single read may need a whole tensor
            S.window       = true;
            S.window_rows  = true;
            S.window_views = true;
        }
        // keep one chunk around 1/5 of the arena (a chunk fits together with the other tensors
        // of the node and the rows/experts windows that are pinned at the same time)
        if (S.max_row_bytes > 0) {
            int64_t rows = (int64_t) ((S.arena_size / 5) / S.max_row_bytes);
            rows = std::max<int64_t>(256, std::min<int64_t>(rows, 16384));
            S.auto_chunk_rows = (int) rows;
        }
        dmsg("llama-disk: tiny arena: chunked reads, %d rows per matmul chunk, windows on\n",
                S.auto_chunk_rows);
    }

    if (align_up(S.max_tensor + S.align_io) > S.arena_size) {
        if (S.tiny) {
            dmsg("llama-disk: tiny-arena: largest tensor '%s' (%zu MiB) exceeds the arena, "
                 "it must be read in chunks\n",
                    S.name_max.c_str(), S.max_tensor >> 20);
        } else {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "arena is %zu MiB but the largest tensor '%s' needs %zu MiB",
                     S.arena_size >> 20, S.name_max.c_str(),
                     align_up(S.max_tensor + S.align_io) >> 20);
            throw std::runtime_error(msg);
        }
    }

    if (S.prefetch) {
        // file order is the order the weights are read in, use it as the prefetch heuristic
        std::vector<std::pair<size_t, ggml_tensor *>> tmp;
        tmp.reserve(S.tensors.size());
        for (auto & kv : S.tensors) {
            tmp.emplace_back(kv.second.off, kv.first);
        }
        std::sort(tmp.begin(), tmp.end());
        S.order.reserve(tmp.size());
        for (size_t i = 0; i < tmp.size(); ++i) {
            S.order.push_back(tmp[i].second);
            S.tensors[tmp[i].second].ord = i;
        }

        S.reader_running = true;
        S.reader = std::thread(reader_main);
    }

    if (S.debug) {
        S.watchdog = std::thread(watchdog_main);
    }

    ggml_cpu_set_node_residency_cb(node_cb);
}

void cleanup() {
    if (!S.on) {
        return;
    }

    ggml_cpu_set_node_residency_cb(nullptr);

    {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.stop = true;
        S.cv_work.notify_all();
        S.cv_done.notify_all();
    }

    // the threads are owned now, so a second model in the same process does not race
    // them against the fresh state built by the next init()
    if (S.reader.joinable()) {
        S.reader.join();
    }
    if (S.watchdog.joinable()) {
        S.watchdog.join();
    }

    {
        // never leave a weight tensor pointing into the arena: patch every live MoE window back
        std::lock_guard<std::mutex> lk(S.mtx);
        for (auto & kv : S.moe_windows) {
            moe_window & win = kv.second;
            if (win.w != nullptr) {
                win.w->data  = win.w_data;
                win.w->ne[2] = win.ne2;
            }
            if (win.ids != nullptr) {
                win.ids->data = win.ids_data;
            }
        }
        S.moe_windows.clear();
    }

    for (const auto & info : S.fds) {
        if (handle_ok(info.fd) && info.fd != info.fd_buf) {
            close_disk_handle(info.fd);
        }
        close_disk_handle(info.fd_buf);
    }

#if defined(_WIN32)
    _aligned_free(S.arena);
#else
    free(S.arena);
#endif

    // a process can load more than one model (common's --fit probe does exactly that),
    // so leave the state the way it was before init()
    S.fds.clear();
    S.fd_map.clear();
    S.tensors.clear();
    S.windows.clear();
    S.window_saved.clear();
    S.windows_defer.clear();
    S.window_restore_map.clear();
    S.order.clear();
    S.slots.clear();
    S.free_blk.clear();
    S.queue.clear();

    S.arena           = nullptr;
    S.arena_size      = 0;
    S.arena_req       = 0;
    S.placeholder     = g_dummy;
    S.align_io        = ALIGN;
    S.on              = false;
    S.fail            = false;
    S.stop            = false;
    S.drain           = false;
    S.reader_running  = false;
    S.n_loading       = 0;
    S.max_tensor      = 0;
    S.name_max.clear();
    S.cur_layer       = -1;
    S.n_layer         = -1;
    S.tick            = 0;
    S.n_load          = 0;
    S.n_pf_load       = 0;
    S.n_pf_hit        = 0;
    S.n_pf_drop       = 0;
    S.n_evict         = 0;
    S.n_cb            = 0;
    S.t_read          = 0;
    S.n_wait          = 0;
    S.bytes_registered = 0;
    S.bytes_loaded     = 0;
    S.bytes_evicted    = 0;
    S.n_moe_nodes      = 0;
    S.n_moe_experts    = 0;
    S.n_moe_bytes      = 0;
    S.n_moe_fallback   = 0;
    S.moe              = false;
}

void set_msg_cb(void (*cb)(const char *)) {
    g_msg_cb = cb;
}

void get_stats(struct llama_disk_stats * out) {
    if (out == nullptr) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (!S.on) {
        return;
    }

    std::lock_guard<std::mutex> lk(S.mtx);

    out->io_us     = S.t_read;
    out->loads     = S.n_load;
    out->bytes     = S.bytes_loaded;
    out->evictions = S.n_evict;
    out->clock_us  = now_us();
    out->layer     = S.cur_layer;
    out->n_layer   = S.n_layer;
    out->arena_bytes      = S.arena_size;
    out->registered_bytes = S.bytes_registered;
    out->tensors          = (int) S.tensors.size();
    out->direct_io        = S.odirect  ? 1 : 0;
    out->mlock            = S.mlock    ? 1 : 0;
    out->prefetch         = S.prefetch ? 1 : 0;
    out->tiny             = S.tiny ? 1 : 0;
    out->chunk_rows       = S.tiny ? S.auto_chunk_rows : 0;
    out->moe              = S.moe      ? 1 : 0;
    out->moe_nodes        = S.n_moe_nodes;
    out->moe_experts      = S.n_moe_experts;
    out->moe_expert_bytes = S.n_moe_bytes;

    if (S.debug) {
        static int n = 0;
        if (n < 8) {
            dmsg("get_stats: io=%llu loads=%llu", (unsigned long long) out->io_us, (unsigned long long) out->loads);
            n++;
        }
    }
}

} // namespace llama_disk
