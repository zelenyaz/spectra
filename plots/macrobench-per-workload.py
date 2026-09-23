#!/usr/bin/env python3
"""
Macro-benchmark normalized duration across application pairs.

One subplot per pair. Each baseline shows two bars, one for W1 and one for W2,
using the per-workload normalized values from the summary table. This version
does not aggregate the two workloads with a geometric mean.
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyArrowPatch, Patch

# -- Style --------------------------------------------------------------------
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "font.size": 9,
    "axes.labelsize": 9,
    "axes.titlesize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "legend.fontsize": 8,
    "figure.dpi": 300,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.5,
    "ytick.major.width": 0.5,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
})

# -- Paths --------------------------------------------------------------------
REPO = Path(__file__).resolve().parents[1]
DATA_DIR = Path(os.environ.get("SPECTRA_DATA_DIR", REPO / "results/generated/data"))
OUT_DIR = Path(os.environ.get("SPECTRA_FIG_DIR", REPO / "results/generated/figures"))
DATA_FILE = DATA_DIR / "macrobench-duration-summary.md"
OUT_PDF = OUT_DIR / "macrobench-per-workload.pdf"

DIRECTION_ARROW_X = 0.96
DIRECTION_TEXT_X = 0.975
DIRECTION_BOTTOM = 0.18
DIRECTION_TOP = 0.88
DIRECTION_FONT_SIZE = 9.5

COMBOS = [
    "faissbench-spec607",
    "llama-faissflat",
    "spec619-llama",
    "vht-dramhit",
    "vht-faissbench",
]
COMBO_TITLES = {
    "faissbench-spec607": "SPEC607 + FA-poly",
    "llama-faissflat": "Llama + FA-flat",
    "spec619-llama": "SPEC619 + Llama",
    "vht-dramhit": "VHT + DRAMhit",
    "vht-faissbench": "VHT + FA-poly",
}
SUBFIG_LABELS = ["(a)", "(b)", "(c)", "(d)", "(e)"]

# The All-P case is recorded as "local" in the results summary and drawn at y=1.
BASELINES = [
    "only-W1-local",
    "only-W2-local",
    "tpp",
    "colloid",
    "alto",
    "memtis",
    "mttm",
    "libtiermem",
]
LEGEND_LABELS = {
    "only-W1-local": "W1-P",
    "only-W2-local": "W2-P",
    "tpp": "TPP",
    "colloid": "Colloid",
    "alto": "Alto",
    "memtis": "Memtis",
    "mttm": "MTTM",
    "libtiermem": "Spectra",
}
WORKLOADS = [
    ("W1", -0.18, "", "solid"),
    ("W2", 0.18, "///////", "hatched"),
]


def parse_per_workload_normalized_values(path: Path):
    """Return dict[combo][case][workload] -> normalized duration."""
    data = {}
    in_local_section = False
    current_combo = None

    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()

        if line.startswith("## "):
            in_local_section = line == "## Normalized duration vs. local baseline"
            current_combo = None
            continue
        if not in_local_section:
            continue

        combo_match = re.match(r"^### `([^`]+)`", line)
        if combo_match:
            current_combo = combo_match.group(1)
            data[current_combo] = {}
            continue
        if line.startswith("### Geometric mean"):
            current_combo = None
            continue

        if current_combo is None or not line.startswith("|"):
            continue
        if re.match(r"^\|[\s\-:|]+\|$", line):
            continue

        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells[0].lower() == "case":
            continue

        case = re.sub(r"\s*\(n=\d+\)$", "", cells[0])
        data[current_combo][case] = {
            "W1": float(cells[1]),
            "W2": float(cells[2]),
        }

    missing = [combo for combo in COMBOS if combo not in data]
    if missing:
        raise ValueError(f"missing combo data: {missing}")
    for combo in COMBOS:
        missing_cases = [case for case in ["local", *BASELINES] if case not in data[combo]]
        if missing_cases:
            raise ValueError(f"{combo} missing cases: {missing_cases}")

    return data


def add_direction_cue(fig, label: str, *, upward: bool):
    start_y, end_y = (
        (DIRECTION_BOTTOM, DIRECTION_TOP)
        if upward
        else (DIRECTION_TOP, DIRECTION_BOTTOM)
    )
    arrow = FancyArrowPatch(
        (DIRECTION_ARROW_X, start_y),
        (DIRECTION_ARROW_X, end_y),
        transform=fig.transFigure,
        arrowstyle="->",
        mutation_scale=8,
        linewidth=0.8,
        color="#222222",
        clip_on=False,
    )
    fig.add_artist(arrow)
    fig.text(
        DIRECTION_TEXT_X,
        (DIRECTION_BOTTOM + DIRECTION_TOP) / 2,
        label,
        rotation=270,
        va="center",
        ha="center",
        fontsize=DIRECTION_FONT_SIZE,
    )


def main():
    values = parse_per_workload_normalized_values(DATA_FILE)

    colors = {
        "only-W1-local": "#c0c0c0",
        "only-W2-local": "#808a87",
        "tpp": "#fb6a4a",
        "colloid": "#fd8d3c",
        "alto": "#fdae6b",
        "memtis": "#9ecae1",
        "mttm": "#c994c7",
        "libtiermem": "#2166ac",
    }
    bar_edge = "#222222"

    fig, axes = plt.subplots(
        1, len(COMBOS), figsize=(6.8, 1.2), sharey=True, constrained_layout=False
    )

    x = np.arange(len(BASELINES))
    bar_w = 0.32

    for ax, combo, sub in zip(axes, COMBOS, SUBFIG_LABELS):
        for xi, baseline in zip(x, BASELINES):
            for workload, offset, hatch, style in WORKLOADS:
                facecolor = colors[baseline] if style == "solid" else "#ffffff"
                edgecolor = bar_edge if style == "solid" else colors[baseline]
                ax.bar(
                    xi + offset,
                    values[combo][baseline][workload],
                    width=bar_w,
                    color=facecolor,
                    edgecolor=edgecolor,
                    linewidth=0.5,
                    hatch=hatch,
                    zorder=3,
                )

        ax.axhline(1.0, color="#666666", linewidth=0.5, linestyle="--", zorder=2)
        ax.set_xticks([])
        ax.set_xlabel(f"{sub} {COMBO_TITLES[combo]}", fontsize=8, labelpad=4)
        ax.set_ylim(0.68, 1.55)
        ax.set_yticks([0.8, 1.0, 1.2, 1.4])

        # if combo == "vht-dramhit":
        #     mttm_xi = BASELINES.index("mttm")
        #     mttm_w1 = values[combo]["mttm"]["W1"]
        #     mttm_w2 = values[combo]["mttm"]["W2"]
        #     ax.text(
        #         mttm_xi - 0.18, min(mttm_w1, 2.15) + 0.02, f"{mttm_w1:.2f}",
        #         ha="center", va="bottom", fontsize=6.3, zorder=5,
        #     )
        #     ax.text(
        #         mttm_xi + 0.18, min(mttm_w2, 2.15) + 0.02, f"{mttm_w2:.2f}",
        #         ha="center", va="bottom", fontsize=6.3, zorder=5,
        #     )
        ax.yaxis.grid(True, linestyle="--", alpha=0.3, zorder=1)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    axes[0].set_ylabel("Normalized duration\n(All-P = 1.0)")

    baseline_handles = [
        Patch(
            facecolor=colors[baseline],
            edgecolor=bar_edge,
            linewidth=0.3,
            label=LEGEND_LABELS[baseline],
        )
        for baseline in BASELINES
    ]
    workload_handles = [
        Patch(facecolor="#ffffff", edgecolor=bar_edge, linewidth=0.3, label="W1", hatch=""),
        Patch(facecolor="#ffffff", edgecolor="#2166ac", linewidth=0.3, label="W2", hatch="/////"),
    ]
    fig.legend(
        handles=baseline_handles,
        loc="upper center",
        bbox_to_anchor=(0.5, 1.01),
        ncol=8,
        frameon=False,
        handlelength=0.7,
        handletextpad=0.35,
        columnspacing=0.8,
    )
    fig.legend(
        handles=workload_handles,
        loc="upper left",
        bbox_to_anchor=(0.015, 1.01),
        ncol=2,
        frameon=False,
        handlelength=1.0,
        handletextpad=0.35,
        columnspacing=0.8,
    )

    add_direction_cue(fig, "Lower is better", upward=False)

    plt.subplots_adjust(left=0.065, right=0.95, top=0.82, bottom=0.18, wspace=0.08)

    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
