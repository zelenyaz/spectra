#ifndef TIERMEM_SHM_H
#define TIERMEM_SHM_H

#include "config.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

/* ── Epoch synchronization ── */
typedef struct {
    _Atomic int  phase;           /* PHASE_SAMPLING / PHASE_MIGRATING / PHASE_DONE */
    _Atomic int  barrier_count;   /* processes that reached the barrier */
    _Atomic int  barrier_target;  /* active process count */
    _Atomic int  migrate_done;    /* processes that finished migrating */
    _Atomic int  futex_word;      /* futex for epoch phase transitions */

    /* Congestion controller decision, published by the leader each epoch before
     * PHASE_MIGRATING. When cc_owns is set, every process interleaves its own
     * local objects instead of running the base bandwidth-knapsack migration.
     * Demotion is bandwidth-proportional: each object spills to interleave level
     * k_i = cc_k_for_object(cc_r, cc_g, bw_ema/nr_pages, cc_density_max), so the
     * leader publishes the global intensity cc_r and the global density
     * reference cc_density_max rather than a single shared level. */
    _Atomic int    cc_owns;
    _Atomic int    cc_g;
    _Atomic double cc_r;            /* controller intensity, in [0,1]          */
    _Atomic double cc_density_max;  /* max bw_ema/nr_pages over local objects   */
} epoch_sync_t;

/* ── Slab pool (lock-free free list) ── */
typedef struct {
    _Atomic uint32_t free_head;   /* index of first free slot, UINT32_MAX = empty */
    uint32_t capacity;
    uint32_t slot_size;
    /* slots[] follows immediately (allocated inline in shm region) */
} slab_header_t;

/* ── Shared memory header ── */
typedef struct {
    uint32_t        magic;
    uint32_t        version;
    _Atomic int     refcount;
    _Atomic pid_t   leader_pid;

    /* Process registry */
    pid_t           proc_pids[MAX_PROCS];
    _Atomic int     proc_count;
    pthread_mutex_t proc_mutex;       /* PTHREAD_PROCESS_SHARED */

    /* Per-process self-reported OCR accumulators.
     * Each process claims a fixed slot via proc_ocr_count and writes its own
     * OCR deltas there.  The leader reads and zeros them at epoch boundaries
     * via atomic_exchange.  Slots are never shifted (unlike proc_pids[]). */
    struct {
        pid_t            pid;                 /* owning pid (set once) */

        /* 4-event fields (delta + per-event enabled/running). */
        _Atomic uint64_t delta_demand_data_rd, en_demand_data_rd, ru_demand_data_rd;
        _Atomic uint64_t delta_demand_rfo,     en_demand_rfo,     ru_demand_rfo;
        _Atomic uint64_t delta_hw_data_pf,     en_hw_data_pf,     ru_hw_data_pf;
        _Atomic uint64_t delta_hw_rfo_pf,      en_hw_rfo_pf,      ru_hw_rfo_pf;
    } proc_ocr[MAX_PROCS];
    _Atomic int         proc_ocr_count;

    /* Epoch synchronization */
    epoch_sync_t    epoch;

    /* Object store (shared rwlock + bucket array of slab indices) */
    pthread_rwlock_t store_lock;      /* PTHREAD_PROCESS_SHARED */
    int              store_buckets[OBJ_STORE_PID_BUCKETS]; /* -1 = empty */

    /* Slab pool offsets (relative to shm base) */
    size_t          obj_slab_offset;
    size_t          proc_slab_offset;

    /* Total shm region size */
    size_t          shm_size;
} shm_header_t;

/* ── Module-level state (per-process) ── */
extern shm_header_t *g_shm;          /* pointer to shared region */
extern int            g_shm_creator;  /* 1 if this process created the shm */

/* ── Lifecycle ── */
int   shm_create_or_attach(void);     /* returns 1=creator, 0=joiner, -1=error */
void  shm_detach(void);

/* ── Slab allocator (forward decls — actual types in obj_store.h) ── */
struct mem_object;
struct proc_obj_list;

struct mem_object    *shm_alloc_object(void);
void                  shm_free_object(struct mem_object *obj);
struct proc_obj_list *shm_alloc_proc_list(void);
void                  shm_free_proc_list(struct proc_obj_list *p);

/* ── Slab index helpers ── */
int                   shm_proc_list_idx(struct proc_obj_list *p);
struct proc_obj_list *shm_proc_list_at(int idx);

/* ── Process registry ── */
void  shm_register_proc(pid_t pid);
void  shm_unregister_proc(pid_t pid);
bool  shm_is_leader(void);
pid_t shm_get_leader(void);
void  shm_elect_new_leader(void);
void  shm_check_dead_procs(void);     /* called by leader each epoch */

/* ── Per-process OCR slot ── */
int  shm_claim_ocr_slot(pid_t pid);     /* returns slot index, or -1 on full */
int  shm_find_ocr_slot(pid_t pid);      /* returns slot index, or -1 if not found */

/* ── Epoch sync accessor ── */
epoch_sync_t *shm_epoch_sync(void);

/* ── Futex helpers (shared memory) ── */
void shm_futex_wait(_Atomic int *addr, int expected);
void shm_futex_wake(_Atomic int *addr, int count);

#endif /* TIERMEM_SHM_H */
