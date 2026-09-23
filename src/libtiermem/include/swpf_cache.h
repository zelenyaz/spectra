#ifndef TIERMEM_SWPF_CACHE_H
#define TIERMEM_SWPF_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Returns true iff the instruction at (pid, ip) decodes to an x86 software
 * prefetch (PREFETCHNTA / PREFETCHT0 / PREFETCHT1 / PREFETCHT2 / PREFETCHW
 * / PREFETCHWT1). On read errors or undecodable bytes, returns false.
 *
 * Implementation uses a fixed-size open-addressed cache keyed by (pid, ip);
 * cache misses fall through to process_vm_readv() + opcode decode and
 * insert the verdict (positive OR negative) back into the cache so both
 * outcomes avoid future reads. The cache is monitor-thread-private, so no
 * locking is required (callers are the monitor / follower threads only). */
bool swpf_is_prefetch(pid_t pid, uint64_t ip);

/* Diagnostics: cache hit / miss / probe stats. May be called at epoch end. */
struct swpf_cache_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t read_failures;
    uint64_t pf_verdicts;
    uint64_t non_pf_verdicts;
};
void swpf_cache_get_stats(struct swpf_cache_stats *out);
void swpf_cache_reset_stats(void);

#endif /* TIERMEM_SWPF_CACHE_H */
