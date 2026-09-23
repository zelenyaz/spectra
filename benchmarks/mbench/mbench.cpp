// mbench - memory performance micro-benchmark
//
// Scalar 64-bit load/store memory benchmark supporting multiple access orders
// (stride / stride_pf / stridep / chk_stride / rand) and operations
// (read / write / rw / wr), running
// several independent benches concurrently with per-bench buffers, optional
// NUMA buffer binding (mbind) and CPU pinning.
//
// Build: g++ -O3 -march=native -std=c++17 -pthread mbench.cpp -lnuma -o mbench

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <numa.h>
#include <numaif.h>

// ---------------------------------------------------------------------------
// Scalar 64-bit load / store (x86-64 inline asm). volatile prevents the
// optimizer from eliminating, reordering or vectorizing these accesses.
// ---------------------------------------------------------------------------
static inline uint64_t ld64(const uint64_t* p) {
    uint64_t v;
    asm volatile("movq %1, %0" : "=r"(v) : "m"(*p));
    return v;
}
static inline void st64(uint64_t* p, uint64_t v) {
    asm volatile("movq %1, %0" : "=m"(*p) : "r"(v));
}
static inline void cpu_relax() { asm volatile("pause" ::: "memory"); }

static constexpr uint64_t STEP = 0x9E3779B97F4A7C15ull; // golden-ratio odd const

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum Order { ORD_STRIDE, ORD_STRIDE_PF, ORD_STRIDEP, ORD_CHK_STRIDE, ORD_RAND };
enum Op    { OP_READ = 0, OP_WRITE = 1, OP_RW = 2, OP_WR = 3 };

static constexpr uint64_t ops_per_elem(int op) {
    return (op == OP_RW || op == OP_WR) ? 2u : 1u;
}

// ---------------------------------------------------------------------------
// Per-thread, cache-line-padded ops counter.
// ---------------------------------------------------------------------------
struct alignas(64) Slot {
    std::atomic<uint64_t> ops{0};
    char pad[64 - sizeof(std::atomic<uint64_t>)];
};

// Shared run state.
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_go{false};
static std::atomic<int>  g_ready{0};
static volatile uint64_t g_sink = 0; // sink to defeat dead-code elimination

// ---------------------------------------------------------------------------
// Per-element operation.
// ---------------------------------------------------------------------------
template <int OP>
static inline void do_touch(uint64_t* p, uint64_t& acc) {
    if constexpr (OP == OP_READ) {
        uint64_t v = ld64(p);
        acc ^= v;
    } else if constexpr (OP == OP_WRITE) {
        acc += STEP;
        st64(p, acc);
    } else if constexpr (OP == OP_RW) {
        uint64_t v = ld64(p);   // read a uint64_t ...
        st64(p, v);             // ... then write it (back)
        acc += v;
    } else { // OP_WR
        acc += STEP;
        st64(p, acc);           // write a uint64_t ...
        uint64_t v = ld64(p);   // ... then read it
        acc ^= v;
    }
}

// ---------------------------------------------------------------------------
// Strided kernel (used by both stride and stridep; only L differs).
//   for off in 0..L-1: for e=off; e<n; e+=L: touch(base[e])
// ---------------------------------------------------------------------------
template <int OP>
static void kernel_strided(uint64_t* base, uint64_t n, uint64_t L, Slot* slot,
                           uint64_t seed) {
    constexpr uint64_t ope = ops_per_elem(OP);
    constexpr uint64_t PUB = 1ull << 20; // publish/check every ~1M elements
    if (L < 1) L = 1;
    if (L > n) L = n;
    uint64_t acc = seed;
    uint64_t local = 0;
    // Each offset 0..L-1 sweeps one partition (elements off, off+L, off+2L,...).
    // The innermost per-element loop is kept free of any counter/branch so it
    // compiles to a tight scalar load+accumulate over a single pointer
    // induction variable; the publish/stop check is hoisted to the per-offset
    // loop and fires on an element-count threshold. This keeps the demand-load
    // issue rate (and thus MLP / memory bandwidth) high while still checking
    // g_stop and publishing the counter roughly every PUB elements, regardless
    // of stride. Each offset touches exactly ceil((n-off)/L) elements: q+1 for
    // the first r offsets, q afterwards (q=n/L, r=n%L) -- so the element count
    // is tracked without a per-element increment.
    const uint64_t q = n / L;
    const uint64_t r = n % L;
    uint64_t* const end = base + n;
    uint64_t since_pub = 0;
    for (;;) {
        for (uint64_t off = 0; off < L; off++) {
            for (uint64_t* p = base + off; p < end; p += L)
                do_touch<OP>(p, acc);
            uint64_t cnt = q + (off < r);
            local += cnt;
            since_pub += cnt;
            if (since_pub >= PUB) {
                slot->ops.store(local * ope, std::memory_order_relaxed);
                since_pub = 0;
                if (g_stop.load(std::memory_order_relaxed)) goto done;
            }
        }
    }
done:
    slot->ops.store(local * ope, std::memory_order_relaxed);
    g_sink ^= acc;
}

