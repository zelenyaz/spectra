#ifndef TIERMEM_OBJ_STORE_H
#define TIERMEM_OBJ_STORE_H

#include "config.h"
#include "regularity.h"
#include <pthread.h>
#include <sys/types.h>

/* A tracked memory object (one large allocation) */
typedef struct mem_object {
    pid_t       pid;
    uintptr_t   start_addr;
    size_t      size;
    size_t      nr_pages;

    /* Monitoring state (updated each epoch) */

    /* PEBS per-epoch raw counters */
    uint64_t    load_sample_count;
    uint64_t    store_sample_count;
    uint64_t    store_lat_sum;

    /* MEM_INST_RETIRED.ALL_LOADS PEBS attribution (per-epoch):
     * - allload_sample_count = total samples landed on this object
     * - pf_sample_count      = subset whose IP decoded to a SW prefetch opcode
     * Normal-load samples = allload_sample_count - pf_sample_count. */
    uint64_t    allload_sample_count;
    uint64_t    pf_sample_count;

    /* Streaming regularity state. Load regularity is measured from
     * non-prefetch MEM_INST_RETIRED.ALL_LOADS PEBS samples. */
    uintptr_t   last_load_addr;
    uintptr_t   last_store_addr;
    uint32_t    load_stride_bins[REG_NBINS];
    uint32_t    store_stride_bins[REG_NBINS];

    /* Epoch-end derived bandwidth (intermediate per-object terms are
     * computed and consumed inside compute_per_object_bw_new and not stored
     * on the object). */
    double      rd_bw;
    double      wr_bw;
    double      bw;

    /* EMA-smoothed bandwidth used for ranking/hysteresis decisions.
     * Persists across epochs (not reset in obj_store_epoch_reset). Seeded
     * from the first non-zero `bw` observation; thereafter blended as
     * bw_ema = alpha * bw + (1 - alpha) * bw_ema. Dampens the placement-bias
     * swing in L3-miss-based BW measurements. */
    double      bw_ema;

    /* Congestion controller: applied interleave level k in [0, G]. 0 = fully
     * local. Persists across epochs (not cleared by obj_store_epoch_reset). */
    uint32_t    cc_ilv_k;

    /* Placement state */
    int         current_node;
    int         target_node;
    bool        migration_pending;

    /* Metadata */
    int         alloc_type;
    uint64_t    alloc_epoch;
} mem_object_t;

#define obj_end_addr(obj) ((obj)->start_addr + (obj)->size)

/*
 * Per-process object list.
 *
 * In single-process mode: uses dynamically allocated pointer array (objs_dyn)
 * and linked-list hash chain (hash_next pointer).
 *
 * In multi-process mode: uses embedded fixed array (objs_shm[PROC_OBJ_CAPACITY])
 * and slab-index hash chain (next_idx).
 */
typedef struct proc_obj_list {
    pid_t           pid;
    int             count;

    /* Multi-process mode: slab-index hash chain, -1 = end */
    int             next_idx;

    /* Single-process mode: pointer-based hash chain */
    struct proc_obj_list *hash_next;

    /* Object pointers — layout depends on mode */
    union {
        struct {
            /* Single-process: dynamically allocated */
            mem_object_t  **objs;
            int             capacity;
        } dyn;
        /* Multi-process: embedded fixed array */
        mem_object_t   *objs_shm[PROC_OBJ_CAPACITY];
    };
} proc_obj_list_t;

/* Global object store (single-process mode) */
typedef struct {
    proc_obj_list_t *buckets[OBJ_STORE_PID_BUCKETS];
    pthread_rwlock_t lock;
} obj_store_t;

extern obj_store_t g_obj_store;   /* used only in single-process mode */

int           obj_store_init(void);
int           obj_store_attach(void);   /* multi-process joiner */
void          obj_store_cleanup(void);
mem_object_t *obj_store_insert(pid_t pid, uintptr_t addr, size_t size, int alloc_type);
void          obj_store_remove(pid_t pid, uintptr_t addr);
mem_object_t *obj_store_lookup(pid_t pid, uintptr_t addr);
void          obj_store_foreach(void (*fn)(mem_object_t *obj, void *ctx), void *ctx);
void          obj_store_epoch_reset(void);
int           obj_store_total_count(void);

/* Multi-process: remove all objects belonging to a specific PID */
void          obj_store_remove_pid(pid_t pid);

#endif /* TIERMEM_OBJ_STORE_H */
