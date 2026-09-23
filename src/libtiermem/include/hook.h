#ifndef TIERMEM_HOOK_H
#define TIERMEM_HOOK_H

#include <stddef.h>
#include <sys/mman.h>
#include <pthread.h>

/* Original function pointers resolved via dlsym(RTLD_NEXT, ...) */
extern void *(*real_malloc)(size_t);
extern void  (*real_free)(void *);
extern void *(*real_calloc)(size_t, size_t);
extern void *(*real_realloc)(void *, size_t);
extern int   (*real_posix_memalign)(void **, size_t, size_t);
extern void *(*real_mmap)(void *, size_t, int, int, int, off_t);
extern int   (*real_munmap)(void *, size_t);
extern int   (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *);

/* Thread-local recursion guard */
extern __thread int in_hook;

/* Three-state initialization: 0=not yet, -1=in progress, 1=done */
extern volatile int hooks_initialized;

/* Global initialized flag */
extern volatile int g_initialized;

/* Resolve all real_* function pointers */
int init_alloc_hooks(void);
int init_pthread_hook(void);

#endif /* TIERMEM_HOOK_H */
