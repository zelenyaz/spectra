#ifndef TIERMEM_PERF_UTIL_H
#define TIERMEM_PERF_UTIL_H

#include <linux/perf_event.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

static inline long
sys_perf_event_open(struct perf_event_attr *attr,
                    pid_t pid, int cpu, int group_fd,
                    unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static inline int perf_enable(int fd)
{
    return ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
}

static inline int perf_disable(int fd)
{
    return ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
}

static inline int perf_reset(int fd)
{
    return ioctl(fd, PERF_EVENT_IOC_RESET, 0);
}

/* Read a counting event's value */
static inline uint64_t perf_read_count(int fd)
{
    uint64_t val = 0;
    if (read(fd, &val, sizeof(val)) != sizeof(val))
        return 0;
    return val;
}

/* Read a counting event with PERF_FORMAT_TOTAL_TIME_ENABLED|RUNNING */
struct perf_read_scaled {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

static inline struct perf_read_scaled perf_read_count_scaled(int fd)
{
    struct perf_read_scaled r = {0, 0, 0};
    if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        memset(&r, 0, sizeof(r));
    return r;
}

/* Memory barrier macros for ring buffer */
#define smp_rmb()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb()  __atomic_thread_fence(__ATOMIC_RELEASE)

static inline uint64_t rb_read_head(struct perf_event_mmap_page *page)
{
    uint64_t head = __atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE);
    return head;
}

static inline void rb_write_tail(struct perf_event_mmap_page *page, uint64_t tail)
{
    __atomic_store_n(&page->data_tail, tail, __ATOMIC_RELEASE);
}

#endif /* TIERMEM_PERF_UTIL_H */
