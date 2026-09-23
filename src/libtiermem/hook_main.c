/*
 * hook_main.c — Entry point interception + init/cleanup
 *
 * Loaded via LD_PRELOAD. Intercepts __libc_start_main to initialize
 * tiermem before the application's main() runs.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "hook.h"
#include "log.h"
#include "obj_store.h"
#include "monitor.h"
#include "migrate.h"
#include "thread.h"
#include "shm.h"
#include "congest_ctrl.h"

/* ── Global state ── */
tiermem_config_t g_config;
volatile int g_initialized = 0;
static int g_is_monitor_owner = 0;  /* true only if this process started the monitor */

/* ── Config parsing helpers ── */

static int parse_int(const char *s, int def)
{
    if (!s || !*s) return def;
    char *end;
    long v = strtol(s, &end, 0);
    if (end == s) {
        TM_EPOCH("Warning: malformed int '%s', using default %d", s, def);
        return def;
    }
    return (int)v;
}

static double parse_double(const char *s, double def)
{
    if (!s || !*s) return def;
    char *end;
    double v = strtod(s, &end);
    if (end == s) {
        TM_EPOCH("Warning: malformed double '%s', using default %f", s, def);
        return def;
    }
    return v;
}

static uint64_t parse_u64(const char *s, uint64_t def)
{
    if (!s || !*s) return def;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s) return def;
    return (uint64_t)v;
}

static size_t parse_size(const char *s, size_t def)
{
    if (!s || !*s) return def;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s) return def;

    /* Handle suffixes */
    if (*end == 'G' || !strncmp(end, "GiB", 3))
        v *= 1024ULL * 1024 * 1024;
    else if (*end == 'M' || !strncmp(end, "MiB", 3))
        v *= 1024ULL * 1024;
    else if (*end == 'K' || !strncmp(end, "KiB", 3))
        v *= 1024ULL;

    return (size_t)v;
}

void tiermem_parse_config(void)
{
    g_config.target            = getenv("TIERMEM_TARGET");
    g_config.obj_threshold     = parse_size(getenv("TIERMEM_OBJ_THRESHOLD"),
                                            2ULL * 1024 * 1024);
    g_config.epoch_sec         = parse_double(getenv("TIERMEM_EPOCH_SEC"), 5.0);
    g_config.pebs_period       = parse_u64(getenv("TIERMEM_PEBS_PERIOD"), 10007);
    g_config.store_period      = parse_u64(getenv("TIERMEM_STORE_PERIOD"), 100003);
    g_config.allload_period    = parse_u64(getenv("TIERMEM_ALLLOAD_PERIOD"), 1000003);
    g_config.ocr_interval_ms   = parse_int(getenv("TIERMEM_OCR_INTERVAL"), 100);
    g_config.dram_budget_bytes = parse_size(getenv("TIERMEM_DRAM_BUDGET"), 0);
    g_config.migrate_threads   = parse_int(getenv("TIERMEM_MIGRATE_THREADS"), 4);
    g_config.hysteresis_pct    = parse_double(getenv("TIERMEM_HYSTERESIS"), 10.0);
    g_config.reg_theta         = parse_double(getenv("TIERMEM_REG_THETA"), 0.15);
    g_config.pf_reg_gamma      = parse_double(getenv("TIERMEM_PF_REG_GAMMA"), 3.0);
    g_config.pf_sw_kappa       = parse_double(getenv("TIERMEM_PF_SW_KAPPA"), 2.0);
    g_config.bw_ema_alpha      = parse_double(getenv("TIERMEM_BW_EMA_ALPHA"), 0.3);
    if (g_config.bw_ema_alpha < 0.0) g_config.bw_ema_alpha = 0.0;
    if (g_config.bw_ema_alpha > 1.0) g_config.bw_ema_alpha = 1.0;
    g_config.monitor_cpu       = parse_int(getenv("TIERMEM_MONITOR_CPU"), 1);
    g_config.migrate_cpu_start = parse_int(getenv("TIERMEM_MIGRATE_CPU_START"), 2);
    g_config.warmup_sec        = parse_double(getenv("TIERMEM_WARMUP_SEC"), 0.0);
    g_config.log_level         = parse_int(getenv("TIERMEM_LOG_LEVEL"), 1);
    g_config.no_thread_pause   = parse_int(getenv("TIERMEM_NO_THREAD_PAUSE"), 0);

    /* Multi-process */
    g_config.multi_proc        = parse_int(getenv("TIERMEM_MULTI_PROC"), 0);
    g_config.shm_name          = getenv("TIERMEM_SHM_NAME");
    if (!g_config.shm_name)    g_config.shm_name = SHM_DEFAULT_NAME;
    g_config.shm_size          = parse_size(getenv("TIERMEM_SHM_SIZE"),
                                            SHM_DEFAULT_SIZE);
}

