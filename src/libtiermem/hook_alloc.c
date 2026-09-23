/*
 * hook_alloc.c — Allocation function interception
 *
 * Intercepts malloc/free/calloc/realloc/posix_memalign/mmap/munmap
 * to track large objects in the object store.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "config.h"
#include "hook.h"
#include "log.h"
#include "obj_store.h"

/* ── Original function pointers ── */
void *(*real_malloc)(size_t)                              = NULL;
void  (*real_free)(void *)                                = NULL;
void *(*real_calloc)(size_t, size_t)                      = NULL;
void *(*real_realloc)(void *, size_t)                     = NULL;
int   (*real_posix_memalign)(void **, size_t, size_t)     = NULL;
void *(*real_mmap)(void *, size_t, int, int, int, off_t)  = NULL;
int   (*real_munmap)(void *, size_t)                      = NULL;

/* ── Thread-local recursion guard ── */
__thread int in_hook = 0;

/* ── Three-state init flag ── */
volatile int hooks_initialized = 0;

/* ── Bootstrap buffer for allocations during dlsym ── */
static char bootstrap_buf[16384];
static size_t bootstrap_used = 0;

static void *bootstrap_malloc(size_t size)
{
    /* Align to 16 bytes */
    size = (size + 15) & ~(size_t)15;
    if (bootstrap_used + size > sizeof(bootstrap_buf))
        return NULL;
    void *ptr = bootstrap_buf + bootstrap_used;
    bootstrap_used += size;
    return ptr;
}

static int is_bootstrap_ptr(void *ptr)
{
    return (char *)ptr >= bootstrap_buf &&
           (char *)ptr < bootstrap_buf + sizeof(bootstrap_buf);
}

/* ── Resolve real functions ── */
int init_alloc_hooks(void)
{
    if (hooks_initialized == 1)
        return 0;
    if (hooks_initialized == -1)
        return 0; /* re-entrant call during dlsym */

    hooks_initialized = -1;

    real_malloc         = dlsym(RTLD_NEXT, "malloc");
    real_free           = dlsym(RTLD_NEXT, "free");
    real_calloc         = dlsym(RTLD_NEXT, "calloc");
    real_realloc        = dlsym(RTLD_NEXT, "realloc");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_mmap           = dlsym(RTLD_NEXT, "mmap");
    real_munmap         = dlsym(RTLD_NEXT, "munmap");

    if (!real_malloc || !real_free || !real_calloc || !real_realloc ||
        !real_posix_memalign || !real_mmap || !real_munmap) {
        fprintf(stderr, "TIERMEM: FATAL: failed to resolve allocation functions\n");
        return -1;
    }

    hooks_initialized = 1;
    return 0;
}

/* ── Macro to ensure hooks are ready ── */
#define ENSURE_HOOKS_OR_RETURN(fallback) \
    do { \
        if (__builtin_expect(hooks_initialized <= 0, 0)) { \
            if (hooks_initialized == -1) \
                return fallback; \
            init_alloc_hooks(); \
        } \
    } while (0)

#define ENSURE_HOOKS_OR_RETURN_VOID() \
    do { \
        if (__builtin_expect(hooks_initialized <= 0, 0)) { \
            if (hooks_initialized == -1) \
                return; \
            init_alloc_hooks(); \
        } \
    } while (0)

/* ── malloc ── */
void *malloc(size_t size)
{
    if (__builtin_expect(hooks_initialized <= 0, 0)) {
        if (hooks_initialized == -1)
            return bootstrap_malloc(size);
        init_alloc_hooks();
        if (!real_malloc)
            return bootstrap_malloc(size);
    }

    if (in_hook || !g_initialized)
        return real_malloc(size);

    in_hook = 1;
    void *ptr = real_malloc(size);
    if (ptr && size >= g_config.obj_threshold)
        obj_store_insert(getpid(), (uintptr_t)ptr, size, ALLOC_MALLOC);
    in_hook = 0;
    return ptr;
}

