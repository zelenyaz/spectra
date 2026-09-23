#ifndef TIERMEM_REGULARITY_H
#define TIERMEM_REGULARITY_H

#include <stdint.h>

#include "config.h"

#define REG_NBINS        41
#define REG_MIN_SAMPLES  8

/* Linear stride binning. `d` is the signed stride in 4 KiB pages between two
 * consecutive sample addresses within the same object. `obj_size` is the
 * enclosing object's size in bytes. The range [-obj_size/PAGE, +obj_size/PAGE]
 * pages is divided evenly into REG_NBINS bins; out-of-range strides clamp to
 * the end bins. */
static inline int linear_stride_bin(int64_t d, uint64_t obj_size)
{
    int64_t half = (int64_t)(obj_size >> TM_PAGE_SHIFT);
    if (half < 1) half = 1;
    if (d >=  half) return REG_NBINS - 1;
    if (d <= -half) return 0;
    int64_t idx = ((d + half) * REG_NBINS) / (2 * half);
    if (idx < 0) idx = 0;
    if (idx >= REG_NBINS) idx = REG_NBINS - 1;
    return (int)idx;
}

/* Regularity in [0, 1]. Fraction of samples covered by the most popular
 * stride bin. 1 = one stride explains all samples; 0 = too few samples. */
static inline double regularity(const uint32_t bins[REG_NBINS])
{
    uint64_t total = 0;
    uint32_t top1 = 0, top2 = 0;
    for (int i = 0; i < REG_NBINS; i++) {
        uint32_t c = bins[i];
        total += c;
        if (c > top1) {
            top2 = top1;
            top1 = c;
        } else if (c > top2) {
            top2 = c;
        }
    }
    if (total < REG_MIN_SAMPLES) return 0.0;
    return (double)(top1 + top2) / (double)total;
    // return (double)(top1) / (double)total;
}

static inline double regularity_theta_gate(double value, double theta)
{
    if (value < theta)
        return 0.0;
    if (value > 1.0 - theta)
        return 1.0;
    return value;
}

#endif /* TIERMEM_REGULARITY_H */