/* ── Is this the target program? ── */
static int is_target_program(void)
{
    if (!g_config.target)
        return 1; /* No filter — instrument all */

    /* Read program name from /proc/self/cmdline */
    char buf[256];
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    /* Extract basename */
    const char *base = strrchr(buf, '/');
    base = base ? base + 1 : buf;

    return strcmp(base, g_config.target) == 0;
}

/* ── Init / Cleanup ── */

static void tiermem_cleanup(void)
{
    if (!g_initialized) return;
    g_initialized = 0;

    TM_INFO("Cleanup starting (PID=%d)...", getpid());

    if (g_config.multi_proc && g_shm) {
        if (g_is_monitor_owner) {
            /* Original leader: stop monitor thread we started */
            g_monitor.running = false;
            pthread_join(g_monitor.monitor_tid, NULL);
            monitor_disable_events();
            monitor_cleanup();
        } else {
            /* Follower (even if re-elected as leader): stop follower thread */
            epoch_follower_stop();
        }
        ocr_self_cleanup();
    } else {
        /* Single-process: stop monitor thread */
        g_monitor.running = false;
        pthread_join(g_monitor.monitor_tid, NULL);
        monitor_disable_events();
        monitor_cleanup();
    }

    /* Shutdown migration pool (per-process) */
    migrate_cleanup();

    /* Print stats */
    TM_INFO("=== Tiermem Statistics (PID=%d) ===", getpid());
    if (!g_config.multi_proc || shm_is_leader()) {
        TM_INFO("  Epochs completed:     %lu", g_monitor.epoch_counter);
        TM_INFO("  PEBS samples total:   %lu", g_monitor.samples_collected);
        TM_INFO("  PEBS samples lost:    %lu", g_monitor.samples_lost);
        TM_INFO("  PEBS unmatched:       %lu", g_monitor.samples_unmatched);
    }
    TM_INFO("  Objects tracked:      %d",  obj_store_total_count());
    TM_INFO("  Migrations total:     %lu", g_migrate_pool.migrations_total);
    TM_INFO("  Pages promoted:       %lu", g_migrate_pool.pages_promoted);
    TM_INFO("  Pages demoted:        %lu", g_migrate_pool.pages_demoted);
    TM_INFO("  Migration errors:     %lu", g_migrate_pool.migration_errors);

    /* Congestion-control migrations (separate path; bypasses g_migrate_pool). */
    {
        uint64_t cc_dem = 0, cc_pro = 0;
        congest_ctrl_get_stats(&cc_dem, &cc_pro);
        if (congest_ctrl_enabled() || cc_dem + cc_pro > 0) {
            TM_INFO("  CC pages demoted:     %lu", cc_dem);
            TM_INFO("  CC pages promoted:    %lu", cc_pro);
        }
    }

    /* Cleanup subsystems */
    obj_store_cleanup();
    thread_mgmt_cleanup();

    /* Detach from shared memory (handles refcount, leader re-election, unlink) */
    if (g_config.multi_proc && g_shm)
        shm_detach();

    TM_INFO("Cleanup completed.");
}

