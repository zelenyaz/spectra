#ifndef TIERMEM_CHA_LAT_H
#define TIERMEM_CHA_LAT_H

#include <stdint.h>

/* Per-tier demand-read access-latency measurement via CHA TOR counters
 * (Little's law: latency = occupancy / inserts, in CHA clocks).
 *
 * split_mode: 1 = even/odd CHA split (even slices measure local, odd measure
 *             remote; avoids the TOR_OCCUPANCY counter-0 conflict without
 *             multiplexing). 0 = open all four events per CHA and time-scale.
 * beta:        EWMA weight on the new per-tier latency sample (0..1); <=0
 *              disables smoothing. Holds the last value across idle-guard epochs.
 * min_inserts: idle guard. An epoch with fewer than this many LOCAL inserts is
 *              reported as untrustworthy (the local tier is idle, no signal).
 *              The REMOTE tier is allowed to fall below this bound, in which
 *              case its latency is unmeasurable and la_fallback is substituted
 *              so the controller can still arm on local load alone.
 * la_fallback: assumed remote-tier latency (CHA clocks) used when the remote
 *              tier has too few inserts to measure. Acts as the arming bar:
 *              the controller spills to remote once L_D rises above it. */
int  cha_lat_init(int split_mode, double beta, uint64_t min_inserts,
                  double la_fallback);
void cha_lat_cleanup(void);

/* Reduce the counter delta accumulated since the previous call into smoothed
 * per-tier latencies (CHA clocks) and per-tier insert magnitudes. Any output
 * pointer may be NULL. Returns 0 on success, -1 on the local-idle guard. */
int  cha_lat_epoch(double *L_D, double *L_A, double *R_D, double *R_A);

#endif /* TIERMEM_CHA_LAT_H */
