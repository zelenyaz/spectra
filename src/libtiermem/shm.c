/*
 * shm.c — Shared memory region, slab allocator, process registry, epoch sync
 *
 * Manages a POSIX shared memory region mapped at a fixed virtual address
 * so that pointers within the region are valid in all attached processes.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <stdatomic.h>

#include "shm.h"
#include "config.h"
#include "hook.h"
#include "log.h"
#include "obj_store.h"

/* ── Per-process state ── */
shm_header_t *g_shm          = NULL;
int            g_shm_creator  = 0;
static int     shm_fd         = -1;

/* ── Helpers ── */

static inline void *shm_base(void)
{
    return (void *)g_shm;
}

static inline void *shm_at_offset(size_t off)
{
    return (char *)shm_base() + off;
}

static inline slab_header_t *obj_slab(void)
{
    return (slab_header_t *)shm_at_offset(g_shm->obj_slab_offset);
}

static inline slab_header_t *proc_slab(void)
{
    return (slab_header_t *)shm_at_offset(g_shm->proc_slab_offset);
}

/* ── Futex ── */

void shm_futex_wait(_Atomic int *addr, int expected)
{
    syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

void shm_futex_wake(_Atomic int *addr, int count)
{
    syscall(SYS_futex, addr, FUTEX_WAKE, count, NULL, NULL, 0);
}

/* ── Slab allocator ── */

static void slab_init(slab_header_t *slab, uint32_t capacity, uint32_t slot_size)
{
    slab->capacity  = capacity;
    slab->slot_size = slot_size;
    atomic_store(&slab->free_head, 0);

    /* Build free list: each slot's first 4 bytes = next free index */
    char *slots = (char *)(slab + 1);
    for (uint32_t i = 0; i < capacity - 1; i++) {
        uint32_t next = i + 1;
        memcpy(slots + (size_t)i * slot_size, &next, sizeof(next));
    }
    /* Last slot: UINT32_MAX = end of list */
    uint32_t end = UINT32_MAX;
    memcpy(slots + (size_t)(capacity - 1) * slot_size, &end, sizeof(end));
}

static void *slab_alloc(slab_header_t *slab)
{
    char *slots = (char *)(slab + 1);

    while (1) {
        uint32_t head = atomic_load_explicit(&slab->free_head,
                                             memory_order_acquire);
        if (head == UINT32_MAX)
            return NULL;  /* slab full */

        uint32_t next;
        memcpy(&next, slots + (size_t)head * slab->slot_size, sizeof(next));

        if (atomic_compare_exchange_weak_explicit(
                &slab->free_head, &head, next,
                memory_order_acq_rel, memory_order_acquire)) {
            void *slot = slots + (size_t)head * slab->slot_size;
            memset(slot, 0, slab->slot_size);
            return slot;
        }
        /* CAS failed, retry */
    }
}

static void slab_free(slab_header_t *slab, void *ptr)
{
    char *slots = (char *)(slab + 1);
    size_t offset = (char *)ptr - slots;
    uint32_t idx = (uint32_t)(offset / slab->slot_size);

    while (1) {
        uint32_t head = atomic_load_explicit(&slab->free_head,
                                             memory_order_acquire);
        memcpy(ptr, &head, sizeof(head));

        if (atomic_compare_exchange_weak_explicit(
                &slab->free_head, &head, idx,
                memory_order_acq_rel, memory_order_acquire))
            return;
    }
}

static int slab_idx(slab_header_t *slab, void *ptr)
{
    char *slots = (char *)(slab + 1);
    return (int)((char *)ptr - slots) / (int)slab->slot_size;
}

static void *slab_at(slab_header_t *slab, int idx)
{
    if (idx < 0 || (uint32_t)idx >= slab->capacity)
        return NULL;
    char *slots = (char *)(slab + 1);
    return slots + (size_t)idx * slab->slot_size;
}

/* ── Shared memory lifecycle ── */

/*
 * Compute layout offsets and total required size.
 */
static size_t compute_layout(size_t *obj_slab_off, size_t *proc_slab_off)
{
    size_t off = 0;

    /* Header — align to 4096 */
    off += sizeof(shm_header_t);
    off = (off + 4095) & ~(size_t)4095;

    /* Object slab */
    *obj_slab_off = off;
    off += sizeof(slab_header_t) + (size_t)MAX_OBJECTS * sizeof(mem_object_t);
    off = (off + 4095) & ~(size_t)4095;

    /* Proc list slab */
    *proc_slab_off = off;
    off += sizeof(slab_header_t) + (size_t)MAX_PROC_LISTS * sizeof(proc_obj_list_t);
    off = (off + 4095) & ~(size_t)4095;

    return off;
}

static int init_process_shared_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    int ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return ret;
}

static int init_process_shared_rwlock(pthread_rwlock_t *rw)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    int ret = pthread_rwlock_init(rw, &attr);
    pthread_rwlockattr_destroy(&attr);
    return ret;
}

