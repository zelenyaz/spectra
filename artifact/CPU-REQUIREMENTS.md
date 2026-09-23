# Spectra CPU Requirements

Spectra's `LD_PRELOAD` library (`src/libtiermem/`) drives its tiering decisions entirely from hardware performance counters. All event programming lives in `src/libtiermem/monitor.c`; this document lists what the CPU must provide.

## Platform

| Requirement | Value | Source |
| --- | --- | --- |
| Microarchitecture | Intel Sapphire Rapids (server) — raw event encodings below are SPR-specific | `monitor.c:58`, `monitor.c:927` |
| Sockets / NUMA nodes | 2 (`NUM_NODES`); node 0 = local DRAM, node 1 = CXL/slow tier | `include/config.h:9-11` |
| Cores per socket | 28 (`NUM_CPUS_PER_NODE`), no SMT assumed in the CPU map | `include/config.h:12` |
| iMC channels per socket | 8 (`NUM_IMC_CHANNELS`), exposed as `uncore_imc_0..7` | `include/config.h:16`, `monitor.c:874` |
| PEBS | Required, `precise_ip = 3` on all sampled events | `monitor.c:186`, `:235`, `:294` |
| Uncore iMC PMU | Required for write-bandwidth ground truth (non-fatal if absent) | `monitor.c:1824-1826` |

`NUM_NODES`, `NUM_CPUS_PER_NODE`, and `NUM_IMC_CHANNELS` are compile-time constants; a host with a different topology needs a rebuild after editing `src/libtiermem/include/config.h`.

## Core PMU events (PEBS, sampling)

Opened system-wide (`pid = -1`) on every node-0 CPU (0–27), each with its own 512 KiB ring buffer (`RING_BUFFER_PAGES = 128`), `exclude_kernel = 1`, `exclude_hv = 1`.

| Event | `attr.config` | Encoding | Sample period (env override) | Purpose |
| --- | --- | --- | --- | --- |
| `MEM_LOAD_RETIRED.L3_MISS` | `0x20D1` | EventCode `0xD1`, UMask `0x20` | 10007 (`TIERMEM_PEBS_PERIOD`) | Per-object read-miss attribution |
| `MEM_TRANS_RETIRED.STORE_SAMPLE` | `0x02CD` | EventCode `0xCD`, UMask `0x02` | 100003 (`TIERMEM_STORE_PERIOD`) | Store attribution (samples carry `PERF_SAMPLE_WEIGHT`) |
| `MEM_INST_RETIRED.ALL_LOADS` | `0x81D0` | EventCode `0xD0`, UMask `0x81` | 1000003 (`TIERMEM_ALLLOAD_PERIOD`) | Splits SW vs HW prefetch traffic (counts `PREFETCH*` uops) |

Sample layout: `PERF_SAMPLE_IP | TID | ADDR | CPU` for the load and all-loads events, plus `PERF_SAMPLE_WEIGHT` for the store event (`monitor.c:63-82`). All three are opened at init and are fatal on failure (`monitor.c:1806-1816`).

## Offcore Response events (counting, per process)

Four `OFFCORE_RESPONSE` events (`attr.config = 0x012A`, encoding in `config1`) are opened per process with `inherit = 1`, so they must be opened before the application creates threads (`monitor.c:165`, `ocr_self_init`).

| Event | `config1` | Purpose |
| --- | --- | --- |
| `DEMAND_DATA_RD` + L3 miss | `0x3FFFC00001` | Demand read traffic |
| `DEMAND_RFO` + L3 miss | `0x3FFFC00002` | Demand write (RFO) traffic |
| Total data prefetch + L3 miss | `0x3FFFC04410` | HW (L1/L2 data-side) **and** SW prefetches; the split is recovered from `ALL_LOADS` samples |
| `HW_RFO_PF` + L3 miss | `0x3FFFC00020` | Prefetched RFO traffic |

Read with `PERF_FORMAT_TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING` and rescaled by enabled/running, so multiplexing is tolerated but degrades accuracy. They are read every `TIERMEM_OCR_INTERVAL` ms (default 100) and at each epoch boundary. In multi-process mode each process opens its own set and reports deltas through shared memory (`MAX_PROCS = 64`).

## Uncore iMC events (counting, per channel)

For each of the 2 nodes × 8 channels, opened on the channel's cpumask CPU with `attr.type` = `uncore_imc_<ch>` (base type read from `/sys/devices/uncore_imc_0/type`, channel `N` = base + N):

| Event | `attr.config` | Purpose |
| --- | --- | --- |
| `UNC_M_CAS_COUNT.WR` | `0xF005` | Ground-truth write bandwidth (each CAS = 64 B) |
| `UNC_M_CAS_COUNT.RD` | `0xCF05` | Diagnosis/logging only |

Failure here is non-fatal: Spectra keeps running with `store_hit_load = 0` (`monitor.c:1824-1826`).

## Optional: CHA latency congestion control

Enabled only with `TIERMEM_CONGEST_CTRL=1` (`congest_ctrl_init` in `monitor_init`). It programs per-CHA `UNC_CHA_TOR_INSERTS` (EventCode `0x35`) and `UNC_CHA_TOR_OCCUPANCY` (EventCode `0x36`, counter 0 only) with base umask `0x01` and umask_ext `0xC816FE` (local) / `0xC8177E` (remote), in config bits 32–55 (`cha_lat.c:14-37`). Failure leaves the controller disabled.

## Counter budget and permissions

- Per logical CPU: 3 PEBS events plus 4 per-process OCR events share the general-purpose counters. Keep other PMU consumers off the machine (`perf`, NMI watchdog, other profilers) or the OCR events will multiplex.
- `/proc/sys/kernel/perf_event_paranoid` must be `-1` (checked by `scripts/prepare.sh:140`) or the run must be root: system-wide per-CPU PEBS and uncore events are otherwise rejected.
- Uncore access additionally needs `CAP_PERFMON`/root and the `uncore_imc_*` PMUs exposed under `/sys/devices/`.
