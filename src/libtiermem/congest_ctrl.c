/*
 * congest_ctrl.c — latency-based congestion controller.
 *
 * Each epoch: read per-tier latency (cha_lat), compute the normalized error
 * e = (L_D - L_A)/max(L_D,L_A), advance a Schmitt trigger, and adjust a global
 * interleave ratio r in [0,1]. r maps to an interleave level k = round(r*G);
 * every local-resident object spills the pages whose slot (i & (G-1)) < k to
 * the remote tier. Migration is performed here (pause/move_pages/resume) so
 * migrate.c is untouched. Gated behind TIERMEM_CONGEST_CTRL=1.
 *
 * _GNU_SOURCE is supplied by the Makefile (-D_GNU_SOURCE).
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <numaif.h>

#include "congest_ctrl.h"
#include "congest_ctrl_math.h"
#include "cha_lat.h"
#include "config.h"
#include "hook.h"
#include "log.h"
#include "obj_store.h"
#include "thread.h"
#include "monitor.h"

/* ── config (parsed from env in congest_ctrl_init) ── */
static bool     g_enabled;
static int      g_G;
static double   g_din, g_dout, g_kp, g_dr_max, g_r0, g_beta;
static uint64_t g_min_ins;
static double   g_la_fallback;
static int      g_split;

/* ── controller state ── */
static double   g_r;
static int      g_state;          /* CC_BALANCED / CC_ARMED */

/* ── cumulative migration stats (this process) ── */
static uint64_t g_cc_pages_demoted;   /* local -> remote (congestion spill) */
static uint64_t g_cc_pages_promoted;  /* remote -> local (interleave level lowered) */

static double env_d(const char *k, double def)
{
    const char *v = getenv(k);
    return v ? atof(v) : def;
}
static long env_l(const char *k, long def)
{
    const char *v = getenv(k);
    return v ? atol(v) : def;
}

bool congest_ctrl_enabled(void) { return g_enabled; }

void congest_ctrl_get_stats(uint64_t *pages_demoted, uint64_t *pages_promoted)
{
    if (pages_demoted)  *pages_demoted  = g_cc_pages_demoted;
    if (pages_promoted) *pages_promoted = g_cc_pages_promoted;
}

int congest_ctrl_init(void)
{
    g_enabled = env_l("TIERMEM_CONGEST_CTRL", 0) != 0;
    if (!g_enabled) return 0;

    g_G       = cc_round_pow2((int)env_l("TIERMEM_CC_INTERLEAVE_G", 16), 1024);
    g_din     = env_d("TIERMEM_CC_DELTA_IN",  0.05);
    g_dout    = env_d("TIERMEM_CC_DELTA_OUT", 0.15);
    g_kp      = env_d("TIERMEM_CC_KP",        0.5);
    g_dr_max  = env_d("TIERMEM_CC_DR_MAX",    0.05);
    g_r0      = env_d("TIERMEM_CC_R0",        0.10);
    g_beta    = env_d("TIERMEM_CC_LAT_EWMA",  0.3);
    g_min_ins = (uint64_t)env_l("TIERMEM_CC_MIN_INSERTS", 10000);
    g_la_fallback = env_d("TIERMEM_CC_LA_FALLBACK", 300.0);
    g_split   = (int)env_l("TIERMEM_CC_SPLIT_CHA", 1);

    g_r = 0.0;
    g_state = CC_BALANCED;

    if (cha_lat_init(g_split, g_beta, g_min_ins, g_la_fallback) != 0) {
        TM_ERR("congest_ctrl: cha_lat_init failed; disabling controller");
        g_enabled = false;
        return -1;
    }
    TM_INFO("congest_ctrl enabled: G=%d din=%.2f dout=%.2f kp=%.2f dr_max=%.2f "
            "r0=%.2f beta=%.2f min_ins=%lu la_fb=%.0f split=%d",
            g_G, g_din, g_dout, g_kp, g_dr_max, g_r0, g_beta,
            (unsigned long)g_min_ins, g_la_fallback, g_split);
    return 0;
}

void congest_ctrl_cleanup(void)
{
    if (g_enabled) cha_lat_cleanup();
}

