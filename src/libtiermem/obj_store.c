/*
 * obj_store.c — Object tracking with per-pid sorted arrays
 *
 * Supports two modes:
 *   - Single-process (default): heap-allocated, process-local
 *   - Multi-process (TIERMEM_MULTI_PROC=1): slab-allocated in shared memory,
 *     index-based hash chains, process-shared rwlock
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "obj_store.h"
#include "hook.h"
#include "log.h"
#include "shm.h"

/* ── Helpers ── */

static inline int is_multi_proc(void)
{
    return g_config.multi_proc && g_shm != NULL;
}

/* Lock/unlock that work for both modes */
static inline void store_wrlock(void)
{
    if (is_multi_proc())
        pthread_rwlock_wrlock(&g_shm->store_lock);
    else
        pthread_rwlock_wrlock(&g_obj_store.lock);
}

static inline void store_rdlock(void)
{
    if (is_multi_proc())
        pthread_rwlock_rdlock(&g_shm->store_lock);
    else
        pthread_rwlock_rdlock(&g_obj_store.lock);
}

static inline void store_unlock(void)
{
    if (is_multi_proc())
        pthread_rwlock_unlock(&g_shm->store_lock);
    else
        pthread_rwlock_unlock(&g_obj_store.lock);
}

/* ── Single-process mode globals ── */
obj_store_t g_obj_store;

/* ── Hash ── */
static inline unsigned pid_hash(pid_t pid)
{
    return ((unsigned)pid) % OBJ_STORE_PID_BUCKETS;
}

/* ══════════════════════════════════════════════════════════════════════
 * Object pointer access — abstracts dyn vs shm layout
 * ══════════════════════════════════════════════════════════════════════ */

static inline mem_object_t **proc_objs(proc_obj_list_t *proc)
{
    if (is_multi_proc())
        return proc->objs_shm;
    else
        return proc->dyn.objs;
}

static inline int proc_capacity(proc_obj_list_t *proc)
{
    if (is_multi_proc())
        return PROC_OBJ_CAPACITY;
    else
        return proc->dyn.capacity;
}

/* ══════════════════════════════════════════════════════════════════════
 * Multi-process mode: index-based bucket/chain operations
 * ══════════════════════════════════════════════════════════════════════ */

static proc_obj_list_t *shm_get_or_create_proc(pid_t pid)
{
    unsigned h = pid_hash(pid);
    int idx = g_shm->store_buckets[h];

    while (idx >= 0) {
        proc_obj_list_t *p = shm_proc_list_at(idx);
        if (p && p->pid == pid) return p;
        idx = p ? p->next_idx : -1;
    }

    /* Create new */
    proc_obj_list_t *p = shm_alloc_proc_list();
    if (!p) return NULL;
    p->pid = pid;
    p->count = 0;
    p->next_idx = g_shm->store_buckets[h];
    p->hash_next = NULL;
    g_shm->store_buckets[h] = shm_proc_list_idx(p);
    return p;
}

