#!/usr/bin/env python3
"""
Micro-benchmark steady-state iMC bandwidth utilization, per pair.

One subplot per pair (1x5 row, double-column). Each bar is one baseline; the
bar is stacked into four segments: primary-tier read, primary-tier write,
expansion-tier read, expansion-tier write (100 GB/s). The primary-tier
segments are the useful fast-tier bandwidth; total height is the achieved DRAM
bandwidth.
Data: data/eval-micro-bw.md (experiments/microbench/scripts/extract_eval_data.py).
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch

# ── Layout knobs ──────────────────────────────────────────────────────────────
FIG_W_IN = 4
FIG_H_IN = 1
WSPACE = 0.12
BW_UNIT_GBPS = 100.0

# ── Font sizes (pt) ─────────────────────────────────────────────────────────
FS_BASE = 9
FS_AXIS_LABEL = 9
FS_TITLE = 9
FS_TICK = 6
FS_LEGEND = 9

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "font.size": FS_BASE,
    "axes.labelsize": FS_AXIS_LABEL,
    "axes.titlesize": FS_TITLE,
    "xtick.labelsize": FS_TICK,
    "ytick.labelsize": 9,
    "legend.fontsize": FS_LEGEND,
    "figure.dpi": 300,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.5,
    "ytick.major.width": 0.5,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
    "hatch.linewidth": 0.6,
})

REPO = Path(__file__).resolve().parents[1]
DATA_DIR = Path(os.environ.get("SPECTRA_DATA_DIR", REPO / "results/generated/data"))
OUT_DIR = Path(os.environ.get("SPECTRA_FIG_DIR", REPO / "results/generated/figures"))
DATA_FILE = DATA_DIR / "eval-micro-bw.md"
OUT_PDF = OUT_DIR / "microbench-bw.pdf"

CASES = ["sdrd-chksdrd", "sdprd-rr", "sdrdpf-rr", "sdrd-sdrw", "sdrd-sdwr"]
CASE_TITLES = {
    "sdrd-chksdrd": "sd-rd\n+ chk-reuse",
    "sdprd-rr":     "lsd-rd\n+ rnd-rd",
    "sdrdpf-rr":    "swpf-rd\n+ rnd-rd",
    "sdrd-sdrw":    "sd-rd\n+ sd-rw",
    "sdrd-sdwr":    "sd-rd\n+ sd-wr",
}
SUBFIG_LABELS = ["(a)", "(b)", "(c)", "(d)", "(e)"]
BASELINES = ["TPP", "Colloid", "Alto", "Memtis", "MTTM", "Spectra"]
BASE_KEY = {"TPP": "tpp", "Colloid": "colloid", "Alto": "alto",
            "Memtis": "memtis", "MTTM": "mttm", "Spectra": "libtiermem"}

SEG = ["local-rd", "local-wr", "remote-rd", "remote-wr"]
SEG_COLOR = {"local-rd": "#08519c", "local-wr": "#6baed6",
             "remote-rd": "#a63603", "remote-wr": "#fdae6b"}
SEG_HATCH = {"local-rd": "", "local-wr": "////",
             "remote-rd": "", "remote-wr": "////"}
SEG_LABEL = {"local-rd": "P-tier Read", "local-wr": "P-tier Write",
             "remote-rd": "E-tier Read", "remote-wr": "E-tier Write"}


def parse_data(path):
    """combo -> baseline -> (n0_rd, n0_wr, n1_rd, n1_wr)."""
    out = {}
    for line in path.read_text().splitlines():
        if not line.startswith("|"):
            continue
        if re.match(r"^\|[\s\-:|]+\|$", line):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if cells[0] == "combo":
            continue
        combo, base = cells[0], cells[1]
        out.setdefault(combo, {})[base] = tuple(float(c) for c in cells[2:6])
    return out


def main():
    data = parse_data(DATA_FILE)
    fig, axes = plt.subplots(1, 5, figsize=(FIG_W_IN, FIG_H_IN), sharey=True,
                             constrained_layout=False)
    x = np.arange(len(BASELINES))
    bar_w = 0.72

    for ax, combo, sub in zip(axes, CASES, SUBFIG_LABELS):
        for xi, disp in zip(x, BASELINES):
            v = data[combo][BASE_KEY[disp]]   # n0_rd, n0_wr, n1_rd, n1_wr
            seg_vals = {"local-rd": v[0] / BW_UNIT_GBPS,
                        "local-wr": v[1] / BW_UNIT_GBPS,
                        "remote-rd": v[2] / BW_UNIT_GBPS,
                        "remote-wr": v[3] / BW_UNIT_GBPS}
            bottom = 0.0
            for s in SEG:
                ax.bar(xi, seg_vals[s], width=bar_w, bottom=bottom,
                       color=SEG_COLOR[s], hatch=SEG_HATCH[s],
                       edgecolor="#222222", linewidth=0.4, zorder=3)
                bottom += seg_vals[s]
        ax.set_xticks(x)
        ax.set_xticklabels(BASELINES, rotation=40, ha="right")
        ax.set_xlabel(f"{sub} {CASE_TITLES[combo]}", fontsize=FS_TITLE, labelpad=2)
        ax.set_ylim(0, 3)
        ax.set_yticks([0, 1, 2, 3])
        ax.yaxis.grid(True, linestyle="--", alpha=0.3, zorder=1)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    axes[0].set_ylabel("iMC BW (100 GB/s)")

    legend_handles = [
        Patch(facecolor=SEG_COLOR[s], hatch=SEG_HATCH[s], edgecolor="#222222",
              linewidth=0.4, label=SEG_LABEL[s]) for s in SEG
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               bbox_to_anchor=(0.5, 1.15), ncol=4, frameon=False,
               handlelength=1, handletextpad=0.4, columnspacing=1.4)

    plt.subplots_adjust(left=0.07, right=0.99, top=0.85, bottom=0.30, wspace=WSPACE)
    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
