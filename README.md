# Spectra artifact

This repository is the artifact for **“Spectra: Bandwidth-Faithful Data Placement for Tiered Memory”** (ACM ATC 2026). Spectra is an `LD_PRELOAD` library that estimates each allocated object's actual memory access intensity using Intel PEBS, offcore-response, and integrated memory controller counters, then migrates pages between two NUMA memory tiers.

## Artifact contents

| Path | Purpose | Paper relationship |
| --- | --- | --- |
| `src/libtiermem/` | Spectra implementation | Sections 4 and 5 |
| `benchmarks/mbench/` | Source of the configurable two-object microbenchmark | Section 6.2 |
| `experiments/microbench/` | Microbenchmark and congestion-control runners | Figures 6–8 and Table 2 |
| `experiments/macrobench/` | Real-application runner | Figures 9–11 |
| `experiments/common/` | Shared baseline setup, workload orchestration, and monitoring | Experimental methodology |
| `plots/` | Scripts that regenerate paper figures | Figures 6–11 |
| `third_party/` | Baseline and workload | Dependency checklist |

## Quick start

Build Spectra and run the hardware smoke test on a supported dual-socket Intel Sapphire Rapids machine:

```sh
cp configs/site.example.conf configs/site.conf
# Edit configs/site.conf for the machine.
./scripts/prepare.sh --build
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh smoke
```

The test uses two 256 MiB objects for about 16 seconds. It requires root because Spectra opens system-wide PEBS and uncore PMU events. A successful run produces at least one `=== Epoch` record in `results/generated/smoke/stderr.log`.

Building alone needs x86-64 Linux, GCC/G++, GNU Make, and `libnuma-dev`, and changes no system settings; `plots/` additionally needs Python 3, NumPy, and Matplotlib.

## Reproducing paper results

### Full experiment

The full matrix has **103 cells per repetition** across three kernels. One repetition takes about **6 hours**; the default three repetitions run **309 cells**. Each microbenchmark and macrobenchmark cell combines one of five workload pairs with a baseline below.

| Kernel | Microbenchmarks | Congestion control | Macrobenchmarks | Cells per repetition |
| --- | --- | --- | --- | ---: |
| `6.3.0-colloid-alto` | 5 pairs × `static-local`, `static-remote`, `static-w1`, `static-w2`, `tpp`, `colloid`, `alto`, `libtiermem` (Spectra) | `colloid`, `libtiermem` | 5 pairs × the same 8 baselines | 82 |
| `5.15.19-htmm` | 5 pairs × `memtis` | `mtcolloid` | 5 pairs × `memtis` | 11 |
| `5.15.145-mttm` | 5 pairs × `mttm` | — | 5 pairs × `mttm` | 10 |

The microbenchmark pairs are `sdrd-chksdrd`, `sdprd-rr`, `sdrdpf-rr`, `sdrd-sdrw`, and `sdrd-sdwr`. The macrobenchmark pairs are `faissbench-spec607`, `llama-faissflat`, `spec619-llama`, `vht-dramhit`, and `vht-faissbench`. Congestion control uses the `sdprw-rrw` pair.

### Set up

On a supported dual-socket Sapphire Rapids machine, copy `configs/site.example.conf` to `configs/site.conf` and set the machine and workload paths. Supply the licensed SPEC CPU 2017 installation and FAISS index and query files described in [`third_party/README.md`](third_party/README.md). Then run:

```sh
./scripts/prepare.sh
```

This builds the binaries, offers to prepare missing kernels and workloads, and checks experiment prerequisites. Full experiments require root and change system settings; use a dedicated test machine.

### Run the full matrix

`run-all.py` runs the cells through a systemd service, records progress, and resumes after the required kernel reboots:

```sh
./scripts/run-all.py plan --runs 3
sudo ./scripts/run-all.py install
sudo ./scripts/run-all.py start --runs 3
sudo ./scripts/run-all.py status
```

The runner reads `configs/site.conf` by default. See [`artifact/RUN-ALL.md`](artifact/RUN-ALL.md) for monitoring and recovery.

### Plot

After `status` reports `complete`, extract the raw logs and regenerate the paper figures:

```sh
./scripts/analyze.sh generated
```