/* ── candidate collection ── */
struct cand_ctx {
    mem_object_t **arr;
    int            n;
    int            cap;
};

static long query_node(uintptr_t addr)
{
    void *p = (void *)addr;
    int status = -1;
    long ret = syscall(__NR_move_pages, 0, 1, &p, NULL, &status, 0);
    return (ret == 0 && status >= 0) ? status : -1;
}

static void collect_local(mem_object_t *obj, void *ctx)
{
    struct cand_ctx *c = ctx;
    if (c->n >= c->cap || obj->nr_pages == 0) return;
    if (obj->pid != getpid()) return;     /* own objects only (multi-proc) */

    int node = obj->current_node;
    if (node < 0) {                       /* resolve unknown node */
        node = (int)query_node(obj->start_addr);
        if (node >= 0) obj->current_node = node;
    }
    if (node != LOCAL_NODE) return;       /* skip cold/remote objects */
    c->arr[c->n++] = obj;
}

/* Global bandwidth-density reference for proportional demotion: the maximum
 * bw_ema/nr_pages over local-resident candidates. Leader-only and pid-agnostic
 * so every process normalizes against the same value (published via shm). Cross
 * process current_node visibility lags by at most one epoch, since each owning
 * process resolves current_node during its own migrate phase; the controller
 * re-evaluates the reference every epoch, so any transient self-corrects. */
static void collect_dmax(mem_object_t *obj, void *ctx)
{
    double *dmax = ctx;
    if (obj->nr_pages == 0 || obj->bw_ema <= 0.0) return;
    if (obj->current_node != LOCAL_NODE) return;   /* candidates only */
    double density = obj->bw_ema / (double)obj->nr_pages;
    if (density > *dmax) *dmax = density;
}

static double compute_density_max(void)
{
    double dmax = 0.0;
    obj_store_foreach(collect_dmax, &dmax);
    return dmax;
}

/* Count pages that landed on the target node after a move_pages() batch. */
static uint64_t count_moved(const int *status, int n, int tnode)
{
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        if (status[i] == tnode) m++;
    return m;
}

/* Migrate the delta slot range for one object. Called while threads are paused;
 * page arrays are on the stack to avoid the malloc arena lock. Successfully
 * moved pages are tallied into the cumulative congestion-control counters. */
static void interleave_object(mem_object_t *obj, int k_target, int G,
                              uint64_t *moved_out)
{
    int k_applied = (int)obj->cc_ilv_k;
    if (k_applied > G) k_applied = G;
    if (k_applied == k_target) return;

    int tnode = (k_target > k_applied) ? CXL_NODE : LOCAL_NODE;

    void *pages[MIGRATE_BATCH_MAX];
    int   nodes[MIGRATE_BATCH_MAX];
    int   status[MIGRATE_BATCH_MAX];
    int   b = 0;
    uint64_t moved = 0;

    for (uint64_t i = 0; i < obj->nr_pages; i++) {
        if (!cc_slot_in_delta(i, k_applied, k_target, G))
            continue;
        pages[b] = (void *)(obj->start_addr + i * TM_PAGE_SIZE);
        nodes[b] = tnode;
        if (++b == MIGRATE_BATCH_MAX) {
            syscall(__NR_move_pages, 0, b, pages, nodes, status, MPOL_MF_MOVE);
            moved += count_moved(status, b, tnode);
            b = 0;
        }
    }
    if (b > 0) {
        syscall(__NR_move_pages, 0, b, pages, nodes, status, MPOL_MF_MOVE);
        moved += count_moved(status, b, tnode);
    }
    obj->cc_ilv_k = (uint32_t)k_target;

    if (tnode == CXL_NODE) g_cc_pages_demoted  += moved;
    else                   g_cc_pages_promoted += moved;
    *moved_out += moved;
}

