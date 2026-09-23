/*
 * swpf_cache.c — Classify a sampled IP as "software prefetch instruction"
 * by reading the instruction bytes via process_vm_readv() and matching
 * x86-64 PREFETCH* opcodes. Cached by (pid, ip).
 *
 * Recognised opcodes (after skipping a single optional REX byte 0x40-0x4F):
 *   0F 18 /0..3   PREFETCHNTA / PREFETCHT0 / PREFETCHT1 / PREFETCHT2
 *   0F 0D /1..2   PREFETCHW   / PREFETCHWT1
 * The ModR/M reg-field selects the variant; only those reg-field values
 * count as software prefetches. Other 0F 18 / 0F 0D variants (NOPs,
 * reserved) and any other opcode are treated as non-prefetch.
 *
 * Why process_vm_readv even on self: avoids signal-handler/setjmp games
 * for unmapped / freed IPs, and keeps the code path uniform.
 */
#include "swpf_cache.h"

#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define CACHE_BITS  14                              /* 16384 entries */
#define CACHE_SIZE  (1u << CACHE_BITS)
#define CACHE_MASK  (CACHE_SIZE - 1)
#define PROBE_LIMIT 4                                /* open-addressing probes */

#define READ_BYTES  8                                /* enough for REX + 0F xx /r disp8 */

struct entry {
    uint64_t key_ip;        /* 0 == empty slot */
    pid_t    pid;
    uint8_t  verdict;       /* 0 = non-pf, 1 = pf */
    uint8_t  valid;
};

static struct entry g_cache[CACHE_SIZE];
static struct swpf_cache_stats g_stats;

static inline uint32_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static inline uint32_t cache_index(pid_t pid, uint64_t ip)
{
    return mix64((uint64_t)pid * 0x9E3779B97F4A7C15ULL ^ ip) & CACHE_MASK;
}

static bool cache_lookup(pid_t pid, uint64_t ip, bool *verdict)
{
    uint32_t h = cache_index(pid, ip);
    for (int i = 0; i < PROBE_LIMIT; i++) {
        struct entry *e = &g_cache[(h + i) & CACHE_MASK];
        if (!e->valid) return false;
        if (e->key_ip == ip && e->pid == pid) {
            *verdict = e->verdict != 0;
            return true;
        }
    }
    return false;
}

static void cache_insert(pid_t pid, uint64_t ip, bool verdict)
{
    uint32_t h = cache_index(pid, ip);
    /* Probe-and-replace: pick first empty slot in window, else h+0. */
    struct entry *target = &g_cache[h];
    for (int i = 0; i < PROBE_LIMIT; i++) {
        struct entry *e = &g_cache[(h + i) & CACHE_MASK];
        if (!e->valid) { target = e; break; }
    }
    target->key_ip = ip;
    target->pid    = pid;
    target->verdict = verdict ? 1 : 0;
    target->valid  = 1;
}

/* Decode whether the bytes at `buf` (up to `len`) form an x86-64 PREFETCH*
 * instruction. Skips one optional REX byte. */
static bool bytes_are_prefetch(const uint8_t *buf, size_t len)
{
    if (len < 3) return false;
    size_t i = 0;
    /* Skip a single REX prefix if present (0x40..0x4F). Other legacy
     * prefixes (segment overrides, 66/F2/F3) don't appear on PREFETCH*. */
    if ((buf[i] & 0xF0) == 0x40) i++;
    if (i + 2 >= len) return false;
    if (buf[i] != 0x0F) return false;
    uint8_t op  = buf[i + 1];
    uint8_t mrm = buf[i + 2];
    uint8_t reg = (mrm >> 3) & 0x7;
    if (op == 0x18) return reg <= 3;          /* PREFETCHNTA/T0/T1/T2 */
    if (op == 0x0D) return reg == 1 || reg == 2; /* PREFETCHW / PREFETCHWT1 */
    return false;
}

static bool read_opcode(pid_t pid, uint64_t ip, uint8_t *out, size_t n)
{
    struct iovec local  = { .iov_base = out, .iov_len = n };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)ip, .iov_len = n };
    ssize_t r = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return r == (ssize_t)n;
}

bool swpf_is_prefetch(pid_t pid, uint64_t ip)
{
    bool v;
    if (cache_lookup(pid, ip, &v)) {
        g_stats.hits++;
        return v;
    }
    g_stats.misses++;

    uint8_t buf[READ_BYTES];
    if (!read_opcode(pid, ip, buf, READ_BYTES)) {
        g_stats.read_failures++;
        cache_insert(pid, ip, false);  /* cache the negative too */
        return false;
    }
    bool is_pf = bytes_are_prefetch(buf, READ_BYTES);
    if (is_pf) g_stats.pf_verdicts++;
    else       g_stats.non_pf_verdicts++;
    cache_insert(pid, ip, is_pf);
    return is_pf;
}

void swpf_cache_get_stats(struct swpf_cache_stats *out)
{
    *out = g_stats;
}

void swpf_cache_reset_stats(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}