// ---------------------------------------------------------------------------
// Software-prefetched strided kernel (stride_pf). `pd` is measured in future
// strided accesses, so the requested prefetch lead is pd * L elements.
// ---------------------------------------------------------------------------
template <int OP>
static void kernel_strided_pf(uint64_t* base, uint64_t n, uint64_t L,
                              uint64_t pd, Slot* slot, uint64_t seed) {
    constexpr uint64_t ope = ops_per_elem(OP);
    constexpr uint64_t PUB = 1ull << 20; // publish/check every ~1M elements
    if (L < 1) L = 1;
    if (L > n) L = n;
    uint64_t acc = seed;
    uint64_t local = 0;
    const uint64_t q = n / L;
    const uint64_t r = n % L;
    uint64_t* const end = base + n;
    uint64_t since_pub = 0;
    for (;;) {
        for (uint64_t off = 0; off < L; off++) {
            uint64_t cnt = q + (off < r);
            uint64_t* p = base + off;
            if (pd == 0) {
                for (; p < end; p += L) {
                    __builtin_prefetch(p, 0, 0);
                    do_touch<OP>(p, acc);
                }
            } else {
                // pd < cnt guarantees pd * L is within this partition, so the
                // multiplication cannot overflow and every prefetched address
                // stays inside the thread-owned range.
                if (pd < cnt) {
                    uint64_t lead = pd * L;
                    uint64_t* const pf_end = end - lead;
                    for (; p < pf_end; p += L) {
                        __builtin_prefetch(p + lead, 0, 0);
                        do_touch<OP>(p, acc);
                    }
                }
                for (; p < end; p += L)
                    do_touch<OP>(p, acc);
            }
            local += cnt;
            since_pub += cnt;
            if (since_pub >= PUB) {
                slot->ops.store(local * ope, std::memory_order_relaxed);
                since_pub = 0;
                if (g_stop.load(std::memory_order_relaxed)) goto done;
            }
        }
    }
done:
    slot->ops.store(local * ope, std::memory_order_relaxed);
    g_sink ^= acc;
}

// ---------------------------------------------------------------------------
// Random kernel. xorshift64 + Lemire multiply-shift index (no division).
// ---------------------------------------------------------------------------
template <int OP>
static void kernel_random(uint64_t* base, uint64_t n, Slot* slot, uint64_t seed) {
    constexpr uint64_t ope = ops_per_elem(OP);
    constexpr int BATCH = 4096;
    uint64_t acc = seed;
    uint64_t rng = seed ? seed : 0x123456789abcdefull;
    uint64_t local = 0;
    for (;;) {
        for (int b = 0; b < BATCH; b++) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            uint64_t idx = (uint64_t)(((unsigned __int128)rng * n) >> 64);
            do_touch<OP>(base + idx, acc);
        }
        local += (uint64_t)BATCH;
        slot->ops.store(local * ope, std::memory_order_relaxed);
        if (g_stop.load(std::memory_order_relaxed)) break;
    }
    g_sink ^= acc;
}

// ---------------------------------------------------------------------------
// Chunked-stride kernel (chk_stride): divide the thread's buffer into chunks of
// `chunk` elements; do `reps` full strided (stride `ssz`) sweeps of each chunk
// before advancing to the next. Models cache reuse of a working-set window.
// ---------------------------------------------------------------------------
template <int OP>
static void kernel_chk_stride(uint64_t* base, uint64_t n, uint64_t ssz,
                              uint64_t chunk, uint64_t reps, Slot* slot,
                              uint64_t seed) {
    constexpr uint64_t ope = ops_per_elem(OP);
    constexpr uint64_t PUB = 1ull << 20; // publish/check every ~1M elements
    if (ssz < 1) ssz = 1;
    if (chunk < 1) chunk = 1;
    if (reps < 1) reps = 1;
    uint64_t acc = seed;
    uint64_t local = 0;
    uint64_t since_pub = 0;
    // As in kernel_strided, the innermost per-element loop is pristine (a tight
    // scalar do_touch over one pointer induction variable) and the publish/stop
    // check is hoisted to the per-offset loop, firing on an element-count
    // threshold. Per offset within a chunk: ceil((clen-off)/s) elements, i.e.
    // q+1 for the first rr offsets, q afterwards (q=clen/s, rr=clen%s).
    for (;;) {
        for (uint64_t cs = 0; cs < n; cs += chunk) {
            uint64_t clen = (n - cs < chunk) ? (n - cs) : chunk;
            uint64_t s = ssz > clen ? clen : ssz;
            uint64_t* cb = base + cs;
            uint64_t* cend = cb + clen;
            uint64_t q = clen / s;
            uint64_t rr = clen % s;
            for (uint64_t r = 0; r < reps; r++) {
                for (uint64_t off = 0; off < s; off++) {
                    for (uint64_t* p = cb + off; p < cend; p += s)
                        do_touch<OP>(p, acc);
                    uint64_t cnt = q + (off < rr);
                    local += cnt;
                    since_pub += cnt;
                    if (since_pub >= PUB) {
                        slot->ops.store(local * ope,
                                        std::memory_order_relaxed);
                        since_pub = 0;
                        if (g_stop.load(std::memory_order_relaxed))
                            goto done;
                    }
                }
            }
        }
    }
done:
    slot->ops.store(local * ope, std::memory_order_relaxed);
    g_sink ^= acc;
}