static int tiermem_init(void)
{
    /* 1. Parse config */
    tiermem_parse_config();

    /* 2. Resolve real functions */
    if (init_alloc_hooks() != 0) {
        TM_ERR("Failed to init alloc hooks");
        return -1;
    }
    if (init_pthread_hook() != 0) {
        TM_ERR("Failed to init pthread hook");
        return -1;
    }

    /* 3. Shared memory (multi-process mode) */
    int is_leader = 1;  /* single-process is always "leader" */
    if (g_config.multi_proc) {
        int ret = shm_create_or_attach();
        if (ret < 0) {
            TM_ERR("Failed to create/attach shared memory");
            return -1;
        }
        is_leader = (ret == 1);  /* creator is leader */
        shm_register_proc(getpid());
    }

    /* 4. Init object store */
    if (g_config.multi_proc) {
        if (is_leader) {
            if (obj_store_init() != 0) {
                TM_ERR("Failed to init object store");
                return -1;
            }
        } else {
            if (obj_store_attach() != 0) {
                TM_ERR("Failed to attach object store");
                return -1;
            }
        }
    } else {
        if (obj_store_init() != 0) {
            TM_ERR("Failed to init object store");
            return -1;
        }
    }

    /* 5. Multi-process: each process opens OCR events for itself (pid=0)
     *    BEFORE any threads are created, so inherit=1 covers them all. */
    if (g_config.multi_proc) {
        if (ocr_self_init() != 0)
            TM_INFO("Warning: ocr_self_init failed, OCR data will be missing");
    }

    /* 6. Init thread management (always per-process) */
    if (thread_mgmt_init() != 0) {
        TM_ERR("Failed to init thread management");
        return -1;
    }

    /* 7. Init migration pool (always per-process) */
    if (migrate_init() != 0) {
        TM_ERR("Failed to init migration pool");
        return -1;
    }

    /* 8. Init monitor / follower */
    if (is_leader) {
        /* Leader (or single-process): open perf events, launch monitor */
        if (monitor_init() != 0) {
            TM_ERR("Failed to init monitor");
            return -1;
        }
        if (monitor_launch() != 0) {
            TM_ERR("Failed to launch monitor thread");
            return -1;
        }
        g_is_monitor_owner = 1;
    } else {
        /* Multi-process follower: launch epoch follower thread */
        if (epoch_follower_launch() != 0) {
            TM_ERR("Failed to launch epoch follower");
            return -1;
        }
    }

    /* 9. Register cleanup */
    atexit(tiermem_cleanup);

    /* 10. Log configuration */
    TM_INFO("=== Tiermem Configuration ===");
    TM_INFO("  Target:             %s", g_config.target ? g_config.target : "(all)");
    TM_INFO("  Object threshold:   %zu bytes", g_config.obj_threshold);
    TM_INFO("  Epoch interval:     %.1f sec", g_config.epoch_sec);
    TM_INFO("  PEBS period:        %lu", g_config.pebs_period);
    TM_INFO("  Store period:       %lu", g_config.store_period);
    TM_INFO("  Allload period:     %lu", g_config.allload_period);
    TM_INFO("  OCR interval:       %d ms", g_config.ocr_interval_ms);
    TM_INFO("  DRAM budget:        %zu bytes (%s)", g_config.dram_budget_bytes,
            g_config.dram_budget_bytes ? "" : "unlimited");
    TM_INFO("  Migrate threads:    %d", g_config.migrate_threads);
    TM_INFO("  Hysteresis:         %.1f%%", g_config.hysteresis_pct);
    TM_INFO("  Reg theta:          %.3f", g_config.reg_theta);
    TM_INFO("  PF reg gamma:       %.3f", g_config.pf_reg_gamma);
    TM_INFO("  PF sw kappa:        %.3f", g_config.pf_sw_kappa);
    TM_INFO("  BW EMA alpha:       %.3f", g_config.bw_ema_alpha);
    TM_INFO("  Monitor CPU:        %d", g_config.monitor_cpu);
    TM_INFO("  Migrate CPU start:  %d", g_config.migrate_cpu_start);
    TM_INFO("  Warmup:             %.1f sec", g_config.warmup_sec);
    TM_INFO("  Log level:          %d", g_config.log_level);
    if (g_config.multi_proc) {
        TM_INFO("  Multi-process:      %s (shm=%s)",
                is_leader ? "LEADER" : "FOLLOWER", g_config.shm_name);
        TM_INFO("  SHM size:           %zu bytes", g_config.shm_size);
    }

    g_initialized = 1;
    TM_INFO("[%s] Initialization completed, PID=%d",
            tm_log_wall_time(), getpid());
    return 0;
}

/* ── __libc_start_main interception ── */

typedef int (*libc_start_main_t)(
    int (*main)(int, char **, char **),
    int argc, char **argv,
    int (*init)(int, char **, char **),
    void (*fini)(void),
    void (*rtld_fini)(void),
    void *stack_end);

int __libc_start_main(
    int (*main)(int, char **, char **),
    int argc, char **argv,
    int (*init)(int, char **, char **),
    void (*fini)(void),
    void (*rtld_fini)(void),
    void *stack_end)
{
    /* Resolve the real __libc_start_main */
    libc_start_main_t real_start = (libc_start_main_t)dlsym(RTLD_NEXT,
                                                             "__libc_start_main");
    if (!real_start) {
        fprintf(stderr, "TIERMEM: FATAL: cannot find __libc_start_main: %s\n",
                dlerror());
        abort();
    }

    /* Parse config early (needed for target check) */
    tiermem_parse_config();

    if (is_target_program()) {
        in_hook = 1;
        if (tiermem_init() != 0) {
            fprintf(stderr, "TIERMEM: FATAL: initialization failed; refusing to run without instrumentation\n");
            /* Initialization may have left workers or shared state partially
             * configured. Do not run application code or exit handlers. */
            _exit(EXIT_FAILURE);
        }
        in_hook = 0;
    }

    return real_start(main, argc, argv, init, fini, rtld_fini, stack_end);
}
