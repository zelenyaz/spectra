/*
 * cha_lat.c — per-tier demand-read access latency from CHA TOR uncore counters.
 *
 * Latency_tier = Σ TOR_OCCUPANCY.IA_MISS_DRD_<tier> / Σ TOR_INSERTS.IA_MISS_DRD_<tier>
 * (CHA clocks), summed across CHA slices on socket 0 (where workloads run).
 *
 * The idle guard applies to the LOCAL tier only. When the remote tier has too
 * few inserts to measure (e.g. nothing has been spilled there yet), its latency
 * is unknowable, so we substitute g_la_fallback rather than dividing by a near-
 * zero insert count (which would yield NaN/inf and poison the EMA). This lets
 * the controller arm on local load alone and start probing the remote tier.
 *
 * SPR encoding (verified on the AE testbed, kernel format umask = config:8-15,32-55):
 *   TOR_INSERTS   EventCode 0x35, base umask 0x01
 *   TOR_OCCUPANCY EventCode 0x36, base umask 0x01   (counter 0 only)
 *   umask_ext LOCAL 0xc816fe, REMOTE 0xc8177e  (placed in config bits 32-55)
 *
 * _GNU_SOURCE is supplied by the Makefile (-D_GNU_SOURCE), as for the other
 * translation units; we do not redefine it here.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "cha_lat.h"
#include "config.h"
#include "log.h"
#include "perf_util.h"

#define CHA_MAX_UNITS    64

#define CHA_EVT_INS      0x35u
#define CHA_EVT_OCC      0x36u
#define CHA_UMASK_BASE   0x01ULL
#define CHA_UMEXT_LOCAL  0xc816feULL
#define CHA_UMEXT_REMOTE 0xc8177eULL

enum { ACC_INS_LOCAL = 0, ACC_OCC_LOCAL, ACC_INS_REMOTE, ACC_OCC_REMOTE, ACC_N };

struct cha_fd {
    int      fd;
    int      acc;                 /* which accumulator this fd feeds */
    uint64_t prev_v, prev_en, prev_ru;
};

static struct cha_fd g_fds[CHA_MAX_UNITS * 4];
static int           g_nfds;
static uint64_t      g_min_inserts;
static double        g_la_fallback;

/* EWMA state for per-tier latency. g_beta is the weight on the new sample
 * (<=0 disables smoothing). Smoothing keeps a single placement-induced latency
 * swing from jerking the controller around. */
static double        g_beta;
static double        g_ld_ema, g_la_ema;
static bool          g_ema_init;

static uint64_t cha_config(uint32_t evt, uint64_t umext)
{
    return (uint64_t)evt | (CHA_UMASK_BASE << 8) | (umext << 32);
}

static int read_cha_type(int n)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/uncore_cha_%d/type", n);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int t = -1;
    if (fscanf(f, "%d", &t) != 1) t = -1;
    fclose(f);
    return t;
}

static int socket0_cpu(void)
{
    FILE *f = fopen("/sys/devices/uncore_cha_0/cpumask", "r");
    if (!f) return 0;
    int c = 0;
    if (fscanf(f, "%d", &c) != 1) c = 0;
    fclose(f);
    return c;   /* first token = a CPU on socket 0 */
}

static void open_one(int type, int cpu, uint32_t evt, uint64_t umext, int acc)
{
    if (g_nfds >= (int)(sizeof(g_fds) / sizeof(g_fds[0]))) return;

    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type        = type;
    a.size        = sizeof(a);
    a.config      = cha_config(evt, umext);
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    a.disabled    = 0;            /* count immediately; we read deltas */
    /* uncore events: do NOT set exclude_kernel or inherit (EINVAL otherwise). */

    int fd = sys_perf_event_open(&a, -1, cpu, -1, 0);
    if (fd < 0) {
        TM_DBG("cha_lat: open type=%d evt=%#x umext=%#lx failed: %s",
               type, evt, (unsigned long)umext, strerror(errno));
        return;
    }
    struct cha_fd *cf = &g_fds[g_nfds++];
    cf->fd  = fd;
    cf->acc = acc;
    struct perf_read_scaled s = perf_read_count_scaled(fd);
    cf->prev_v  = s.value;
    cf->prev_en = s.time_enabled;
    cf->prev_ru = s.time_running;
}

