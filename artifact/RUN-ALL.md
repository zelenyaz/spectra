# Unattended full experiment run

`scripts/run-all.py` executes the complete microbenchmark, congestion-control, and macrobenchmark matrix. It uses a systemd service to resume after kernel reboots and stores progress after every successful cell.

## Matrix

| Kernel | Microbench | Congestion | Macrobench | Cells per repetition |
| --- | ---: | ---: | ---: | ---: |
| `6.3.0-colloid-alto` | 5 pairs × 4 static placements + TPP, Colloid, Alto, Spectra | Colloid, Spectra | 5 pairs × the same 8 baselines | 82 |
| `5.15.19-htmm` | 5 pairs × Memtis | mtcolloid | 5 pairs × Memtis | 11 |
| `5.15.145-mttm` | 5 pairs × MTTM | — | 5 pairs × MTTM | 10 |

The default is **3 repetitions = 309 cells**. The five microbench pairs are listed in `experiments/microbench/run-suite.sh`. The five macrobench pairs are listed in `experiments/macrobench/run-suite.sh` and plotted by `plots/macrobench-per-workload.py`. Congestion uses the `sdprw-rrw` pair. Each cell calls the existing `scripts/run.sh` interface, so results retain the established `runN` layout.

## Prepare once

1. Configure `configs/site.conf` (or provide `--config`) and run `./scripts/prepare.sh` with `SPECTRA_CONFIG` set when needed. It builds the code, checks dependencies, and offers to install missing kernels and prepare macrobenchmarks.
2. Supply the licensed SPEC workload described in `third_party/README.md`; the macrobenchmark preparation script does not provide these inputs.
3. Check that `experiments/common/kernel_version_switcher.sh` is suitable for this machine. The runner calls it with `-s <kernel-release>` to select the next boot kernel. The three `/boot/vmlinuz-<kernel-release>` images must exist.
4. Choose unused run numbers. The runner refuses any nonempty result directory in the planned matrix before starting.

```sh
./scripts/run-all.py plan --runs 3
sudo ./scripts/run-all.py install
sudo ./scripts/run-all.py start --runs 3 --run-start 1 \
  --config /absolute/path/to/site.conf
```

`--runs N` sets repetitions per cell; `--run-start K` names them `runK` through `run(K+N-1)`. `--sleep` sets seconds between cells (default 10), `--boot-delay` sets seconds before reboot (default 10), and `--max-attempts` sets per-cell attempts (default 3). `start` validates all macrobenchmark inputs with dry runs, checks the kernel images and result collisions, then starts the service. The service checks the full system on each required kernel before running that kernel's cells.

The scheduler runs all cells on one kernel before switching to the next. The host reboots twice when started on `6.3.0-colloid-alto`; it may reboot once more if started on another kernel. Keep the repository, configured paths, and `/var/lib/spectra-all-runner` available at boot.

## Monitor and recover

```sh
sudo ./scripts/run-all.py status
sudo journalctl -u spectra-all-runner.service -f
sudo tail -f /var/lib/spectra-all-runner/driver.log
```

The state file is `/var/lib/spectra-all-runner/state.json`. Per-cell logs are under its `logs/` directory. A successful cell advances the saved index. A failed or interrupted cell's partial result is moved to `results/generated/failed/all-in-one/` before retrying, preserving the normal result directory for the next attempt. The runner checks both workload exit codes recorded in `duration.txt` for dual-process cells. Spectra cells also require initialization records for each workload and at least one epoch across their logs; congestion cells additionally require an enabled congestion controller. Keep `TIERMEM_LOG_LEVEL` at 2 or higher (the suite default is 3) to retain this evidence. Spectra initialization failures terminate the application with a nonzero exit code. After repeated failure the campaign stops with `status=failed`, leaving the error and logs for review.

```sh
sudo ./scripts/run-all.py stop       # pause an active campaign
sudo ./scripts/run-all.py resume     # continue after fixing a failure or pause
sudo ./scripts/run-all.py reset      # archive state after stopping/completion
sudo ./scripts/run-all.py uninstall  # remove the boot service
```

`resume` grants three more attempts after an exhausted cell. `reset` preserves all raw results and logs; use a fresh `--run-start` for another campaign.

## Plot the campaign

Once `status` reports `complete`, convert the raw logs into tables and figures:

```sh
./scripts/analyze.sh generated
```

This extracts all three suites into `results/generated/data/`, then writes the six paper figures to `results/generated/figures/`. The README lists which processed table feeds which figure.

Figures 6, 7, 9, and 11 aggregate the whole result tree, so every repetition contributes. Figures 8 and 10 are time series from one repetition. By default, each uses the latest complete `runN` in its suite (the largest `N`); if that run is incomplete, analysis tries earlier runs in descending order.

| Paper figure | Generated PDF | Override |
| --- | --- | --- |
| Figure 8 | `congc.pdf` | `SPECTRA_CONGC_RUN_ID` |
| Figure 10 | `macro-converge.pdf` | `SPECTRA_MACRO_RUN_ID` |

When several campaigns share one result tree, select the intended repetitions explicitly:

```sh
SPECTRA_CONGC_RUN_ID=run8 SPECTRA_MACRO_RUN_ID=run7 ./scripts/analyze.sh generated
```

Both overrides accept any `runN` present in the tree; an incomplete choice is an error rather than a silent fallback.
