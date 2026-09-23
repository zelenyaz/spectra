#ifndef TIERMEM_MIGRATE_H
#define TIERMEM_MIGRATE_H

#include "config.h"
#include "obj_store.h"
#include <pthread.h>

/* Migration job */
struct migrate_job {
    mem_object_t *obj;
    int           target_node;
};

/* Migration worker pool */
typedef struct {
    pthread_t       threads[MIGRATE_POOL_MAX];
    int             count;

    struct migrate_job *queue;
    int             queue_len;
    int             queue_head;
    int             queue_tail;

    pthread_mutex_t mutex;
    pthread_cond_t  work_avail;
    pthread_cond_t  all_done;
    int             jobs_remaining;
    bool            shutdown;

    /* Stats */
    uint64_t        pages_promoted;
    uint64_t        pages_demoted;
    uint64_t        migrations_total;
    uint64_t        migration_errors;
} migrate_pool_t;

extern migrate_pool_t g_migrate_pool;

int  migrate_init(void);
void migrate_cleanup(void);
void migrate_execute_pending(void);

/* Multi-process: migrate only objects belonging to this process */
void migrate_execute_own_pending(void);

/* Part II stubs */
double probe_batch(mem_object_t *obj, size_t page_offset, size_t nr_pages);
int    identify_borderline_objects(mem_object_t **out, int max_out,
                                   double threshold, double delta);
void   tournament_probe_step(void);

#endif /* TIERMEM_MIGRATE_H */