/* ── calloc ── */
void *calloc(size_t nmemb, size_t size)
{
    if (__builtin_expect(hooks_initialized <= 0, 0)) {
        if (hooks_initialized == -1) {
            /* bootstrap: calloc must zero memory */
            void *ptr = bootstrap_malloc(nmemb * size);
            if (ptr) memset(ptr, 0, nmemb * size);
            return ptr;
        }
        init_alloc_hooks();
        if (!real_calloc) {
            void *ptr = bootstrap_malloc(nmemb * size);
            if (ptr) memset(ptr, 0, nmemb * size);
            return ptr;
        }
    }

    if (in_hook || !g_initialized)
        return real_calloc(nmemb, size);

    in_hook = 1;
    void *ptr = real_calloc(nmemb, size);
    size_t total = nmemb * size;
    if (ptr && total >= g_config.obj_threshold)
        obj_store_insert(getpid(), (uintptr_t)ptr, total, ALLOC_CALLOC);
    in_hook = 0;
    return ptr;
}

/* ── realloc ── */
void *realloc(void *oldptr, size_t size)
{
    /* Bootstrap pointers can't be realloc'd properly */
    if (is_bootstrap_ptr(oldptr)) {
        void *newptr = malloc(size);
        if (newptr && oldptr) {
            /* Copy as much as possible (we don't know old size exactly) */
            memcpy(newptr, oldptr, size);
        }
        return newptr;
    }

    ENSURE_HOOKS_OR_RETURN(NULL);

    if (in_hook || !g_initialized)
        return real_realloc(oldptr, size);

    in_hook = 1;

    /* Remove old object if tracked */
    if (oldptr)
        obj_store_remove(getpid(), (uintptr_t)oldptr);

    void *newptr = real_realloc(oldptr, size);

    /* Track new allocation if large enough */
    if (newptr && size >= g_config.obj_threshold)
        obj_store_insert(getpid(), (uintptr_t)newptr, size, ALLOC_REALLOC);

    in_hook = 0;
    return newptr;
}

/* ── free ── */
void free(void *ptr)
{
    if (!ptr) return;
    if (is_bootstrap_ptr(ptr)) return; /* don't free bootstrap allocations */

    ENSURE_HOOKS_OR_RETURN_VOID();

    if (in_hook || !g_initialized) {
        real_free(ptr);
        return;
    }

    in_hook = 1;
    obj_store_remove(getpid(), (uintptr_t)ptr);
    in_hook = 0;

    real_free(ptr);
}

/* ── posix_memalign ── */
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    ENSURE_HOOKS_OR_RETURN(ENOMEM);

    if (in_hook || !g_initialized)
        return real_posix_memalign(memptr, alignment, size);

    in_hook = 1;
    int ret = real_posix_memalign(memptr, alignment, size);
    if (ret == 0 && *memptr && size >= g_config.obj_threshold)
        obj_store_insert(getpid(), (uintptr_t)*memptr, size, ALLOC_POSIX_MEMALIGN);
    in_hook = 0;
    return ret;
}

/* ── mmap ── */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    ENSURE_HOOKS_OR_RETURN(MAP_FAILED);

    if (in_hook || !g_initialized)
        return real_mmap(addr, length, prot, flags, fd, offset);

    in_hook = 1;
    void *ptr = real_mmap(addr, length, prot, flags, fd, offset);

    /* Only track anonymous private mappings */
    if (ptr != MAP_FAILED && length >= g_config.obj_threshold &&
        (flags & MAP_ANONYMOUS) && (flags & MAP_PRIVATE))
        obj_store_insert(getpid(), (uintptr_t)ptr, length, ALLOC_MMAP);

    in_hook = 0;
    return ptr;
}

/* ── munmap ── */
int munmap(void *addr, size_t length)
{
    ENSURE_HOOKS_OR_RETURN(-1);

    if (in_hook || !g_initialized)
        return real_munmap(addr, length);

    in_hook = 1;
    obj_store_remove(getpid(), (uintptr_t)addr);
    in_hook = 0;

    return real_munmap(addr, length);
}
