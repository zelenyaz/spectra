/*
 * thread.c — Thread registry + pause/resume via signal + futex
 *
 * Intercepts pthread_create to register new threads.
 * Uses SIGRTMIN to pause application threads; futex to resume.
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "thread.h"
#include "hook.h"
#include "log.h"
#include "monitor.h"

/* ── Globals ── */
thread_registry_t g_thread_registry = {
    .head = NULL,
    .tail = NULL,
    .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

atomic_int g_world_state = ATOMIC_VAR_INIT(WORLD_STATE_RUNNING);
atomic_int g_futex_word  = ATOMIC_VAR_INIT(0);

__thread thread_entry_t *tl_my_entry = NULL;

/* Original pthread_create */
int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                           void *(*)(void *), void *) = NULL;

static volatile int thread_mgmt_initialized = 0;
static __thread int in_thread_hook = 0;

/* ── Futex helpers ── */
static inline int futex_wait(atomic_int *addr, int expected)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static inline int futex_wake(atomic_int *addr, int count)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, count, NULL, NULL, 0);
}

/* ── Signal handler (must be async-signal-safe) ── */
static void pause_signal_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;

    thread_entry_t *me = tl_my_entry;
    if (!me) return;

    /* Acknowledge pause */
    atomic_store_explicit(&me->paused_ack, 1, memory_order_release);

    /* Wait on futex until world is running again */
    while (atomic_load_explicit(&g_world_state, memory_order_acquire)
           == WORLD_STATE_PAUSED) {
        int val = atomic_load_explicit(&g_futex_word, memory_order_acquire);
        futex_wait(&g_futex_word, val);
    }
}

/* ── Thread registration ── */
int thread_register(pid_t tid)
{
    thread_entry_t *entry = real_malloc(sizeof(thread_entry_t));
    if (!entry) return -1;

    entry->tid = tid;
    atomic_store_explicit(&entry->paused_ack, 0, memory_order_release);
    entry->prev = NULL;
    entry->next = NULL;

    pthread_mutex_lock(&g_thread_registry.mutex);

    if (g_thread_registry.tail == NULL) {
        g_thread_registry.head = entry;
        g_thread_registry.tail = entry;
    } else {
        entry->prev = g_thread_registry.tail;
        g_thread_registry.tail->next = entry;
        g_thread_registry.tail = entry;
    }
    g_thread_registry.count++;

    pthread_mutex_unlock(&g_thread_registry.mutex);

    tl_my_entry = entry;

    TM_DBG("Thread registered: tid=%d (total=%d)", tid, g_thread_registry.count);
    return 0;
}

void thread_unregister(pid_t tid)
{
    pthread_mutex_lock(&g_thread_registry.mutex);

    thread_entry_t *curr = g_thread_registry.head;
    while (curr) {
        if (curr->tid == tid) {
            if (curr->prev) curr->prev->next = curr->next;
            else g_thread_registry.head = curr->next;
            if (curr->next) curr->next->prev = curr->prev;
            else g_thread_registry.tail = curr->prev;
            g_thread_registry.count--;
            real_free(curr);
            break;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&g_thread_registry.mutex);
    tl_my_entry = NULL;
}

/* ── pthread_create wrapper ── */
typedef struct {
    void *(*start_routine)(void *);
    void  *arg;
} thread_wrapper_arg_t;

static void *thread_wrapper(void *arg)
{
    thread_wrapper_arg_t *w = (thread_wrapper_arg_t *)arg;
    void *(*start_routine)(void *) = w->start_routine;
    void *original_arg = w->arg;
    real_free(w);

    pid_t tid = (pid_t)syscall(SYS_gettid);

    /* Ensure pause signal is not blocked */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, THREAD_PAUSE_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

    thread_register(tid);

    void *result = start_routine(original_arg);

    thread_unregister(tid);
    return result;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    if (!thread_mgmt_initialized || in_thread_hook) {
        if (real_pthread_create)
            return real_pthread_create(thread, attr, start_routine, arg);
        return EAGAIN;
    }

    in_thread_hook = 1;

    thread_wrapper_arg_t *w = real_malloc(sizeof(thread_wrapper_arg_t));
    if (!w) {
        in_thread_hook = 0;
        return ENOMEM;
    }
    w->start_routine = start_routine;
    w->arg = arg;

    int ret = real_pthread_create(thread, attr, thread_wrapper, w);
    if (ret != 0)
        real_free(w);

    in_thread_hook = 0;
    return ret;
}

/* ── Init ── */
int init_pthread_hook(void)
{
    if (real_pthread_create) return 0;
    real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    if (!real_pthread_create) {
        TM_ERR("Failed to resolve real pthread_create");
        return -1;
    }
    return 0;
}

int thread_mgmt_init(void)
{
    /* Install signal handler for SIGRTMIN */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = pause_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(THREAD_PAUSE_SIGNAL, &sa, NULL) != 0) {
        TM_ERR("Failed to install pause signal handler: %s", strerror(errno));
        return -1;
    }

    /* Register main thread */
    pid_t tid = (pid_t)syscall(SYS_gettid);
    thread_register(tid);

    thread_mgmt_initialized = 1;
    TM_DBG("Thread management initialized (main tid=%d)", tid);
    return 0;
}