// Uniform kernel signature for dispatch.
//   p0 = stride (stride/stride_pf/chk_stride) ; p1 = prefetch distance
//   (stride_pf) or chunk elems (chk_stride) ; p2 = reps (chk_stride).
//   Unused params are ignored per kernel.
using KernelFn = void (*)(uint64_t*, uint64_t, uint64_t, uint64_t, uint64_t,
                          Slot*, uint64_t);

template <int OP>
static void run_strided(uint64_t* base, uint64_t n, uint64_t L, uint64_t,
                        uint64_t, Slot* slot, uint64_t seed) {
    kernel_strided<OP>(base, n, L, slot, seed);
}
template <int OP>
static void run_strided_pf(uint64_t* base, uint64_t n, uint64_t L, uint64_t pd,
                           uint64_t, Slot* slot, uint64_t seed) {
    kernel_strided_pf<OP>(base, n, L, pd, slot, seed);
}
template <int OP>
static void run_random(uint64_t* base, uint64_t n, uint64_t, uint64_t,
                       uint64_t, Slot* slot, uint64_t seed) {
    kernel_random<OP>(base, n, slot, seed);
}
template <int OP>
static void run_chk_stride(uint64_t* base, uint64_t n, uint64_t ssz,
                           uint64_t chunk, uint64_t reps, Slot* slot,
                           uint64_t seed) {
    kernel_chk_stride<OP>(base, n, ssz, chunk, reps, slot, seed);
}

