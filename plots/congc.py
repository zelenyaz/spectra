#!/usr/bin/env python3
"""
Congestion control under primary-tier bandwidth saturation (stride+rand pair).

Both workloads start on the local tier, which the high-intensity stride
read-write workload saturates. Three panels over wall-clock time:
  (a) high-BW workload throughput (GB/s),
  (b) cumulative pages migrated,
    (c) MTColloid vs Spectra per-tier demand-read latency.
Data: data/eval-congc.md (experiments/microbench/scripts/extract_congc.py).
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# ── Layout knobs ──────────────────────────────────────────────────────────────
FIG_W_IN = 4.0
FIG_H_IN = 1
WSPACE = 0.5

# ── Font sizes (pt) ─────────────────────────────────────────────────────────
FS_BASE = 8
FS_AXIS_LABEL = 8
FS_TITLE = 8
FS_TICK = 7
FS_LEGEND = 7

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "font.size": FS_BASE,
    "axes.labelsize": FS_AXIS_LABEL,
    "axes.titlesize": FS_TITLE,
    "xtick.labelsize": FS_TICK,
    "ytick.labelsize": FS_TICK,
    "legend.fontsize": FS_LEGEND,
    "figure.dpi": 300,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.5,
    "ytick.major.width": 0.5,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
})

REPO = Path(__file__).resolve().parents[1]
DATA_DIR = Path(os.environ.get("SPECTRA_DATA_DIR", REPO / "results/generated/data"))
OUT_DIR = Path(os.environ.get("SPECTRA_FIG_DIR", REPO / "results/generated/figures"))
DATA_FILE = DATA_DIR / "eval-congc.md"
OUT_PDF = OUT_DIR / "congc.pdf"

# Scheme colours (panels a, b).
COLORS = {"colloid": "#fd8d3c", "mtcolloid": "#7b3294", "libtiermem": "#2166ac"}
DISP = {"colloid": "Colloid", "mtcolloid": "MTColloid", "libtiermem": "Spectra"}
ORDER = ["colloid", "mtcolloid", "libtiermem"]

# Per-tier latency colours (panels c, d): primary = blue, expansion = brown.
C_PRIMARY = "#2166ac"
C_EXPANSION = "#a63603"


def parse_section(text, header):
    block = text.split(header)[1].split("\n##")[0]
    out = {}
    for line in block.splitlines():
        m = re.match(r"^(\w+):\s*(.*)$", line)
        if not m or not m.group(2).strip():
            continue
        pts = [p.split(",") for p in m.group(2).split(";") if p]
        out[m.group(1)] = [tuple(float(x) for x in p) for p in pts]
    return out


def parse_series(text, header):
    """Return the single `series:` row under a `## header` section as tuples."""
    block = text.split(header)[1].split("\n##")[0]
    m = re.search(r"series:\s*(.*)", block)
    return [tuple(float(x) for x in p.split(",")) for p in m.group(1).split(";") if p]


def main():
    text = DATA_FILE.read_text()
    tput = parse_section(text, "## w1-throughput")
    migr = parse_section(text, "## migration-cumulative")
    lat = parse_series(text, "## libtiermem-cc-latency")
    mtcolloid_lat = parse_series(text, "## mtcolloid-latency")

    fig, axes = plt.subplots(
        1,
        3,
        figsize=(FIG_W_IN, FIG_H_IN),
        gridspec_kw={"width_ratios": [1.05, 0.7, 1.25]},
    )

    # (a) throughput
    ax = axes[0]
    for s in ORDER:
        ts = [t for t, _ in tput[s]]
        ys = [y for _, y in tput[s]]
        ax.plot(ts, ys, color=COLORS[s], linewidth=1.0, label=DISP[s])
    ax.set_title("(a)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Stride BW (GB/s)")
    ax.set_ylim(100, 250)
    ax.set_xlim(0, 80)

    # (b) cumulative migration (millions of pages)
    ax = axes[1]
    for s in ORDER:
        ts = [t for t, _ in migr[s]]
        ys = [v / 1e6 for _, v in migr[s]]
        # extend Spectra's flat tail to run end for visibility
        if s == "libtiermem" and ts and ts[-1] < 80:
            ts = ts + [80.0]
            ys = ys + [ys[-1]]
        ax.plot(ts, ys, color=COLORS[s], linewidth=1.0, label=DISP[s])
    ax.set_title("(b)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Pages migrated (M)")
    ax.set_xlim(0, 80)
    ax.set_ylim(0, None)

    # (c) MTColloid vs Spectra per-tier latency
    #   tier  -> colour  (primary = blue, expansion = brown)
    #   scheme-> linestyle (Spectra = solid, MTColloid = dashed)
    ax = axes[2]
    for series, ls in ((mtcolloid_lat, "--"), (lat, "-")):
        t = [x[0] for x in series]
        ax.plot(t, [x[1] for x in series], color=C_PRIMARY, linewidth=1.0, linestyle=ls)
        ax.plot(t, [x[2] for x in series], color=C_EXPANSION, linewidth=1.0, linestyle=ls)
    ax.set_title("(c)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Latency (CHA clk)")
    ax.set_xlim(0, 30)
    ax.set_ylim(250, 760)

    # (spill ratio removed) previously plotted on a secondary axis

    for ax in axes:
        ax.yaxis.grid(True, linestyle="--", alpha=0.3)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.title.set_position((0.5, 1.0))
    for ax in axes[:2]:
        ax.spines["right"].set_visible(False)

    # legends
    axes[0].legend(loc="lower right", frameon=False, handlelength=1,
                   labelspacing=0.2, borderpad=0.2, bbox_to_anchor=(1.05, -0.06))
    # panel (c): split legend into tier colours and scheme linestyles
    tier_legend = axes[2].legend(
        [Line2D([0], [0], color=C_PRIMARY, lw=1.0, ls="-"),
         Line2D([0], [0], color=C_EXPANSION, lw=1.0, ls="-")],
        ["$L_\\mathrm{P}$", "$L_\\mathrm{E}$"],
        loc="upper left",
        frameon=False,
        handlelength=0.7,
        labelspacing=0.2,
        borderpad=0.2,
        bbox_to_anchor=(-0.03, 1.15)
    )
    axes[2].add_artist(tier_legend)
    axes[2].legend(
        [Line2D([0], [0], color="#444444", lw=1.0, ls="-"),
         Line2D([0], [0], color="#444444", lw=1.0, ls="--")],
        ["Spectra", "MTColloid"],
        loc="upper right",
        frameon=False,
        handlelength=1,
        labelspacing=0.2,
        borderpad=0.2,
        bbox_to_anchor=(1.0, 0.83)
    )

    plt.subplots_adjust(left=0.06, right=0.945, top=0.9, bottom=0.22, wspace=WSPACE)
    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