static proc_obj_list_t *shm_find_proc(pid_t pid)
{
    unsigned h = pid_hash(pid);
    int idx = g_shm->store_buckets[h];

    while (idx >= 0) {
        proc_obj_list_t *p = shm_proc_list_at(idx);
        if (p && p->pid == pid) return p;
        idx = p ? p->next_idx : -1;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * Single-process mode: pointer-based bucket/chain operations
 * ══════════════════════════════════════════════════════════════════════ */

static proc_obj_list_t *sp_get_or_create_proc(pid_t pid)
{
    unsigned h = pid_hash(pid);
    proc_obj_list_t *p = g_obj_store.buckets[h];

    while (p) {
        if (p->pid == pid) return p;
        p = p->hash_next;
    }

    /* Create new */
    p = real_malloc(sizeof(proc_obj_list_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->next_idx = -1;
    p->hash_next = g_obj_store.buckets[h];
    g_obj_store.buckets[h] = p;
    return p;
}

static proc_obj_list_t *sp_find_proc(pid_t pid)
{
    unsigned h = pid_hash(pid);
    proc_obj_list_t *p = g_obj_store.buckets[h];
    while (p) {
        if (p->pid == pid) return p;
        p = p->hash_next;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * Unified operations
 * ══════════════════════════════════════════════════════════════════════ */

static proc_obj_list_t *get_or_create_proc(pid_t pid)
{
    return is_multi_proc() ? shm_get_or_create_proc(pid)
                           : sp_get_or_create_proc(pid);
}

static proc_obj_list_t *find_proc(pid_t pid)
{
    return is_multi_proc() ? shm_find_proc(pid) : sp_find_proc(pid);
}

static mem_object_t *alloc_object(void)
{
    if (is_multi_proc())
        return shm_alloc_object();
    else {
        mem_object_t *obj = real_malloc(sizeof(mem_object_t));
        if (obj) memset(obj, 0, sizeof(*obj));
        return obj;
    }
}

static void free_object(mem_object_t *obj)
{
    if (is_multi_proc())
        shm_free_object(obj);
    else
        real_free(obj);
}

/* ── Binary search ── */

static int bsearch_obj(proc_obj_list_t *proc, uintptr_t addr)
{
    mem_object_t **objs = proc_objs(proc);
    int lo = 0, hi = proc->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        mem_object_t *obj = objs[mid];
        if (addr < obj->start_addr)
            hi = mid - 1;
        else if (addr >= obj_end_addr(obj))
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static int find_insert_pos(proc_obj_list_t *proc, uintptr_t addr)
{
    mem_object_t **objs = proc_objs(proc);
    int lo = 0, hi = proc->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (objs[mid]->start_addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ── Init / Attach ── */

int obj_store_init(void)
{
    if (is_multi_proc()) {
        /* Shared memory already zeroed by shm_create_or_attach;
         * rwlock and buckets initialized there too.
         * Nothing extra to do. */
        return 0;
    }

    /* Single-process mode */
    memset(&g_obj_store, 0, sizeof(g_obj_store));
    if (pthread_rwlock_init(&g_obj_store.lock, NULL) != 0) {
        TM_ERR("Failed to init obj_store rwlock");
        return -1;
    }
    return 0;
}

int obj_store_attach(void)
{
    /* Multi-process joiner: shared structures already exist, nothing to init */
    if (!g_shm) {
        TM_ERR("obj_store_attach: no shared memory");
        return -1;
    }
    return 0;
}

/* ── Insert ── */

mem_object_t *obj_store_insert(pid_t pid, uintptr_t addr, size_t size,
                               int alloc_type)
{
    store_wrlock();

    proc_obj_list_t *proc = get_or_create_proc(pid);
    if (!proc) {
        store_unlock();
        return NULL;
    }

    /* Check capacity */
    int cap = proc_capacity(proc);
    if (!is_multi_proc()) {
        /* Single-process: grow array if needed */
        if (proc->count >= proc->dyn.capacity) {
            int new_cap = proc->dyn.capacity ? proc->dyn.capacity * 2 : 16;
            mem_object_t **new_arr = real_realloc(proc->dyn.objs,
                                                  new_cap * sizeof(mem_object_t *));
            if (!new_arr) {
                store_unlock();
                return NULL;
            }
            proc->dyn.objs = new_arr;
            proc->dyn.capacity = new_cap;
        }
    } else {
        if (proc->count >= cap) {
            TM_ERR("proc_obj_list full for pid=%d (max=%d)", pid, cap);
            store_unlock();
            return NULL;
        }
    }

    /* Allocate object */
    mem_object_t *obj = alloc_object();
    if (!obj) {
        store_unlock();
        return NULL;
    }
    obj->pid = pid;
    obj->start_addr = addr;
    obj->size = size;
    obj->nr_pages = (size + TM_PAGE_SIZE - 1) / TM_PAGE_SIZE;
    obj->alloc_type = alloc_type;
    obj->current_node = -1;

    /* Insert in sorted position */
    mem_object_t **objs = proc_objs(proc);
    int pos = find_insert_pos(proc, addr);
    if (pos < proc->count)
        memmove(&objs[pos + 1], &objs[pos],
                (proc->count - pos) * sizeof(mem_object_t *));
    objs[pos] = obj;
    proc->count++;

    TM_DBG("Object insert: pid=%d addr=%#lx size=%zu (%zu pages) type=%d",
           pid, addr, size, obj->nr_pages, alloc_type);

    store_unlock();
    return obj;
}

/* ── Remove ── */

void obj_store_remove(pid_t pid, uintptr_t addr)
{
    store_wrlock();

    proc_obj_list_t *proc = find_proc(pid);
    if (!proc) {
        store_unlock();
        return;
    }

    mem_object_t **objs = proc_objs(proc);
    for (int i = 0; i < proc->count; i++) {
        if (objs[i]->start_addr == addr) {
            TM_DBG("Object remove: pid=%d addr=%#lx size=%zu",
                   pid, addr, objs[i]->size);
            free_object(objs[i]);
            memmove(&objs[i], &objs[i + 1],
                    (proc->count - i - 1) * sizeof(mem_object_t *));
            proc->count--;
            break;
        }
    }

    store_unlock();
}

/* ── Lookup ── */

mem_object_t *obj_store_lookup(pid_t pid, uintptr_t addr)
{
    store_rdlock();

    proc_obj_list_t *proc = find_proc(pid);
    if (!proc) {
        store_unlock();
        return NULL;
    }

    int idx = bsearch_obj(proc, addr);
    mem_object_t *result = (idx >= 0) ? proc_objs(proc)[idx] : NULL;

    store_unlock();
    return result;
}

/* ── Foreach ── */

/* Iterate helper for multi-process mode (index-based chains) */
static void foreach_shm(void (*fn)(mem_object_t *obj, void *ctx), void *ctx)
{
    for (int b = 0; b < OBJ_STORE_PID_BUCKETS; b++) {
        int idx = g_shm->store_buckets[b];
        while (idx >= 0) {
            proc_obj_list_t *proc = shm_proc_list_at(idx);
            if (!proc) break;
            for (int i = 0; i < proc->count; i++)
                fn(proc->objs_shm[i], ctx);
            idx = proc->next_idx;
        }
    }
}

/* Iterate helper for single-process mode (pointer-based chains) */
static void foreach_sp(void (*fn)(mem_object_t *obj, void *ctx), void *ctx)
{
    for (int b = 0; b < OBJ_STORE_PID_BUCKETS; b++) {
        proc_obj_list_t *proc = g_obj_store.buckets[b];
        while (proc) {
            for (int i = 0; i < proc->count; i++)
                fn(proc->dyn.objs[i], ctx);
            proc = proc->hash_next;
        }
    }
}

void obj_store_foreach(void (*fn)(mem_object_t *obj, void *ctx), void *ctx)
{
    store_rdlock();

    if (is_multi_proc())
        foreach_shm(fn, ctx);
    else
        foreach_sp(fn, ctx);

    store_unlock();
}

/* ── Epoch reset ── */

static void reset_obj(mem_object_t *obj, void *ctx)
{
    (void)ctx;
    /* Per-epoch state */
    obj->load_sample_count  = 0;
    obj->store_sample_count = 0;
    obj->store_lat_sum      = 0;
    obj->allload_sample_count = 0;
    obj->pf_sample_count    = 0;
    obj->last_load_addr     = 0;
    obj->last_store_addr    = 0;
    memset(obj->load_stride_bins,  0, sizeof(obj->load_stride_bins));
    memset(obj->store_stride_bins, 0, sizeof(obj->store_stride_bins));

    obj->rd_bw          = 0.0;
    obj->wr_bw          = 0.0;
    obj->bw             = 0.0;
}

void obj_store_epoch_reset(void)
{
    store_rdlock();

    if (is_multi_proc())
        foreach_shm(reset_obj, NULL);
    else
        foreach_sp(reset_obj, NULL);

    store_unlock();
}

/* ── Total count ── */

int obj_store_total_count(void)
{
    int total = 0;

    store_rdlock();

    if (is_multi_proc()) {
        for (int b = 0; b < OBJ_STORE_PID_BUCKETS; b++) {
            int idx = g_shm->store_buckets[b];
            while (idx >= 0) {
                proc_obj_list_t *proc = shm_proc_list_at(idx);
                if (!proc) break;
                total += proc->count;
                idx = proc->next_idx;
            }
        }
    } else {
        for (int b = 0; b < OBJ_STORE_PID_BUCKETS; b++) {
            proc_obj_list_t *proc = g_obj_store.buckets[b];
            while (proc) {
                total += proc->count;
                proc = proc->hash_next;
            }
        }
    }

    store_unlock();
    return total;
}

/* ── Remove all objects for a PID (multi-process cleanup) ── */

void obj_store_remove_pid(pid_t pid)
{
    store_wrlock();

    if (is_multi_proc()) {
        unsigned h = pid_hash(pid);
        int prev_idx = -1;
        int idx = g_shm->store_buckets[h];

        while (idx >= 0) {
            proc_obj_list_t *proc = shm_proc_list_at(idx);
            if (!proc) break;

            if (proc->pid == pid) {
                /* Free all objects */
                for (int i = 0; i < proc->count; i++)
                    shm_free_object(proc->objs_shm[i]);

                /* Unlink from chain */
                if (prev_idx < 0)
                    g_shm->store_buckets[h] = proc->next_idx;
                else
                    shm_proc_list_at(prev_idx)->next_idx = proc->next_idx;

                shm_free_proc_list(proc);
                break;
            }
            prev_idx = idx;
            idx = proc->next_idx;
        }
    } else {
        unsigned h = pid_hash(pid);
        proc_obj_list_t *prev = NULL;
        proc_obj_list_t *proc = g_obj_store.buckets[h];

        while (proc) {
            if (proc->pid == pid) {
                for (int i = 0; i < proc->count; i++)
                    real_free(proc->dyn.objs[i]);
                if (proc->dyn.objs)
                    real_free(proc->dyn.objs);

                if (prev)
                    prev->hash_next = proc->hash_next;
                else
                    g_obj_store.buckets[h] = proc->hash_next;

                real_free(proc);
                break;
            }
            prev = proc;
            proc = proc->hash_next;
        }
    }

    store_unlock();
}

/* ── Cleanup ── */

void obj_store_cleanup(void)
{
    if (is_multi_proc()) {
        /* In multi-process mode, remove only our own objects.
         * If we're the last process, the shm region will be unlinked. */
        obj_store_remove_pid(getpid());
        return;
    }

    /* Single-process: free everything */
    pthread_rwlock_wrlock(&g_obj_store.lock);

    for (int b = 0; b < OBJ_STORE_PID_BUCKETS; b++) {
        proc_obj_list_t *proc = g_obj_store.buckets[b];
        while (proc) {
            proc_obj_list_t *next = proc->hash_next;
            for (int i = 0; i < proc->count; i++)
                real_free(proc->dyn.objs[i]);
            if (proc->dyn.objs)
                real_free(proc->dyn.objs);
            real_free(proc);
            proc = next;
        }
        g_obj_store.buckets[b] = NULL;
    }

    pthread_rwlock_unlock(&g_obj_store.lock);
    pthread_rwlock_destroy(&g_obj_store.lock);
}
