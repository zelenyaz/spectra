#ifndef TIERMEM_MONITOR_H
#define TIERMEM_MONITOR_H

#include "config.h"
#include <pthread.h>
#include <linux/perf_event.h>
#include <time.h>

/* Per-process OCR event tracking */
typedef struct {
    pid_t    pid;

    /* ── 4-event state ── */
    int      fd_demand_data_rd, fd_demand_rfo,     fd_hw_data_pf,     fd_hw_rfo_pf;
    uint64_t prev_demand_data_rd, prev_demand_rfo,     prev_hw_data_pf,     prev_hw_rfo_pf;
    uint64_t prev_en_demand_data_rd, prev_en_demand_rfo,  prev_en_hw_data_pf,  prev_en_hw_rfo_pf;
    uint64_t prev_ru_demand_data_rd, prev_ru_demand_rfo,  prev_ru_hw_data_pf,  prev_ru_hw_rfo_pf;
    uint64_t epoch_demand_data_rd, epoch_demand_rfo,    epoch_hw_data_pf,    epoch_hw_rfo_pf;
} proc_ocr_t;

/* Global monitoring context */
typedef struct {
    /* Perf event fds */
    int pebs_fd[NUM_CPUS];

    /* Per-process OCR events (pid=target, cpu=-1) */
    proc_ocr_t ocr_procs[MAX_PROCS];
    int        ocr_proc_count;

    /* PEBS ring buffers */
    struct perf_event_mmap_page *pebs_mmap[NUM_CPUS];
    size_t pebs_mmap_size;

    /* Store-sample PEBS (mem_trans_retired.store_sample) */
    int pebs_store_fd[NUM_CPUS];
    struct perf_event_mmap_page *pebs_store_mmap[NUM_CPUS];

    /* Store-sample stats */
    uint64_t store_samples_collected;
    uint64_t store_samples_lost;
    uint64_t store_samples_unmatched;

    /* MEM_INST_RETIRED.ALL_LOADS PEBS: covers normal loads + SW prefetches */
    int pebs_allload_fd[NUM_CPUS];
    struct perf_event_mmap_page *pebs_allload_mmap[NUM_CPUS];
    uint64_t allload_samples_collected;
    uint64_t allload_samples_lost;
    uint64_t allload_samples_unmatched;
    uint64_t allload_samples_pf;    /* subset classified as SW prefetch */

    /* iMC uncore WR fds: [node][channel] — UNC_M_CAS_COUNT.WR ground truth */
    int      imc_wr_fds[NUM_NODES][NUM_IMC_CHANNELS];
    uint64_t prev_imc_wr[NUM_NODES][NUM_IMC_CHANNELS];
    uint64_t imc_wr_bytes[NUM_NODES];  /* per-epoch total */

    /* iMC uncore RD fds: [node][channel] — UNC_M_CAS_COUNT.RD (diagnosis only) */
    int      imc_rd_fds[NUM_NODES][NUM_IMC_CHANNELS];
    uint64_t prev_imc_rd[NUM_NODES][NUM_IMC_CHANNELS];
    uint64_t imc_rd_bytes[NUM_NODES];  /* per-epoch total */

    /* Epoch tracking */
    uint64_t epoch_counter;
    struct timespec epoch_start;

    /* Monitor thread */
    pthread_t monitor_tid;
    pid_t     monitor_tid_kernel;
    volatile bool running;

    /* Stats */
    uint64_t samples_collected;
    uint64_t samples_lost;
    uint64_t samples_unmatched;
} monitor_ctx_t;

extern monitor_ctx_t g_monitor;

int  monitor_init(void);
void monitor_cleanup(void);
int  monitor_launch(void);
void monitor_enable_events(void);
void monitor_disable_events(void);

/* Multi-process: each process opens OCR for itself (pid=0) before threads */
int  ocr_self_init(void);       /* open & enable OCR events for self */
void ocr_self_cleanup(void);    /* close self OCR fds */

/* Multi-process: follower thread (reads own OCR, waits for epoch signals) */
int  epoch_follower_launch(void);
void epoch_follower_stop(void);

#endif /* TIERMEM_MONITOR_H */
