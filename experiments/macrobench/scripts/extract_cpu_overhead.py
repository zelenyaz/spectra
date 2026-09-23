#!/usr/bin/env python3
"""
Extract tiering CPU overhead per macrobench pair, for three representative
mechanisms:
  - TPP      : kswapd0 demotion daemon (%CPU), from tpp/kswapd0-pidstat.log
  - Memtis   : ksamplingd sampling daemon (%CPU), from memtis/memtis-ksamplingd.pidstat.log
  - libtiermem: monitor-thread CPU usage (%), from libtiermem/w1/stderr.log

pidstat output can include or omit an AM/PM column. The final three fields are
always %CPU, CPU, and Command; we average matching daemon rows over runs.
"""

import os
import re
import statistics as st
from glob import glob
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = Path(os.environ.get(
    "SPECTRA_MACRO_RESULTS", REPO / "results/generated/raw/macrobench/results"))
DATA = Path(os.environ.get(
    "SPECTRA_DATA_DIR", REPO / "results/generated/data"))
DATA.mkdir(parents=True, exist_ok=True)

COMBOS = ["faissbench-spec607", "llama-faissflat", "spec619-llama",
          "vht-dramhit", "vht-faissbench"]
COMBO_DISP = {
    "faissbench-spec607": "SPEC607+FA-poly",
    "llama-faissflat":    "Llama+FA-flat",
    "spec619-llama":      "SPEC619+Llama",
    "vht-dramhit":        "VHT+DRAMhit",
    "vht-faissbench":     "VHT+FA-poly",
}


def pidstat_mean_cpu(path, daemon):
    """Mean %CPU over rows whose command (last field) is `daemon`."""
    vals = []
    if not Path(path).exists():
        return None
    for line in Path(path).read_text(errors="replace").splitlines():
        f = line.split()
        if len(f) < 4 or f[-1] != daemon:
            continue
        try:
            vals.append(float(f[-3]))
        except ValueError:
            continue
    return st.mean(vals) if vals else None


def ltm_monitor_usage(path):
    """libtiermem monitor-thread CPU usage % from the exit line."""
    if not Path(path).exists():
        return None
    m = re.search(r"Monitor thread exit: elapsed=[\d.]+s cpu=[\d.]+s usage=([\d.]+)%",
                  Path(path).read_text(errors="replace"))
    return float(m.group(1)) if m else None


def agg(combo, kind):
    """Average a metric over runs for one combo."""
    vals = []
    for run in sorted(glob(str(RESULTS / "run*"))):
        c = Path(run) / combo
        if kind == "tpp":
            v = pidstat_mean_cpu(c / "tpp" / "kswapd0-pidstat.log", "kswapd0")
        elif kind == "memtis":
            v = pidstat_mean_cpu(c / "memtis" / "memtis-ksamplingd.pidstat.log",
                                 "ksamplingd")
        else:
            v = ltm_monitor_usage(c / "libtiermem" / "w1" / "stderr.log")
        if v is not None:
            vals.append(v)
    return (st.mean(vals), len(vals)) if vals else (None, 0)


def main():
    lines = ["# Tiering CPU overhead per macrobench pair (mean %CPU of one core)",
             "# TPP=kswapd0 demotion daemon; Memtis=ksamplingd sampling daemon;",
             "# libtiermem=monitor-thread usage. Averaged over runs.",
             "#",
             "| pair | TPP | Memtis | Spectra |",
             "|---|---:|---:|---:|"]
    tpp_all, mem_all, ltm_all = [], [], []
    for combo in COMBOS:
        t, _ = agg(combo, "tpp")
        m, _ = agg(combo, "memtis")
        l, _ = agg(combo, "ltm")
        if t is None or m is None or l is None:
            raise ValueError(f"missing CPU overhead samples for {combo}: "
                             f"TPP={t}, Memtis={m}, Spectra={l}")
        tpp_all.append(t)
        mem_all.append(m)
        ltm_all.append(l)
        lines.append(f"| {COMBO_DISP[combo]} | "
                     f"{t:.2f} | {m:.2f} | {l:.2f} |")
    lines.append(f"| **mean** | {st.mean(tpp_all):.2f} | "
                 f"{st.mean(mem_all):.2f} | {st.mean(ltm_all):.2f} |")
    (DATA / "eval-cpu-overhead.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
