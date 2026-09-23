# Reproducing the paper results

## 1. Configure the machine

Copy `configs/site.example.conf` to `configs/site.conf` and set the paths once. Select it explicitly when it is not at the default path:

```sh
export SPECTRA_CONFIG=/absolute/path/to/site.conf
```

Run `./scripts/prepare.sh --check` after booting each baseline kernel. The check is read-only and reports missing tools, kernel mismatches, and unavailable workload inputs before an experiment changes the machine. Every option also checks CPU identity and topology; a mismatch only warns and continues. Follow `artifact/CPU-REQUIREMENTS.md` to adapt `src/libtiermem/monitor.c` and CPU bindings in `experiments/` when necessary.

## 2. Build

```sh
./scripts/prepare.sh --print-deps
./scripts/prepare.sh
```

`prepare.sh --install-deps` invokes `apt-get` and therefore requires network access and root. The printed package list can instead be installed through the site's normal provisioning process. Without arguments, `prepare.sh` also checks all three kernel images and the configured macrobenchmark paths; it asks before running the third-party installation scripts.

## 3. Kick the tires

```sh
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh smoke
```

This checks allocation interception, PMU sampling, object attribution, and page placement.

## 4. Microbenchmarks

The paper uses five pairs, ten placements/baselines (eight on `6.3.0-colloid-alto`, plus Memtis and MTTM on their own kernels), and three repetitions. Boot the kernel needed by the chosen baseline, then run one cell at a time:

```sh
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh microbench libtiermem sdprd-rr
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh microbench static-remote sdprd-rr
```

Static oracle variants are selected through `W1_MEM_NODE` and `W2_MEM_NODE`:

```sh
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh microbench static-w1 sdprd-rr
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh microbench static-w2 sdprd-rr
```

Set `SPECTRA_RUN_ID=run1`, `run2`, or `run3` to keep repetitions separate. The default refuses to overwrite a nonempty result directory.

## 5. Congestion control

```sh
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh congestion colloid
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh congestion mtcolloid
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh congestion libtiermem
```

These commands use the `sdprw-rrw` pair. The Spectra configuration enables `TIERMEM_CONGEST_CTRL=1` through the wrapper.

## 6. Macrobenchmarks

Supply the licensed/external workloads described in `third_party/README.md`, then run a baseline and pair:

```sh
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh macrobench libtiermem llama-faissflat
sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh macrobench tpp llama-faissflat
```

The five paper pairs are `faissbench-spec607`, `llama-faissflat`, `spec619-llama`, `vht-dramhit`, and `vht-faissbench`.

## 7. Aggregate and plot

Raw-result extractors remain next to their suites. The top-level analysis command regenerates plots from processed data placed under the generated result directory:

```sh
./scripts/analyze.sh generated
```

Extractors write to `results/generated/data`; plotting writes to `results/generated/figures`. To process one suite without plotting, run `./scripts/analyze.sh extract <suite>` with `microbench`, `congestion`, or `macrobench` as the suite. Omitting the suite processes all three.
