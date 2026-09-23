#ifndef TIERMEM_CONFIG_H
#define TIERMEM_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Topology (Sapphire Rapids, 2-socket) ── */
#define NUM_NODES             2
#define LOCAL_NODE            0
#define CXL_NODE              1
#define NUM_CPUS_PER_NODE     28
#define NUM_CPUS              (NUM_NODES * NUM_CPUS_PER_NODE)

/* ── iMC channels per socket (Sapphire Rapids) ── */
#define NUM_IMC_CHANNELS      8

/* ── Structural limits ── */
#define OBJ_STORE_PID_BUCKETS 64
#define MIGRATE_BATCH_MAX     512
#define MIGRATE_POOL_MAX      8
#define MIGRATE_QUEUE_MAX     256

/* ── Multi-process shared memory ── */
#define SHM_DEFAULT_NAME      "/tiermem_shm"
#define SHM_DEFAULT_SIZE      (64ULL << 20)       /* 64 MiB */
#define SHM_BASE_ADDR         0x600000000000ULL
#define SHM_MAGIC             0x5449524DU          /* "TIRM" */
#define SHM_VERSION           4
#define MAX_PROCS             64
#define MAX_OBJECTS           65536
#define MAX_PROC_LISTS        128
#define PROC_OBJ_CAPACITY     4096

/* ── Page size ── */
#define TM_PAGE_SIZE          4096
#define TM_PAGE_SHIFT         12

/* ── PEBS ring buffer ── */
#define RING_BUFFER_PAGES     128   /* 512 KiB per CPU */

/* ── Thread pause signal ── */
#define THREAD_PAUSE_SIGNAL   SIGRTMIN

/* ── World state ── */
#define WORLD_STATE_RUNNING   0
#define WORLD_STATE_PAUSED    1

/* ── Allocation types ── */
#define ALLOC_MALLOC          0
#define ALLOC_CALLOC          1
#define ALLOC_REALLOC         2
#define ALLOC_POSIX_MEMALIGN  3
#define ALLOC_MMAP            4

/* ── Epoch phases (multi-process) ── */
#define PHASE_SAMPLING        0
#define PHASE_MIGRATING       1
#define PHASE_DONE            2

/* ── Runtime configuration ── */
typedef struct {
    const char *target;
    size_t      obj_threshold;
    double      epoch_sec;
    uint64_t    pebs_period;
    uint64_t    store_period;    /* PEBS period for mem_trans_retired.store_sample */
    uint64_t    allload_period;     /* PEBS period for mem_inst_retired.all_loads */
    int         ocr_interval_ms;
    size_t      dram_budget_bytes;
    int         migrate_threads;
    double      hysteresis_pct;
    double      reg_theta;          /* saturation threshold for reg_load/reg_store */
    double      pf_reg_gamma;       /* exponent on reg_load in the HW-prefetch attribution weight */
    double      pf_sw_kappa;        /* multiplier on est. SW-prefetch lines in that weight */
    double      bw_ema_alpha;       /* EMA weight on new epoch bw (0..1); 0 disables smoothing */
    int         monitor_cpu;
    int         migrate_cpu_start;
    double      warmup_sec;
    int         log_level;
    int         no_thread_pause;    /* skip SIGRTMIN pause/resume; let workload threads run during migration */

    /* Multi-process */
    int         multi_proc;
    const char *shm_name;
    size_t      shm_size;
} tiermem_config_t;

extern tiermem_config_t g_config;

void tiermem_parse_config(void);

#endif /* TIERMEM_CONFIG_H */