static KernelFn pick_kernel(Order ord, Op op) {
    switch (ord) {
        case ORD_RAND:
            switch (op) {
                case OP_READ:  return run_random<OP_READ>;
                case OP_WRITE: return run_random<OP_WRITE>;
                case OP_RW:    return run_random<OP_RW>;
                case OP_WR:    return run_random<OP_WR>;
            }
            break;
        case ORD_CHK_STRIDE:
            switch (op) {
                case OP_READ:  return run_chk_stride<OP_READ>;
                case OP_WRITE: return run_chk_stride<OP_WRITE>;
                case OP_RW:    return run_chk_stride<OP_RW>;
                case OP_WR:    return run_chk_stride<OP_WR>;
            }
            break;
        case ORD_STRIDE_PF:
            switch (op) {
                case OP_READ:  return run_strided_pf<OP_READ>;
                case OP_WRITE: return run_strided_pf<OP_WRITE>;
                case OP_RW:    return run_strided_pf<OP_RW>;
                case OP_WR:    return run_strided_pf<OP_WR>;
            }
            break;
        case ORD_STRIDE:
        case ORD_STRIDEP:
            switch (op) {
                case OP_READ:  return run_strided<OP_READ>;
                case OP_WRITE: return run_strided<OP_WRITE>;
                case OP_RW:    return run_strided<OP_RW>;
                case OP_WR:    return run_strided<OP_WR>;
            }
            break;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Bench configuration / runtime state.
// ---------------------------------------------------------------------------
struct Bench {
    std::string name;
    Order order;
    Op    op;
    int   threads  = 1;
    uint64_t ssz   = 8;   // stride / stride_pf / chk_stride: elements
    uint64_t pd    = 16;  // stride_pf: future strided accesses
    uint64_t np    = 16;  // stridep: partitions
    uint64_t X     = 1ull << 20; // chk_stride: chunk size in bytes
    uint64_t Y     = 20;  // chk_stride: reuse count
    uint64_t size  = 256ull << 20; // bytes (will be rounded)
    int   bufnode  = -1;
    int   cpunode  = -1;
    std::vector<int> cpulist; // explicit per-bench core list (overrides cpunode)
    // computed at setup:
    uint64_t* buf  = nullptr;
    uint64_t  epp  = 0;   // elements per thread
    uint64_t  L    = 1;   // effective per-thread stride (elements)
    uint64_t  chk_elems = 0; // chk_stride: chunk size in elements
    Slot*  slots   = nullptr;
    KernelFn fn    = nullptr;
    std::vector<int> cores; // assigned core per thread (-1 = unpinned)
};

// ---------------------------------------------------------------------------
// Errors / parsing helpers.
// ---------------------------------------------------------------------------
[[noreturn]] static void die(const std::string& msg) {
    fprintf(stderr, "mbench: error: %s\n", msg.c_str());
    exit(1);
}

static uint64_t parse_size(const std::string& s) {
    if (s.empty()) die("empty size");
    size_t i = 0;
    uint64_t val = 0;
    bool any = false;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
        val = val * 10 + (uint64_t)(s[i] - '0');
        i++;
        any = true;
    }
    if (!any) die("bad size: " + s);
    uint64_t mult = 1;
    if (i < s.size()) {
        char c = (char)toupper((unsigned char)s[i]);
        if (c == 'K') mult = 1ull << 10;
        else if (c == 'M') mult = 1ull << 20;
        else if (c == 'G') mult = 1ull << 30;
        else die("bad size suffix in: " + s);
        i++;
    }
    if (i != s.size()) die("trailing chars in size: " + s);
    return val * mult;
}

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(d, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// Explicit CPU list for a bench, e.g. "0-3", "0+2+4", "0-3+8+10-11". The bench
// spec is split on ':' (fields) and ',' (benches), so neither may appear here;
// tokens are '+'-separated and each is a single core "N" or a range "A-B".
static std::vector<int> parse_cpulist(const std::string& s) {
    std::vector<int> out;
    for (const std::string& tok : split(s, '+')) {
        if (tok.empty()) continue;
        size_t dash = tok.find('-');
        if (dash == std::string::npos) {
            out.push_back(atoi(tok.c_str()));
        } else {
            int lo = atoi(tok.substr(0, dash).c_str());
            int hi = atoi(tok.substr(dash + 1).c_str());
            if (hi < lo) die("bad cpus range '" + tok + "' (hi < lo)");
            for (int c = lo; c <= hi; c++) out.push_back(c);
        }
    }
    if (out.empty()) die("empty cpus list");
    return out;
}

static bool parse_name(const std::string& name, Order& ord, Op& op) {
    // Ops never contain '_', so split on the LAST '_' (orders like chk_stride
    // contain one).
    size_t u = name.rfind('_');
    if (u == std::string::npos) return false;
    std::string o = name.substr(0, u);
    std::string p = name.substr(u + 1);
    if (o == "stride") ord = ORD_STRIDE;
    else if (o == "stride_pf") ord = ORD_STRIDE_PF;
    else if (o == "stridep") ord = ORD_STRIDEP;
    else if (o == "chk_stride") ord = ORD_CHK_STRIDE;
    else if (o == "rand") ord = ORD_RAND;
    else return false;
    if (p == "read") op = OP_READ;
    else if (p == "write") op = OP_WRITE;
    else if (p == "rw") op = OP_RW;
    else if (p == "wr") op = OP_WR;
    else return false;
    return true;
}

static Bench parse_bench(const std::string& spec) {
    std::vector<std::string> f = split(spec, ':');
    if (f.size() < 2) die("bench spec needs name:threads  (got '" + spec + "')");
    Bench b;
    b.name = f[0];
    if (!parse_name(b.name, b.order, b.op))
        die("unknown bench name '" + b.name +
            "' (order=stride|stride_pf|stridep|chk_stride|rand, "
            "op=read|write|rw|wr)");
    b.threads = atoi(f[1].c_str());
    if (b.threads < 1) die("threads must be >= 1 in '" + spec + "'");
    for (size_t i = 2; i < f.size(); i++) {
        if (f[i].empty()) continue;
        size_t eq = f[i].find('=');
        if (eq == std::string::npos) die("option needs key=value: '" + f[i] + "'");
        std::string k = f[i].substr(0, eq);
        std::string v = f[i].substr(eq + 1);
        if (k == "ssz")        b.ssz = strtoull(v.c_str(), nullptr, 10);
        else if (k == "pd")    b.pd = strtoull(v.c_str(), nullptr, 10);
        else if (k == "np")    b.np = strtoull(v.c_str(), nullptr, 10);
        else if (k == "x")     b.X = parse_size(v);
        else if (k == "y")     b.Y = strtoull(v.c_str(), nullptr, 10);
        else if (k == "size")  b.size = parse_size(v);
        else if (k == "bufnode") b.bufnode = atoi(v.c_str());
        else if (k == "cpunode") b.cpunode = atoi(v.c_str());
        else if (k == "cpus")    b.cpulist = parse_cpulist(v);
        else die("unknown option '" + k + "' in '" + spec + "'");
    }
    if (b.ssz < 1) b.ssz = 1;
    if (b.np < 1) b.np = 1;
    if (b.Y < 1) b.Y = 1;
    if (b.X < 8) b.X = 8;
    return b;
}

// ---------------------------------------------------------------------------
// NUMA helpers.
// ---------------------------------------------------------------------------
static std::vector<int> node_cpus(int node) {
    std::vector<int> v;
    struct bitmask* mask = numa_allocate_cpumask();
    if (numa_node_to_cpus(node, mask) != 0) {
        numa_free_cpumask(mask);
        die("numa_node_to_cpus failed for node " + std::to_string(node));
    }
    for (unsigned i = 0; i < mask->size; i++)
        if (numa_bitmask_isbitset(mask, i)) v.push_back((int)i);
    numa_free_cpumask(mask);
    if (v.empty()) die("node " + std::to_string(node) + " has no CPUs");
    return v;
}

// Allocate a page-aligned buffer; optionally bind it to a NUMA node, fault it,
// then restore the default memory policy for the range.
static uint64_t* alloc_buf(uint64_t size, int bufnode) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap failed");
    if (bufnode >= 0) {
        numa_tonode_memory(p, size, bufnode); // mbind(MPOL_BIND, bufnode)
        memset(p, 0, size);                   // fault pages onto the bound node
        if (mbind(p, size, MPOL_DEFAULT, nullptr, 0, 0) != 0)
            die("mbind(MPOL_DEFAULT) restore failed");
    }
    return (uint64_t*)p;
}

// ---------------------------------------------------------------------------
// NUMA placement monitor: an optional helper thread that periodically queries,
// via move_pages(nodes=NULL), which NUMA node each bench buffer's pages reside
// on, tallies them per node, and appends one CSV row per bench per sample.
// Read-only observation -- it never migrates pages and never touches the
// kernels, so the scalar-64-bit / pristine-loop invariants are unaffected.
// ---------------------------------------------------------------------------
static int numa_node_count() {
    if (numa_available() < 0) return 1;
    int m = numa_max_node();
    return m < 0 ? 1 : m + 1;
}

static void numa_monitor(const std::vector<Bench>* benches, FILE* f,
                         int interval, int nnodes,
                         std::chrono::steady_clock::time_point t0) {
    using clk = std::chrono::steady_clock;
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    // Cap the move_pages batch so scratch stays bounded for multi-GB buffers.
    uint64_t maxpages = 1;
    for (const Bench& b : *benches) {
        uint64_t p = (b.size + page - 1) / page;
        if (p > maxpages) maxpages = p;
    }
    const uint64_t CHUNK_CAP = 1ull << 19; // 512Ki pages (4 MiB addr + 2 MiB status)
    size_t chunk = (size_t)(maxpages < CHUNK_CAP ? maxpages : CHUNK_CAP);
    std::vector<void*> addrs(chunk);
    std::vector<int>   status(chunk);
    std::vector<uint64_t> bins(nnodes + 1, 0); // [0..nnodes-1]=nodes, [nnodes]=unknown

    while (!g_stop.load(std::memory_order_relaxed)) {
        // Wait `interval` seconds, but wake within 100ms of a stop request so
        // shutdown isn't delayed by up to a full interval.
        for (int i = 0; i < interval * 10 &&
                        !g_stop.load(std::memory_order_relaxed); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_stop.load(std::memory_order_relaxed)) break;

        double tsec = std::chrono::duration<double>(clk::now() - t0).count();
        for (const Bench& b : *benches) {
            for (uint64_t& c : bins) c = 0;
            const uint64_t total = (b.size + page - 1) / page;
            char* basep = (char*)b.buf;
            uint64_t done = 0;
            while (done < total) {
                uint64_t rem = total - done;
                size_t cnt = (size_t)(rem < (uint64_t)chunk ? rem : chunk);
                for (size_t i = 0; i < cnt; i++)
                    addrs[i] = basep + (done + i) * page;
                long rc = move_pages(0, cnt, addrs.data(), nullptr,
                                     status.data(), 0);
                if (rc < 0) {
                    bins[nnodes] += cnt; // query failed: whole batch unknown
                } else {
                    for (size_t i = 0; i < cnt; i++) {
                        int s = status[i];
                        if (s >= 0 && s < nnodes) bins[s]++;
                        else bins[nnodes]++; // not present / error / out of range
                    }
                }
                done += cnt;
            }
            double mib = (double)total * (double)page / (1024.0 * 1024.0);
            fprintf(f, "%.1f,%s,%llu,%.1f", tsec, b.name.c_str(),
                    (unsigned long long)total, mib);
            for (int n = 0; n < nnodes; n++)
                fprintf(f, ",%llu", (unsigned long long)bins[n]);
            fprintf(f, ",%llu\n", (unsigned long long)bins[nnodes]);
        }
        fflush(f);
    }
}

// ---------------------------------------------------------------------------
// Worker.
// ---------------------------------------------------------------------------
struct ThreadArg {
    uint64_t* base;      // kernel access range base
    uint64_t  n;         // kernel access range elements
    uint64_t* warm_base; // first-touch/warm-up range base (per-thread slice)
    uint64_t  warm_n;    // first-touch/warm-up range elements
    uint64_t  L;     // stride (stride/stride_pf/chk_stride)
    uint64_t  p1;    // stride_pf: prefetch distance; chk_stride: chunk elems
    uint64_t  p2;    // chk_stride: reps
    Slot*     slot;
    uint64_t  seed;
    KernelFn  fn;
    int       core;  // -1 => unpinned
};

static void worker(ThreadArg a) {
    if (a.core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(a.core, &set);
        sched_setaffinity(0, sizeof(set), &set);
    }
    // Warm-up: first-touch this thread's own slice (even page distribution for
    // the unbound NUMA case) and warm caches/TLB. The warm-up range is always
    // the per-thread partition even when the kernel access range is wider -- the
    // rand kernel accesses the whole shared buffer, but each thread still faults
    // only its 1/threads slice here so pages stay evenly spread and the buffer
    // isn't zeroed once per thread.
    memset(a.warm_base, 0, a.warm_n * sizeof(uint64_t));

    g_ready.fetch_add(1, std::memory_order_acq_rel);
    while (!g_go.load(std::memory_order_acquire)) cpu_relax();

    a.fn(a.base, a.n, a.L, a.p1, a.p2, a.slot, a.seed);
}

// ---------------------------------------------------------------------------
// Misc.
// ---------------------------------------------------------------------------
static const char* order_str(Order o) {
    switch (o) {
        case ORD_STRIDE:     return "stride";
        case ORD_STRIDE_PF:  return "stride_pf";
        case ORD_STRIDEP:    return "stridep";
        case ORD_CHK_STRIDE: return "chk_stride";
        case ORD_RAND:       return "rand";
    }
    return "?";
}

static uint64_t sum_ops(const Bench& b) {
    uint64_t s = 0;
    for (int t = 0; t < b.threads; t++)
        s += b.slots[t].ops.load(std::memory_order_relaxed);
    return s;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --benches \"<spec>[,<spec>...]\" [--time SECONDS]"
        " [--numa-csv=FILE[:S]]\n"
        "\n"
        "  spec = name:threads[:key=value ...]\n"
        "  name = <order>_<op>\n"
        "    order = stride | stride_pf | stridep | chk_stride | rand\n"
        "    op    = read | write | rw | wr\n"
        "  keys:\n"
        "    ssz=N      stride in uint64_t        (stride/stride_pf/chk_stride,\n"
        "                                          default 8)\n"
        "    pd=N       future strided accesses   (stride_pf only, default 16)\n"
        "    np=N       partition count           (stridep only,  default 16)\n"
        "    x=N        chunk size, bytes K/M/G   (chk_stride only, default 1M)\n"
        "    y=N        chunk reuse count         (chk_stride only, default 20)\n"
        "    size=N     buffer bytes, K/M/G ok     (default 256M)\n"
        "    bufnode=N  bind buffer to NUMA node   (default: none)\n"
        "    cpunode=N  pin threads to node's CPUs (default: none)\n"
        "    cpus=LIST  pin threads to an explicit core list, '+'-separated\n"
        "               cores/ranges e.g. cpus=0-3 or cpus=0+2+4 (overrides\n"
        "               cpunode; round-robin if threads > cores)\n"
        "\n"
        "  --numa-csv=FILE[:S]  log per-bench buffer NUMA placement (resident\n"
        "                       pages per node) to FILE as CSV every S seconds\n"
        "                       (default 3) via move_pages()\n"
        "\n"
        "Example:\n"
        "  %s --benches \"stride_read:4:ssz=8:size=512M:bufnode=0:cpunode=0,"
        "rand_write:6:size=1G:bufnode=1:cpunode=1\" --time 20\n",
        prog, prog);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string benches_str;
    int run_time = 10;
    std::string numa_csv_arg;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "--benches" || a == "-b") && i + 1 < argc) {
            benches_str = argv[++i];
        } else if (a.rfind("--benches=", 0) == 0) {
            benches_str = a.substr(strlen("--benches="));
        } else if ((a == "--time" || a == "-t") && i + 1 < argc) {
            run_time = atoi(argv[++i]);
        } else if (a.rfind("--time=", 0) == 0) {
            run_time = atoi(a.substr(strlen("--time=")).c_str());
        } else if (a == "--numa-csv" && i + 1 < argc) {
            numa_csv_arg = argv[++i];
        } else if (a.rfind("--numa-csv=", 0) == 0) {
            numa_csv_arg = a.substr(strlen("--numa-csv="));
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a[0] != '-' && benches_str.empty()) {
            benches_str = a; // positional
        } else {
            usage(argv[0]);
            die("unknown argument: " + a);
        }
    }
    if (benches_str.empty()) {
        usage(argv[0]);
        die("no benches specified");
    }
    if (run_time < 1) die("--time must be >= 1");

    // NUMA placement monitor: --numa-csv=FILE[:interval]. Split on the LAST ':'
    // and treat the suffix as the interval only if it is all digits, so file
    // paths that happen to contain ':' still parse as a path.
    bool numa_on = !numa_csv_arg.empty();
    std::string numa_path;
    int numa_interval = 3;
    if (numa_on) {
        numa_path = numa_csv_arg;
        size_t c = numa_csv_arg.rfind(':');
        if (c != std::string::npos && c + 1 < numa_csv_arg.size()) {
            std::string suf = numa_csv_arg.substr(c + 1);
            bool digits = true;
            for (char ch : suf)
                if (!isdigit((unsigned char)ch)) digits = false;
            if (digits) {
                numa_interval = atoi(suf.c_str());
                numa_path = numa_csv_arg.substr(0, c);
            }
        }
        if (numa_path.empty()) die("--numa-csv needs a file path");
        if (numa_interval < 1) die("--numa-csv interval must be >= 1");
    }

    // Parse benches.
    std::vector<Bench> benches;
    for (const std::string& s : split(benches_str, ','))
        if (!s.empty()) benches.push_back(parse_bench(s));
    if (benches.empty()) die("no benches specified");

    // NUMA availability check if any bench uses it.
    bool needs_numa = false;
    for (const Bench& b : benches)
        if (b.bufnode >= 0 || b.cpunode >= 0) needs_numa = true;
    if (needs_numa && numa_available() < 0)
        die("NUMA requested but libnuma reports NUMA is unavailable");

    // Set up each bench: round size, allocate buffer, compute layout, pin map.
    printf("=== mbench: %zu bench(es), run time %ds ===\n",
           benches.size(), run_time);
    for (Bench& b : benches) {
        // Round size down to a multiple of threads*64 (even split, 64B aligned).
        uint64_t gran = (uint64_t)b.threads * 64ull;
        b.size = (b.size / gran) * gran;
        if (b.size == 0)
            die("buffer too small for " + std::to_string(b.threads) +
                " threads in bench " + b.name);
        b.epp = (b.size / (uint64_t)b.threads) / sizeof(uint64_t);
        if (b.epp == 0) die("zero elements per thread in bench " + b.name);

        // Effective stride.
        if (b.order == ORD_STRIDE || b.order == ORD_STRIDE_PF ||
            b.order == ORD_CHK_STRIDE)
            b.L = b.ssz;
        else if (b.order == ORD_STRIDEP)
            b.L = (b.epp + b.np - 1) / b.np; // ceil(epp / np)
        else
            b.L = 1; // unused for rand
        if (b.L < 1) b.L = 1;
        if (b.L > b.epp) b.L = b.epp;

        // chk_stride: chunk size in elements (capped to the thread's buffer).
        if (b.order == ORD_CHK_STRIDE) {
            b.chk_elems = b.X / sizeof(uint64_t);
            if (b.chk_elems < 1) b.chk_elems = 1;
            if (b.chk_elems > b.epp) b.chk_elems = b.epp;
        }

        b.buf = alloc_buf(b.size, b.bufnode);
        b.slots = new Slot[b.threads]();
        b.fn = pick_kernel(b.order, b.op);

        // CPU pin map. An explicit cpus= list takes priority over cpunode; both
        // assign core[t] round-robin over the chosen set.
        b.cores.assign(b.threads, -1);
        if (!b.cpulist.empty()) {
            for (int t = 0; t < b.threads; t++)
                b.cores[t] = b.cpulist[(size_t)t % b.cpulist.size()];
        } else if (b.cpunode >= 0) {
            std::vector<int> cpus = node_cpus(b.cpunode);
            for (int t = 0; t < b.threads; t++)
                b.cores[t] = cpus[(size_t)t % cpus.size()];
        }

        // Print bench info.
        uint64_t chunk = b.epp * sizeof(uint64_t);
        printf("[%s] order=%s op=%d threads=%d\n",
               b.name.c_str(), order_str(b.order), (int)b.op, b.threads);
        printf("    buffer: start=%p size=%llu B (%.1f MiB) chunk/thread=%llu B "
               "elems/thread=%llu\n",
               (void*)b.buf, (unsigned long long)b.size,
               b.size / (1024.0 * 1024.0),
               (unsigned long long)chunk, (unsigned long long)b.epp);
        if (b.order == ORD_STRIDE)
            printf("    stride: ssz=%llu elems (%llu B)\n",
                   (unsigned long long)b.L, (unsigned long long)(b.L * 8));
        else if (b.order == ORD_STRIDE_PF)
            printf("    stride_pf: ssz=%llu elems pd=%llu accesses "
                   "(%llu B ahead)\n",
                   (unsigned long long)b.L, (unsigned long long)b.pd,
                   (unsigned long long)(b.pd * b.L * 8));
        else if (b.order == ORD_STRIDEP)
            printf("    stridep: np=%llu -> stride=%llu elems (%llu B)\n",
                   (unsigned long long)b.np, (unsigned long long)b.L,
                   (unsigned long long)(b.L * 8));
        else if (b.order == ORD_CHK_STRIDE)
            printf("    chk_stride: ssz=%llu elems chunk=%llu B (%llu elems) "
                   "reuse=%llu\n",
                   (unsigned long long)b.L,
                   (unsigned long long)(b.chk_elems * 8),
                   (unsigned long long)b.chk_elems, (unsigned long long)b.Y);
        else if (b.order == ORD_RAND)
            printf("    rand: shared buffer across %d thread(s) "
                   "(%llu elems, %llu B total)\n",
                   b.threads,
                   (unsigned long long)(b.epp * (uint64_t)b.threads),
                   (unsigned long long)(b.epp * (uint64_t)b.threads * 8));
        printf("    bufnode=%s cpunode=%s cores=[",
               b.bufnode >= 0 ? std::to_string(b.bufnode).c_str() : "default",
               b.cpunode >= 0 ? std::to_string(b.cpunode).c_str() : "none");
        if (!b.cores.empty() && b.cores[0] >= 0) {
            for (int t = 0; t < b.threads; t++)
                printf("%s%d", t ? "," : "", b.cores[t]);
        } else {
            printf("unpinned");
        }
        printf("]\n");
    }

    // Open the NUMA placement CSV (if requested) and write its header.
    FILE* numa_f = nullptr;
    int numa_nodes = 0;
    if (numa_on) {
        numa_f = fopen(numa_path.c_str(), "w");
        if (!numa_f) die("cannot open numa-csv file '" + numa_path + "'");
        numa_nodes = numa_node_count();
        fprintf(numa_f, "time_s,bench,total_pages,total_MiB");
        for (int n = 0; n < numa_nodes; n++) fprintf(numa_f, ",node%d", n);
        fprintf(numa_f, ",unknown\n");
        fflush(numa_f);
        printf("numa monitor: %s every %ds (%d node%s)\n",
               numa_path.c_str(), numa_interval, numa_nodes,
               numa_nodes == 1 ? "" : "s");
    }

    int total_threads = 0;
    for (const Bench& b : benches) total_threads += b.threads;

    // Spawn workers.
    std::vector<std::thread> pool;
    pool.reserve(total_threads);
    uint64_t gtid = 0;
    for (Bench& b : benches) {
        for (int t = 0; t < b.threads; t++) {
            ThreadArg a;
            // Warm-up always covers this thread's own slice (first-touch).
            a.warm_base = b.buf + (uint64_t)t * b.epp;
            a.warm_n = b.epp;
            if (b.order == ORD_RAND) {
                // rand is not partitioned: every thread issues random accesses
                // over the whole shared buffer.
                a.base = b.buf;
                a.n = b.epp * (uint64_t)b.threads;
            } else {
                a.base = a.warm_base;
                a.n = a.warm_n;
            }
            a.L = b.L;
            a.p1 = b.order == ORD_STRIDE_PF ? b.pd : b.chk_elems;
            a.p2 = b.Y;
            a.slot = &b.slots[t];
            a.seed = STEP * (gtid + 1) ^ 0xD1B54A32D192ED03ull;
            if (a.seed == 0) a.seed = 1;
            a.fn = b.fn;
            a.core = b.cores[t];
            pool.emplace_back(worker, a);
            gtid++;
        }
    }

    // Barrier: wait for all warm-ups, then release and start timing.
    while (g_ready.load(std::memory_order_acquire) < total_threads) cpu_relax();
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    g_go.store(true, std::memory_order_release);

    std::thread numa_thread;
    if (numa_on)
        numa_thread = std::thread(numa_monitor, &benches, numa_f, numa_interval,
                                  numa_nodes, t0);

    printf("--- running ---\n");
    std::vector<uint64_t> last(benches.size(), 0);
    auto t_prev = t0;
    for (int s = 0; s < run_time; s++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = clk::now();
        double dt = std::chrono::duration<double>(now - t_prev).count();
        t_prev = now;
        double elapsed = std::chrono::duration<double>(now - t0).count();
        for (size_t bi = 0; bi < benches.size(); bi++) {
            uint64_t cur = sum_ops(benches[bi]);
            uint64_t d = cur - last[bi];
            last[bi] = cur;
            double mops = d / dt / 1e6;
            double gbps = (double)d * 8.0 / dt / 1e9;
            printf("[%6.1fs] %-16s %10.2f Mops/s  %8.2f GB/s\n",
                   elapsed, benches[bi].name.c_str(), mops, gbps);
        }
    }

    g_stop.store(true, std::memory_order_relaxed);
    for (std::thread& th : pool) th.join();
    if (numa_on) {
        numa_thread.join();
        fclose(numa_f);
    }
    double total_elapsed =
        std::chrono::duration<double>(clk::now() - t0).count();

    printf("--- totals (%.2fs) ---\n", total_elapsed);
    for (const Bench& b : benches) {
        uint64_t total = sum_ops(b);
        double mops = total / total_elapsed / 1e6;
        double gbps = (double)total * 8.0 / total_elapsed / 1e9;
        printf("[%-16s] %llu ops  %10.2f Mops/s avg  %8.2f GB/s avg\n",
               b.name.c_str(), (unsigned long long)total, mops, gbps);
    }

    // Cleanup.
    for (Bench& b : benches) {
        munmap(b.buf, b.size);
        delete[] b.slots;
    }
    return (int)(g_sink & 0); // always 0; keeps g_sink live
}