/* ── Pause / Resume ── */
int pause_all_threads(void)
{
    if (!thread_mgmt_initialized) return -1;

    /* Opt-out: leave workload threads running during migration. */
    if (g_config.no_thread_pause) return 0;

    /* Set world state */
    atomic_store_explicit(&g_world_state, WORLD_STATE_PAUSED, memory_order_release);

    /* Reset ack flags */
    pthread_mutex_lock(&g_thread_registry.mutex);
    thread_entry_t *curr = g_thread_registry.head;
    while (curr) {
        atomic_store_explicit(&curr->paused_ack, 0, memory_order_release);
        curr = curr->next;
    }

    /* Send signal to all threads except monitor */
    pid_t my_pid = getpid();
    pid_t monitor_tid = g_monitor.monitor_tid_kernel;

    curr = g_thread_registry.head;
    while (curr) {
        if (curr->tid != monitor_tid) {
            if (syscall(SYS_tgkill, my_pid, curr->tid, THREAD_PAUSE_SIGNAL) != 0) {
                TM_DBG("Failed to send pause signal to tid=%d: %s",
                       curr->tid, strerror(errno));
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_thread_registry.mutex);

    /* Wait for all threads to ack (with 2s timeout) */
    int max_wait_us = 2000000;
    int waited_us = 0;

    while (waited_us < max_wait_us) {
        int all_paused = 1;

        pthread_mutex_lock(&g_thread_registry.mutex);
        curr = g_thread_registry.head;
        while (curr) {
            if (curr->tid != monitor_tid) {
                if (!atomic_load_explicit(&curr->paused_ack, memory_order_acquire)) {
                    all_paused = 0;
                    break;
                }
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&g_thread_registry.mutex);

        if (all_paused) {
            TM_DBG("All threads paused");
            return 0;
        }

        usleep(1000);
        waited_us += 1000;
    }

    TM_ERR("Timeout waiting for threads to pause");
    return -1;
}

int resume_all_threads(void)
{
    if (!thread_mgmt_initialized) return -1;

    /* Opt-out: pause was a no-op, nothing to resume. */
    if (g_config.no_thread_pause) return 0;

    atomic_store_explicit(&g_world_state, WORLD_STATE_RUNNING, memory_order_release);
    atomic_fetch_add_explicit(&g_futex_word, 1, memory_order_release);
    futex_wake(&g_futex_word, INT_MAX);

    TM_DBG("All threads resumed");
    return 0;
}

void thread_mgmt_cleanup(void)
{
    if (!thread_mgmt_initialized) return;

    /* Ensure all threads are running */
    if (atomic_load(&g_world_state) == WORLD_STATE_PAUSED)
        resume_all_threads();

    /* Free registry */
    pthread_mutex_lock(&g_thread_registry.mutex);
    thread_entry_t *curr = g_thread_registry.head;
    while (curr) {
        thread_entry_t *next = curr->next;
        real_free(curr);
        curr = next;
    }
    g_thread_registry.head = NULL;
    g_thread_registry.tail = NULL;
    g_thread_registry.count = 0;
    pthread_mutex_unlock(&g_thread_registry.mutex);

    thread_mgmt_initialized = 0;
}
