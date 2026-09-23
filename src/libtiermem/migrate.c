/*
 * migrate.c — Migration thread pool + move_pages()
 *
 * Dedicated worker threads migrate objects between NUMA nodes.
 * Called at the end of each epoch by the monitor thread.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <numaif.h>
#include <sys/syscall.h>

#include "migrate.h"
#include "config.h"
#include "hook.h"
#include "log.h"
#include "thread.h"
#include "monitor.h"

migrate_pool_t g_migrate_pool;

static void destroy_migrate_pool(void)
{
    if (g_migrate_pool.queue) {
        real_free(g_migrate_pool.queue);
        g_migrate_pool.queue = NULL;
    }

    pthread_mutex_destroy(&g_migrate_pool.mutex);
    pthread_cond_destroy(&g_migrate_pool.work_avail);
    pthread_cond_destroy(&g_migrate_pool.all_done);
}

static void stop_migrate_workers(void)
{
    pthread_mutex_lock(&g_migrate_pool.mutex);
    g_migrate_pool.shutdown = true;
    pthread_cond_broadcast(&g_migrate_pool.work_avail);
    pthread_mutex_unlock(&g_migrate_pool.mutex);

    for (int i = 0; i < g_migrate_pool.count; i++)
        pthread_join(g_migrate_pool.threads[i], NULL);
}

/* ── move_pages wrapper with batching ── */

static void migrate_object(mem_object_t *obj, int target_node)
{
    size_t total_pages = obj->nr_pages;
    size_t done = 0;

    /* Stack-allocated arrays to avoid heap allocation while app threads
     * are paused (which could deadlock on the malloc arena lock). */
    void  *pages[MIGRATE_BATCH_MAX];
    int    nodes[MIGRATE_BATCH_MAX];
    int    status[MIGRATE_BATCH_MAX];

    while (done < total_pages) {
        size_t batch = total_pages - done;
        if (batch > MIGRATE_BATCH_MAX)
            batch = MIGRATE_BATCH_MAX;

        for (size_t i = 0; i < batch; i++) {
            pages[i] = (void *)(obj->start_addr + (done + i) * TM_PAGE_SIZE);
            nodes[i] = target_node;
        }

        long ret = syscall(__NR_move_pages, 0, batch, pages, nodes, status,
                           MPOL_MF_MOVE);
        if (ret < 0) {
            TM_DBG("move_pages failed for obj %#lx batch at offset %zu: %s",
                   obj->start_addr, done, strerror(errno));
            g_migrate_pool.migration_errors++;
        }

        /* Count successful page migrations */
        for (size_t i = 0; i < batch; i++) {
            if (status[i] == target_node) {
                if (target_node == LOCAL_NODE)
                    g_migrate_pool.pages_promoted++;
                else
                    g_migrate_pool.pages_demoted++;
            }
        }

        done += batch;
    }

    obj->current_node = target_node;
    obj->migration_pending = false;
    g_migrate_pool.migrations_total++;

    TM_DBG("Migrated obj %#lx (%zu pages) to node %d",
           obj->start_addr, total_pages, target_node);
}

/* ── Worker thread ── */

static void *migrate_worker(void *arg)
{
    int worker_id = (int)(intptr_t)arg;

    /* Pin to dedicated CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(g_config.migrate_cpu_start + worker_id, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);

    TM_DBG("Migration worker %d started (cpu=%d)",
           worker_id, g_config.migrate_cpu_start + worker_id);

    while (1) {
        pthread_mutex_lock(&g_migrate_pool.mutex);

        while (g_migrate_pool.queue_head == g_migrate_pool.queue_tail &&
               !g_migrate_pool.shutdown)
            pthread_cond_wait(&g_migrate_pool.work_avail,
                              &g_migrate_pool.mutex);

        if (g_migrate_pool.shutdown) {
            pthread_mutex_unlock(&g_migrate_pool.mutex);
            break;
        }

        /* Dequeue */
        struct migrate_job job =
            g_migrate_pool.queue[g_migrate_pool.queue_head];
        g_migrate_pool.queue_head =
            (g_migrate_pool.queue_head + 1) % g_migrate_pool.queue_len;
        pthread_mutex_unlock(&g_migrate_pool.mutex);

        /* Execute */
        migrate_object(job.obj, job.target_node);

        /* Signal completion */
        pthread_mutex_lock(&g_migrate_pool.mutex);
        if (--g_migrate_pool.jobs_remaining == 0)
            pthread_cond_signal(&g_migrate_pool.all_done);
        pthread_mutex_unlock(&g_migrate_pool.mutex);
    }

    TM_DBG("Migration worker %d exiting", worker_id);
    return NULL;
}

/* ── Init / Cleanup ── */

