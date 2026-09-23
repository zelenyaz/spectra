#ifndef TIERMEM_CONGEST_CTRL_H
#define TIERMEM_CONGEST_CTRL_H

#include <stdbool.h>
#include <stdint.h>

/* Latency-based congestion controller. Parses TIERMEM_CC_* env vars and (when
 * enabled) opens CHA latency events. No-ops entirely when disabled, so the
 * default libtiermem placement path is unchanged. */
int  congest_ctrl_init(void);
void congest_ctrl_cleanup(void);
bool congest_ctrl_enabled(void);

/* Cumulative page-migration counts driven by the controller in THIS process
 * (demoted = local->remote spills, promoted = remote->local when the interleave
 * level is lowered). Either pointer may be NULL. */
void congest_ctrl_get_stats(uint64_t *pages_demoted, uint64_t *pages_promoted);

/* Leader-only: measure per-tier latency, advance the controller state machine,
 * and compute this epoch's global decision. Returns true if the controller owns
 * placement (caller must skip the base rank/migrate path), writing the global
 * demotion intensity *r in [0,1], the global density reference *density_max
 * (max bw_ema/nr_pages over local objects), and the interleave granularity *G.
 * Demotion is bandwidth-proportional: each object's level is derived per-object
 * from (r, density_max) so high-bandwidth objects shed more pages. T = epoch
 * duration (s). Followers never call this; they receive (owns, r, density_max,
 * G) via shm. */
bool congest_ctrl_decide(double T, double *r, double *density_max, int *G);

/* Any process: bandwidth-proportionally interleave THIS process's own
 * local-resident objects, pausing only its own threads. Each object spills to
 * k_i = cc_k_for_object(r, G, bw_ema/nr_pages, density_max). disable_mon
 * controls whether the leader's PMU sampling is paused around the migration
 * (true in single-process mode, false in multi-process where monitoring keeps
 * running, matching migrate_execute_own_pending). */
void congest_ctrl_migrate_local(double r, double density_max, int G,
                                bool disable_mon);

#endif /* TIERMEM_CONGEST_CTRL_H */