int cha_lat_init(int split_mode, double beta, uint64_t min_inserts,
                 double la_fallback)
{
    memset(g_fds, 0, sizeof(g_fds));
    g_nfds        = 0;
    g_min_inserts = min_inserts;
    g_la_fallback = la_fallback;
    g_beta        = beta;
    g_ld_ema      = 0.0;
    g_la_ema      = 0.0;
    g_ema_init    = false;

    int cpu = socket0_cpu();
    int units = 0;
    for (int n = 0; n < CHA_MAX_UNITS; n++) {
        int type = read_cha_type(n);
        if (type < 0) break;                 /* no more CHA slices */
        if (g_nfds + 4 > (int)(sizeof(g_fds) / sizeof(g_fds[0]))) break;

        if (split_mode) {
            if ((n & 1) == 0) {              /* even slice -> local tier */
                open_one(type, cpu, CHA_EVT_OCC, CHA_UMEXT_LOCAL,  ACC_OCC_LOCAL);
                open_one(type, cpu, CHA_EVT_INS, CHA_UMEXT_LOCAL,  ACC_INS_LOCAL);
            } else {                         /* odd slice  -> remote tier */
                open_one(type, cpu, CHA_EVT_OCC, CHA_UMEXT_REMOTE, ACC_OCC_REMOTE);
                open_one(type, cpu, CHA_EVT_INS, CHA_UMEXT_REMOTE, ACC_INS_REMOTE);
            }
        } else {                             /* multiplex all four per slice */
            open_one(type, cpu, CHA_EVT_OCC, CHA_UMEXT_LOCAL,  ACC_OCC_LOCAL);
            open_one(type, cpu, CHA_EVT_INS, CHA_UMEXT_LOCAL,  ACC_INS_LOCAL);
            open_one(type, cpu, CHA_EVT_OCC, CHA_UMEXT_REMOTE, ACC_OCC_REMOTE);
            open_one(type, cpu, CHA_EVT_INS, CHA_UMEXT_REMOTE, ACC_INS_REMOTE);
        }
        units++;
    }
    if (g_nfds == 0) {
        TM_ERR("cha_lat: no CHA events opened (uncore access denied?)");
        return -1;
    }
    TM_INFO("cha_lat: %d CHA units, %d fds, split=%d cpu=%d",
            units, g_nfds, split_mode, cpu);
    return 0;
}

void cha_lat_cleanup(void)
{
    for (int i = 0; i < g_nfds; i++)
        if (g_fds[i].fd >= 0) close(g_fds[i].fd);
    g_nfds = 0;
}

int cha_lat_epoch(double *L_D, double *L_A, double *R_D, double *R_A)
{
    uint64_t acc[ACC_N] = {0};

    for (int i = 0; i < g_nfds; i++) {
        struct cha_fd *cf = &g_fds[i];
        struct perf_read_scaled s = perf_read_count_scaled(cf->fd);
        uint64_t dv = s.value        - cf->prev_v;
        uint64_t de = s.time_enabled - cf->prev_en;
        uint64_t dr = s.time_running - cf->prev_ru;
        cf->prev_v  = s.value;
        cf->prev_en = s.time_enabled;
        cf->prev_ru = s.time_running;
        if (dr > 0) {
            dv = (uint64_t)((double)dv * (double)de / (double)dr);  /* mux scale */
            acc[cf->acc] += dv;
        }
    }

    uint64_t ins_l = acc[ACC_INS_LOCAL],  occ_l = acc[ACC_OCC_LOCAL];
    uint64_t ins_r = acc[ACC_INS_REMOTE], occ_r = acc[ACC_OCC_REMOTE];

    if (ins_l < g_min_inserts)
        return -1;                       /* local-idle guard: no usable signal */

    double ld = (double)occ_l / (double)ins_l;
    /* Remote tier may be unsampled (nothing spilled there yet); its latency is
     * then unmeasurable, so assume the reference latency instead of dividing by
     * a near-zero insert count. The controller still arms on local load. */
    double la = (ins_r < g_min_inserts)
                  ? g_la_fallback
                  : (double)occ_r / (double)ins_r;

    if (la < 300.0)
        la = 300.0;                  /* floor remote-tier latency at 300 CHA clocks */

    /* EWMA-smooth both tiers. The local-idle guard above returns early without
     * updating the EMA, so a stretch of idle epochs holds the last smoothed
     * value rather than decaying it. Inputs are already floored, so the convex
     * blend stays above the floors too. */
    if (g_beta > 0.0) {
        if (!g_ema_init) {
            g_ld_ema   = ld;
            g_la_ema   = la;
            g_ema_init = true;
        } else {
            g_ld_ema = g_beta * ld + (1.0 - g_beta) * g_ld_ema;
            g_la_ema = g_beta * la + (1.0 - g_beta) * g_la_ema;
        }
        ld = g_ld_ema;
        la = g_la_ema;
    }

    if (L_D) *L_D = ld;
    if (L_A) *L_A = la;
    if (R_D) *R_D = (double)ins_l;       /* split mode: proportional, not absolute */
    if (R_A) *R_A = (double)ins_r;
    return 0;
}