int migrate_init(void)
{
    memset(&g_migrate_pool, 0, sizeof(g_migrate_pool));

    int nthreads = g_config.migrate_threads;
    if (nthreads > MIGRATE_POOL_MAX)
        nthreads = MIGRATE_POOL_MAX;

    g_migrate_pool.count = nthreads;
    g_migrate_pool.queue_len = MIGRATE_QUEUE_MAX;
    g_migrate_pool.queue = real_calloc(MIGRATE_QUEUE_MAX,
                                       sizeof(struct migrate_job));
    if (!g_migrate_pool.queue)
        return -1;

    pthread_mutex_init(&g_migrate_pool.mutex, NULL);
    pthread_cond_init(&g_migrate_pool.work_avail, NULL);
    pthread_cond_init(&g_migrate_pool.all_done, NULL);

    /* Create worker threads (not registered in thread registry) */
    for (int i = 0; i < nthreads; i++) {
        int ret = real_pthread_create(&g_migrate_pool.threads[i], NULL,
                                      migrate_worker, (void *)(intptr_t)i);
        if (ret != 0) {
            TM_ERR("Failed to create migration worker %d: %s", i, strerror(ret));
            g_migrate_pool.count = i;
            stop_migrate_workers();
            destroy_migrate_pool();
            return -1;
        }
        char tname[16];
        snprintf(tname, sizeof(tname), "tm:migrate/%d", i);
        pthread_setname_np(g_migrate_pool.threads[i], tname);
    }

    TM_DBG("Migration pool initialized (%d workers)", nthreads);
    return 0;
}

void migrate_cleanup(void)
{
    stop_migrate_workers();
    destroy_migrate_pool();

    TM_DBG("Migration pool cleaned up");
}

/* ── Execute pending migrations (called from monitor thread) ── */

struct pending_ctx {
    struct migrate_job *jobs;
    int count;
    int capacity;
};

static void collect_pending(mem_object_t *obj, void *ctx)
{
    struct pending_ctx *p = (struct pending_ctx *)ctx;
    if (obj->migration_pending && p->count < p->capacity) {
        p->jobs[p->count].obj = obj;
        p->jobs[p->count].target_node = obj->target_node;
        p->count++;
    }
}

/* ── Multi-process: migrate only objects belonging to this process ── */

static void collect_own_pending(mem_object_t *obj, void *ctx)
{
    struct pending_ctx *p = (struct pending_ctx *)ctx;
    if (obj->migration_pending && obj->pid == getpid() &&
        p->count < p->capacity) {
        p->jobs[p->count].obj = obj;
        p->jobs[p->count].target_node = obj->target_node;
        p->count++;
    }
}

static void execute_pending_jobs(struct pending_ctx *pctx, bool own_only,
                                 bool disable_monitoring)
{
    if (pctx->count == 0) {
        real_free(pctx->jobs);
        return;
    }

    if (own_only)
        TM_EPOCH("Process %d migrating %d objects...", getpid(), pctx->count);
    else
        TM_EPOCH("Migrating %d objects...", pctx->count);

    if (disable_monitoring)
        monitor_disable_events();

    /* Pause only the relevant application threads before queueing migrations. */
    if (pause_all_threads() != 0) {
        TM_ERR("Failed to pause threads, skipping migration");
        resume_all_threads();  /* release any threads that did pause */
        if (disable_monitoring)
            monitor_enable_events();
        real_free(pctx->jobs);
        return;
    }

    pthread_mutex_lock(&g_migrate_pool.mutex);
    for (int i = 0; i < pctx->count; i++) {
        int tail = g_migrate_pool.queue_tail;
        g_migrate_pool.queue[tail] = pctx->jobs[i];
        g_migrate_pool.queue_tail = (tail + 1) % g_migrate_pool.queue_len;
    }
    g_migrate_pool.jobs_remaining = pctx->count;
    pthread_cond_broadcast(&g_migrate_pool.work_avail);
    pthread_mutex_unlock(&g_migrate_pool.mutex);

    pthread_mutex_lock(&g_migrate_pool.mutex);
    while (g_migrate_pool.jobs_remaining > 0)
        pthread_cond_wait(&g_migrate_pool.all_done, &g_migrate_pool.mutex);
    pthread_mutex_unlock(&g_migrate_pool.mutex);

    resume_all_threads();
    if (disable_monitoring)
        monitor_enable_events();
    real_free(pctx->jobs);

    if (own_only)
        TM_EPOCH("Process %d migration complete", getpid());
    else
        TM_EPOCH("Migration complete");
}

void migrate_execute_pending(void)
{
    struct pending_ctx pctx;
    pctx.capacity = MIGRATE_QUEUE_MAX;
    pctx.count = 0;
    pctx.jobs = real_malloc(pctx.capacity * sizeof(struct migrate_job));
    if (!pctx.jobs) return;

    obj_store_foreach(collect_pending, &pctx);
    execute_pending_jobs(&pctx, false, true);
}

void migrate_execute_own_pending(void)
{
    struct pending_ctx pctx;
    pctx.capacity = MIGRATE_QUEUE_MAX;
    pctx.count = 0;
    pctx.jobs = real_malloc(pctx.capacity * sizeof(struct migrate_job));
    if (!pctx.jobs) return;

    obj_store_foreach(collect_own_pending, &pctx);
    execute_pending_jobs(&pctx, true, false);
}

/* ── Part II stubs ── */

double probe_batch(mem_object_t *obj, size_t page_offset, size_t nr_pages)
{
    (void)obj; (void)page_offset; (void)nr_pages;
    return -1.0;
}

int identify_borderline_objects(mem_object_t **out, int max_out,
                                double threshold, double delta)
{
    (void)out; (void)max_out; (void)threshold; (void)delta;
    return 0;
}

void tournament_probe_step(void) {}
