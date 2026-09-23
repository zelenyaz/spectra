#!/usr/bin/env python3
"""
Extract per-workload NUMA placement from macrobench numa_maps logs.

numa_maps.log rows: "YYYY-MM-DD HH:MM:SS  N0(GB)  N1(GB)  Total(GB)".
N0 = local/primary tier, N1 = remote/expansion tier.

Emits:
  - data/eval-macro-converge.md  : local-tier-resident GB over time for the
    bandwidth-dominant workload of two representative pairs, per baseline
    (for the convergence time-series figure).
"""

import os
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = Path(os.environ.get(
    "SPECTRA_MACRO_RESULTS", REPO / "results/generated/raw/macrobench/results"))
DATA = Path(os.environ.get(
    "SPECTRA_DATA_DIR", REPO / "results/generated/data"))
DATA.mkdir(parents=True, exist_ok=True)

# pair -> (W1 short, W2 short, bandwidth-dominant slot)
PAIRS = {
    "faissbench-spec607": ("SPEC607", "FAISS-poly", "w2"),
    "llama-faissflat":    ("Llama", "FAISS-flat", "w1"),
    "spec619-llama":      ("SPEC619", "Llama", "w1"),
    "vht-dramhit":        ("VHT", "DRAMHiT", "w2"),
    "vht-faissbench":     ("VHT", "FAISS-poly", "w2"),
}
BASELINES = ["tpp", "colloid", "alto", "memtis", "mttm", "libtiermem"]
ROW_RE = re.compile(r"^\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)")


def parse_numa_maps(path):
    """Return [(n0, n1, total)] samples."""
    out = []
    if not Path(path).exists():
        return out
    for line in Path(path).read_text(errors="replace").splitlines():
        m = ROW_RE.match(line)
        if m:
            out.append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
    return out


def select_run(pairs_to_plot):
    requested = os.environ.get("SPECTRA_MACRO_RUN_ID") or os.environ.get("SPECTRA_MIGRATE_RUN_ID")
    if requested:
        candidates = [RESULTS / requested]
    else:
        candidates = sorted(
            (p for p in RESULTS.glob("run*") if p.is_dir() and p.name[3:].isdigit()),
            key=lambda p: int(p.name[3:]), reverse=True)
    for candidate in candidates:
        if all(parse_numa_maps(candidate / pair / base / PAIRS[pair][2] / "numa_maps.log")
               for pair in pairs_to_plot for base in BASELINES):
            return candidate
    raise FileNotFoundError(f"no complete macro convergence run under {RESULTS}")


def converge(pairs_to_plot):
    """Time-series of bandwidth-dominant workload's local GB per baseline."""
    run_dir = select_run(pairs_to_plot)
    lines = ["# Macrobench convergence: local-tier GB of the bandwidth-dominant",
             f"# workload over time (elapsed since first numa_maps sample), {run_dir.name}.",
             "# Format: 'pair/baseline: t,localGB;t,localGB;...'",
             ""]
    for pair in pairs_to_plot:
        w1n, w2n, dom = PAIRS[pair]
        lines.append(f"## {pair}  (dominant={dom}: {w1n if dom=='w1' else w2n})")
        for base in BASELINES:
            path = run_dir / pair / base / dom / "numa_maps.log"
            s = parse_numa_maps(path)
            if not s:
                continue
            # elapsed index = sample number * ~2s (numa_maps interval = 2s)
            series = ";".join(f"{i*2.0:.0f},{n0:.3f}" for i, (n0, _, _) in enumerate(s))
            lines.append(f"{base}: {series}")
        lines.append("")
    (DATA / "eval-macro-converge.md").write_text("\n".join(lines) + "\n")
    print("wrote data/eval-macro-converge.md")


if __name__ == "__main__":
    converge(["llama-faissflat", "spec619-llama"])
