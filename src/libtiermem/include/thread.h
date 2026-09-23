#ifndef TIERMEM_THREAD_H
#define TIERMEM_THREAD_H

#include "config.h"
#include <sys/types.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct thread_entry {
    pid_t               tid;
    atomic_int          paused_ack;
    struct thread_entry *prev;
    struct thread_entry *next;
} thread_entry_t;

typedef struct {
    thread_entry_t *head;
    thread_entry_t *tail;
    int             count;
    pthread_mutex_t mutex;
} thread_registry_t;

extern thread_registry_t g_thread_registry;
extern atomic_int g_world_state;
extern atomic_int g_futex_word;

/* Thread-local pointer to own registry entry (for signal handler) */
extern __thread thread_entry_t *tl_my_entry;

int  thread_mgmt_init(void);
void thread_mgmt_cleanup(void);
int  thread_register(pid_t tid);
void thread_unregister(pid_t tid);
int  pause_all_threads(void);
int  resume_all_threads(void);

#endif /* TIERMEM_THREAD_H */