int shm_create_or_attach(void)
{
    size_t obj_off, proc_off;
    size_t required = compute_layout(&obj_off, &proc_off);

    size_t map_size = g_config.shm_size;
    if (map_size < required) {
        TM_INFO("SHM size %zu too small (need %zu), adjusting", map_size, required);
        map_size = required;
    }

    /* Try to create exclusively */
    int fd = shm_open(g_config.shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        /* We are the creator */
        g_shm_creator = 1;

        if (ftruncate(fd, (off_t)map_size) != 0) {
            TM_ERR("ftruncate shm failed: %s", strerror(errno));
            close(fd);
            shm_unlink(g_config.shm_name);
            return -1;
        }

        void *base = mmap((void *)SHM_BASE_ADDR, map_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (base == MAP_FAILED) {
            TM_ERR("mmap shm at %#lx failed: %s",
                   (unsigned long)SHM_BASE_ADDR, strerror(errno));
            close(fd);
            shm_unlink(g_config.shm_name);
            return -1;
        }

        shm_fd = fd;
        g_shm = (shm_header_t *)base;
        memset(g_shm, 0, map_size);

        /* Init header */
        g_shm->magic   = SHM_MAGIC;
        g_shm->version = SHM_VERSION;
        atomic_store(&g_shm->refcount, 1);
        atomic_store(&g_shm->leader_pid, getpid());
        atomic_store(&g_shm->proc_count, 0);
        g_shm->obj_slab_offset  = obj_off;
        g_shm->proc_slab_offset = proc_off;
        g_shm->shm_size         = map_size;

        /* Init synchronization primitives */
        init_process_shared_mutex(&g_shm->proc_mutex);
        init_process_shared_rwlock(&g_shm->store_lock);

        /* Init buckets to -1 (empty) */
        for (int i = 0; i < OBJ_STORE_PID_BUCKETS; i++)
            g_shm->store_buckets[i] = -1;

        /* Init epoch sync */
        atomic_store(&g_shm->epoch.phase, PHASE_SAMPLING);
        atomic_store(&g_shm->epoch.barrier_count, 0);
        atomic_store(&g_shm->epoch.barrier_target, 0);
        atomic_store(&g_shm->epoch.migrate_done, 0);
        atomic_store(&g_shm->epoch.futex_word, 0);

        /* Init slab pools */
        slab_init(obj_slab(), MAX_OBJECTS, sizeof(mem_object_t));
        slab_init(proc_slab(), MAX_PROC_LISTS, sizeof(proc_obj_list_t));

        TM_INFO("SHM created: name=%s size=%zu base=%p",
                g_config.shm_name, map_size, base);
        return 1;

    } else if (errno == EEXIST) {
        /* Attach to existing */
        g_shm_creator = 0;

        fd = shm_open(g_config.shm_name, O_RDWR, 0600);
        if (fd < 0) {
            TM_ERR("shm_open attach failed: %s", strerror(errno));
            return -1;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            TM_ERR("fstat shm failed: %s", strerror(errno));
            close(fd);
            return -1;
        }

        void *base = mmap((void *)SHM_BASE_ADDR, (size_t)st.st_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (base == MAP_FAILED) {
            TM_ERR("mmap shm attach at %#lx failed: %s",
                   (unsigned long)SHM_BASE_ADDR, strerror(errno));
            close(fd);
            return -1;
        }

        shm_fd = fd;
        g_shm = (shm_header_t *)base;

        /* Validate */
        if (g_shm->magic != SHM_MAGIC || g_shm->version != SHM_VERSION) {
            TM_ERR("SHM magic/version mismatch");
            munmap(base, (size_t)st.st_size);
            close(fd);
            g_shm = NULL;
            return -1;
        }

        atomic_fetch_add(&g_shm->refcount, 1);

        TM_INFO("SHM attached: name=%s size=%zu base=%p refcount=%d",
                g_config.shm_name, (size_t)st.st_size, base,
                atomic_load(&g_shm->refcount));
        return 0;

    } else {
        TM_ERR("shm_open failed: %s", strerror(errno));
        return -1;
    }
}

void shm_detach(void)
{
    if (!g_shm) return;

    shm_unregister_proc(getpid());

    int remaining = atomic_fetch_sub(&g_shm->refcount, 1) - 1;

    /* If we were leader and others remain, elect new leader */
    if (atomic_load(&g_shm->leader_pid) == getpid() && remaining > 0)
        shm_elect_new_leader();

    size_t size = g_shm->shm_size;
    munmap(g_shm, size);
    g_shm = NULL;

    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }

    if (remaining <= 0)
        shm_unlink(g_config.shm_name);
}

/* ── Object slab ── */

mem_object_t *shm_alloc_object(void)
{
    return (mem_object_t *)slab_alloc(obj_slab());
}

void shm_free_object(mem_object_t *obj)
{
    slab_free(obj_slab(), obj);
}

/* ── Proc list slab ── */

proc_obj_list_t *shm_alloc_proc_list(void)
{
    return (proc_obj_list_t *)slab_alloc(proc_slab());
}

void shm_free_proc_list(proc_obj_list_t *p)
{
    slab_free(proc_slab(), p);
}

int shm_proc_list_idx(proc_obj_list_t *p)
{
    return slab_idx(proc_slab(), p);
}

proc_obj_list_t *shm_proc_list_at(int idx)
{
    return (proc_obj_list_t *)slab_at(proc_slab(), idx);
}

/* ── Process registry ── */

static int lock_proc_mutex(void)
{
    int ret = pthread_mutex_lock(&g_shm->proc_mutex);
    if (ret == EOWNERDEAD) {
        /* Previous holder died — recover */
        pthread_mutex_consistent(&g_shm->proc_mutex);
        return 0;
    }
    return ret;
}

void shm_register_proc(pid_t pid)
{
    lock_proc_mutex();

    int count = atomic_load(&g_shm->proc_count);
    if (count < MAX_PROCS) {
        g_shm->proc_pids[count] = pid;
        atomic_store(&g_shm->proc_count, count + 1);
        atomic_store(&g_shm->epoch.barrier_target, count + 1);
    } else {
        TM_ERR("SHM process registry full (max=%d)", MAX_PROCS);
    }

    pthread_mutex_unlock(&g_shm->proc_mutex);

    TM_INFO("Process %d registered (total=%d)", pid,
            atomic_load(&g_shm->proc_count));
}

void shm_unregister_proc(pid_t pid)
{
    lock_proc_mutex();

    int count = atomic_load(&g_shm->proc_count);
    for (int i = 0; i < count; i++) {
        if (g_shm->proc_pids[i] == pid) {
            /* Shift down */
            for (int j = i; j < count - 1; j++)
                g_shm->proc_pids[j] = g_shm->proc_pids[j + 1];
            g_shm->proc_pids[count - 1] = 0;
            atomic_store(&g_shm->proc_count, count - 1);
            atomic_store(&g_shm->epoch.barrier_target, count - 1);
            break;
        }
    }

    pthread_mutex_unlock(&g_shm->proc_mutex);
}

bool shm_is_leader(void)
{
    return g_shm && atomic_load(&g_shm->leader_pid) == getpid();
}

pid_t shm_get_leader(void)
{
    return g_shm ? atomic_load(&g_shm->leader_pid) : 0;
}

void shm_elect_new_leader(void)
{
    lock_proc_mutex();

    pid_t my_pid = getpid();
    pid_t best = 0;
    int count = atomic_load(&g_shm->proc_count);

    for (int i = 0; i < count; i++) {
        pid_t p = g_shm->proc_pids[i];
        if (p != my_pid && p > 0) {
            if (best == 0 || p < best)
                best = p;
        }
    }

    if (best > 0) {
        atomic_store(&g_shm->leader_pid, best);
        TM_INFO("New leader elected: PID %d", best);
    }

    pthread_mutex_unlock(&g_shm->proc_mutex);
}

void shm_check_dead_procs(void)
{
    lock_proc_mutex();

    int count = atomic_load(&g_shm->proc_count);
    int i = 0;
    while (i < count) {
        pid_t p = g_shm->proc_pids[i];
        if (p > 0 && kill(p, 0) != 0 && errno == ESRCH) {
            TM_INFO("Dead process detected: PID %d, removing", p);
            /* Free tracked objects for the dead process */
            obj_store_remove_pid(p);
            /* Remove from registry */
            for (int j = i; j < count - 1; j++)
                g_shm->proc_pids[j] = g_shm->proc_pids[j + 1];
            g_shm->proc_pids[count - 1] = 0;
            count--;
            atomic_store(&g_shm->proc_count, count);
            atomic_store(&g_shm->epoch.barrier_target, count);
            /* Don't increment i — check the shifted entry */
        } else {
            i++;
        }
    }

    pthread_mutex_unlock(&g_shm->proc_mutex);
}

/* ── Epoch sync accessor ── */

epoch_sync_t *shm_epoch_sync(void)
{
    return g_shm ? &g_shm->epoch : NULL;
}

int shm_claim_ocr_slot(pid_t pid)
{
    if (!g_shm) return -1;
    int slot = atomic_fetch_add_explicit(&g_shm->proc_ocr_count, 1,
                                          memory_order_acq_rel);
    if (slot >= MAX_PROCS) {
        atomic_fetch_sub_explicit(&g_shm->proc_ocr_count, 1,
                                   memory_order_relaxed);
        return -1;
    }
    g_shm->proc_ocr[slot].pid = pid;
    atomic_store_explicit(&g_shm->proc_ocr[slot].delta_demand_rfo,     0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].en_demand_rfo,        0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].ru_demand_rfo,        0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].delta_hw_data_pf,     0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].en_hw_data_pf,        0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].ru_hw_data_pf,        0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].delta_hw_rfo_pf,      0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].en_hw_rfo_pf,         0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->proc_ocr[slot].ru_hw_rfo_pf,         0, memory_order_relaxed);
    return slot;
}

int shm_find_ocr_slot(pid_t pid)
{
    if (!g_shm) return -1;
    int n = atomic_load_explicit(&g_shm->proc_ocr_count, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (g_shm->proc_ocr[i].pid == pid)
            return i;
    }
    return -1;
}