Processed tables go to `results/generated/data/` and figures to `results/generated/figures/`.

| Paper result | Processed data file | Generated PDF |
| --- | --- | --- |
| Figure 6: microbenchmark speedup | `eval-micro.md` | `microbench.pdf` |
| Figure 7: microbenchmark bandwidth utilization | `eval-micro-bw.md` | `microbench-bw.pdf` |
| Figure 8: congestion control | `eval-congc.md` | `congc.pdf` |
| Figure 9: macrobenchmark normalized duration | `macrobench-duration-summary.md` | `macrobench-per-workload.pdf` |
| Figure 10: placement convergence and migrations | `eval-macro-converge.md` | `macro-converge.pdf` |
| Figure 11: tiering CPU overhead | `eval-cpu-overhead.md` | `cpu-overhead.pdf` |
| Table 2: per-object attribution and bandwidth accuracy | `eval-micro-attrib.md` | — (table only) |

### Run one experiment

Boot the kernel for the chosen baseline; `list` shows valid pairs, baselines, and required kernels. Run one cell with:

```sh
./scripts/run.sh list
sudo ./scripts/run.sh microbench <baseline> <pair>
sudo ./scripts/run.sh congestion <colloid|mtcolloid|libtiermem>
sudo ./scripts/run.sh macrobench <baseline> <pair>
```

Results are saved under `results/generated/raw/`. The default repetition is `run1`; for another, use `sudo SPECTRA_RUN_ID=run2 ./scripts/run.sh ...`.

## Original evaluation environment

- 2 × Intel Xeon Gold 5420+ (Sapphire Rapids), 28 cores per socket, SMT off
- 504 GiB DDR5 in two NUMA nodes
- node 0 as the primary tier and node 1 as the expansion tier
- workloads pinned to node 0 cores
- Ubuntu 24.04.2 LTS
- Spectra/TPP/Colloid/Alto kernel: `6.3.0-colloid-alto`
- Memtis kernel: `5.15.19-htmm`
- MTTM kernel: `5.15.145-mttm`

Spectra's PMU event encodings and topology limits currently target this Sapphire Rapids configuration. Every `prepare.sh` invocation checks for Sapphire Rapids, exactly two sockets and NUMA nodes, and 28 cores per socket. Mismatches produce a warning and preparation continues; adapting `src/libtiermem/monitor.c` and CPU bindings in `experiments/` may be necessary. See [`artifact/CPU-REQUIREMENTS.md`](artifact/CPU-REQUIREMENTS.md) for the full CPU topology assumptions and the raw PMU event configurations used by Spectra.

## Resource expectations

| Evaluation | Wall time | Memory | Special requirements |
| --- | ---: | ---: | --- |
| Build and plotting | <2 min | <1 GiB | Build and Python plotting dependencies |
| Spectra smoke | ~16 s | ~1 GiB | Sapphire Rapids, 2 NUMA nodes, root/PMU access |
| One microbenchmark run | 30–240 s | 8–12 GiB | Required baseline kernel |
| One congestion run | ~80 s | 8–12 GiB | Colloid/Memtis/Spectra support |
| One macrobenchmark run | 3–15 min | 8–25 GiB | Workload-specific data and software |
| Full matrix, three repetitions | ~18 hours plus reboots | up to 30 GiB | Three kernels and reboots |

## Safety warning

Full experiment scripts are privileged and deliberately modify kernel modules, NUMA balancing, cgroups, swap state, transparent huge pages, turbo settings, and `/proc`/`/sys` tunables; they also drop the page cache. Run them only on a dedicated test machine. Cleanup handlers restore temporary settings where possible, but a reboot into the intended baseline kernel is the clean boundary between groups.

## Non-redistributable inputs

SPEC CPU 2017 cannot be redistributed. The artifact provides a wrapper and exact benchmark/configuration identifiers, but evaluators must supply their licensed installation through `SPEC_DIR`. The Qwen model and the large FAISS indexes also remain external due to size and upstream licensing. Their paths are configured once in `configs/site.conf`; see [`third_party/README.md`](third_party/README.md). The included `mbench` source can be built without any proprietary input.

## Availability

MIT licensed.