/* Leader-only: measure, advance the state machine, compute the epoch decision. */
bool congest_ctrl_decide(double T, double *r_out, double *density_max, int *G)
{
    (void)T;
    *r_out = 0.0;
    *density_max = 0.0;
    *G = g_G;
    if (!g_enabled) { *G = 0; return false; }

    double LD = 0, LA = 0, RD = 0, RA = 0;
    if (cha_lat_epoch(&LD, &LA, &RD, &RA) != 0) {
        /* Untrustworthy sample: hold state. Keep owning placement if r>0 so the
         * current per-object interleave is preserved this epoch. */
        *r_out = g_r;
        *density_max = compute_density_max();
        TM_EPOCH("  [cc] idle guard: ins_local below min, holding r=%.3f", g_r);
        return g_r > 0.0;
    }

    double e = cc_error(LD, LA);
    int prev_state = g_state;
    g_state = cc_schmitt_next(g_state, e, g_din, g_dout);

    if (g_state == CC_ARMED) {
        double r_before = g_r;
        g_r = cc_warm_start(g_r, e, g_r0);
        if (g_r != r_before)
            TM_DBG("  [cc] warm_start: r %.3f -> %.3f (r0=%.3f)", r_before, g_r, g_r0);
        r_before = g_r;
        g_r = cc_update_r(g_r, e, g_kp, g_dr_max);
        TM_DBG("  [cc] update_r:   r %.3f -> %.3f (kp=%.2f e=%+.3f dr_max=%.3f)",
               r_before, g_r, g_kp, e, g_dr_max);
    }

    double dmax = compute_density_max();
    *r_out = g_r;
    *density_max = dmax;

    TM_EPOCH("  [cc] L_D=%.1f L_A=%.1f e=%+.3f state=%s%s r=%.3f dmax=%.3g G=%d "
             "ins_local=%.0f ins_remote=%.0f",
             LD, LA, e,
             g_state == CC_ARMED ? "ARMED" : "BALANCED",
             prev_state != g_state ? "*" : "",
             g_r, dmax, g_G, RD, RA);

    /* Own placement only while interleave is actually applied (r > 0). When
     * congested, warm_start/update above already drove r > 0, so promotion stays
     * suppressed. When r == 0 (balanced, or armed in the under-loaded e<0
     * direction) hand back to the base path so it can promote into local. */
    return g_r > 0.0;
}

/* Any process: bandwidth-proportionally interleave this process's own local
 * objects. Each object's demotion level k_i scales with its bandwidth density
 * relative to density_max, so the spilled-page count is proportional to bw_ema. */
void congest_ctrl_migrate_local(double r, double density_max, int G,
                                bool disable_mon)
{
    if (G <= 0) return;

    /* Collect own local-resident objects (resolve unknown nodes first); malloc
     * the array BEFORE pausing threads. */
    int cap = 4096;
    mem_object_t **arr = real_malloc((size_t)cap * sizeof(*arr));
    if (!arr) return;
    struct cand_ctx cc = { arr, 0, cap };
    obj_store_foreach(collect_local, &cc);
    if (cc.n == 0) { real_free(arr); return; }

    if (disable_mon) monitor_disable_events();
    if (pause_all_threads() != 0) {
        resume_all_threads();
        if (disable_mon) monitor_enable_events();
        real_free(arr);
        return;
    }
    uint64_t moved = 0;
    for (int i = 0; i < cc.n; i++) {
        mem_object_t *obj = cc.arr[i];
        double density = obj->bw_ema / (double)obj->nr_pages;   /* nr_pages>0 */
        int k_i = cc_k_for_object(r, G, density, density_max);
        uint64_t before = moved;
        interleave_object(obj, k_i, G, &moved);
        TM_DBG("  [cc]   obj 0x%lx %lu pg bw_ema=%.2f density=%.3g k=%d/%d moved=%lu",
               (unsigned long)obj->start_addr, (unsigned long)obj->nr_pages,
               obj->bw_ema, density, k_i, G, (unsigned long)(moved - before));
    }
    resume_all_threads();
    if (disable_mon) monitor_enable_events();
    real_free(arr);

    if (moved > 0)
        TM_EPOCH("  [cc] pid %d migrated %lu pages (r=%.3f dmax=%.3g G=%d, %d local objs)",
                 (int)getpid(), (unsigned long)moved, r, density_max, G, cc.n);
}
