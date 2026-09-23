#ifndef TIERMEM_CONGEST_CTRL_MATH_H
#define TIERMEM_CONGEST_CTRL_MATH_H

#include <stdint.h>

/* Controller Schmitt-trigger states. */
enum { CC_BALANCED = 0, CC_ARMED = 1 };

static inline double cc_clamp(double x, double lo, double hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Signed normalized latency error. >0 means the local tier is congested
 * (L_D > L_A); <0 means it is under-loaded relative to remote. */
static inline double cc_error(double L_D, double L_A)
{
    double m = L_D > L_A ? L_D : L_A;
    if (m <= 0.0) return 0.0;
    return (L_D - L_A) / m;
}

/* Schmitt trigger: ARMED while imbalance is live, BALANCED once converged.
 * delta_out > delta_in gives hysteresis; |e| is used so it re-arms in either
 * direction (over- or under-loaded local tier). */
static inline int cc_schmitt_next(int state, double e,
                                  double delta_in, double delta_out)
{
    double ae = e < 0.0 ? -e : e;
    if (state == CC_BALANCED)
        return (ae > delta_out) ? CC_ARMED : CC_BALANCED;
    return (ae < delta_in) ? CC_BALANCED : CC_ARMED;   /* ARMED */
}

/* Warm start: on fresh congestion (no interleave applied yet) jump r to a seed
 * so the first response is not a slow ramp from zero. Idempotent once r > 0. */
static inline double cc_warm_start(double r, double e, double r0)
{
    return (r == 0.0 && e > 0.0) ? r0 : r;
}

/* Proportional update with per-epoch step clamp and [0,1] saturation. */
static inline double cc_update_r(double r, double e, double kp, double dr_max)
{
    double dr = cc_clamp(kp * e, -dr_max, dr_max);
    return cc_clamp(r + dr, 0.0, 1.0);
}

/* Interleave level k = round(r*G), in [0, G]. */
static inline int cc_k_for_ratio(double r, int G)
{
    int k = (int)(r * (double)G + 0.5);
    if (k < 0) k = 0;
    if (k > G) k = G;
    return k;
}

/* Per-object interleave level for bandwidth-proportional demotion.
 *
 * The controller's scalar r in [0,1] sets the global demotion intensity; an
 * object spills a fraction of its pages that scales with its bandwidth density
 * (bw_ema/nr_pages) relative to the hottest local object (density_max). Since
 * the spilled-page count is (k/G)*nr_pages ~= r*(density/density_max)*nr_pages
 * = r*bw_ema/density_max, the demoted-page count is proportional to bw_ema.
 *
 * Reduces to cc_k_for_ratio(r, G) (the uniform scheme) when every object has
 * the same density. Returns 0 on any degenerate input (r<=0, no bandwidth,
 * density_max<=0), so cold objects are never demoted by the controller. */
static inline int cc_k_for_object(double r, int G,
                                  double density, double density_max)
{
    if (G <= 0 || r <= 0.0 || density <= 0.0 || density_max <= 0.0)
        return 0;
    double frac = r * (density / density_max);   /* in (0, 1] */
    int k = (int)(frac * (double)G + 0.5);
    if (k < 0) k = 0;
    if (k > G) k = G;
    return k;
}

/* Round n down to a power of two, clamped to [1, max_pow2]. */
static inline int cc_round_pow2(int n, int max_pow2)
{
    int p = 1;
    if (n < 1) return 1;
    while ((p << 1) <= n && (p << 1) <= max_pow2) p <<= 1;
    return p;
}

/* Count of pages i in [0, nr_pages) whose slot (i & (G-1)) < k. */
static inline uint64_t cc_pages_remote_for_k(uint64_t nr_pages, int k, int G)
{
    uint64_t full = nr_pages / (uint64_t)G;
    uint64_t rem  = nr_pages % (uint64_t)G;
    uint64_t cnt  = full * (uint64_t)k;
    cnt += (rem < (uint64_t)k) ? rem : (uint64_t)k;
    return cnt;
}

/* True if page i lies in the migration delta between k_applied and k_target.
 * Uses the nested-interval property: the remote set at level k is a subset of
 * the remote set at level k+1, so the delta is a single contiguous slot range. */
static inline int cc_slot_in_delta(uint64_t i, int k_applied, int k_target, int G)
{
    int s  = (int)(i & (uint64_t)(G - 1));
    int lo = k_applied < k_target ? k_applied : k_target;
    int hi = k_applied < k_target ? k_target : k_applied;
    return s >= lo && s < hi;
}

#endif /* TIERMEM_CONGEST_CTRL_MATH_H */
