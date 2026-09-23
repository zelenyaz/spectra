#ifndef TIERMEM_LOG_H
#define TIERMEM_LOG_H

#include "config.h"
#include <stdio.h>
#include <time.h>

static inline const char *tm_log_wall_time(void)
{
    static __thread char buf[20];
    time_t now = time(NULL);
    struct tm tm_now;

    if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL) {
        return "0000-00-00 00:00:00";
    }

    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        return "0000-00-00 00:00:00";
    }

    return buf;
}

#define TM_LOG(level, fmt, ...) \
    do { \
        if (g_config.log_level >= (level)) \
            fprintf(stderr, "TIERMEM: " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define TM_ERR(fmt, ...)   TM_LOG(0, "[ERROR] " fmt, ##__VA_ARGS__)
#define TM_INFO(fmt, ...)  TM_LOG(1, fmt, ##__VA_ARGS__)
#define TM_EPOCH(fmt, ...) TM_LOG(2, fmt, ##__VA_ARGS__)
#define TM_DBG(fmt, ...)   TM_LOG(3, fmt, ##__VA_ARGS__)

#endif /* TIERMEM_LOG_H */
