#!/usr/bin/env python3
"""
Micro-benchmark normalized speedup across five membench workload pairs.

One subplot per pair (1x5 row, double-column). Each subplot shows the two
static one-workload-on-primary oracles (W1-P, W2-P) and the dynamic baselines
(TPP, Colloid, Alto, Memtis, MTTM, libtiermem) as grouped bars, normalized to
the all-E placement where both buffers reside on the expansion tier
(all-E = 1.0).  Data: data/eval-micro.md (experiments/microbench/scripts/collect_summary.py).
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyArrowPatch, Patch

# ── Layout knobs ──────────────────────────────────────────────────────────────
FIG_W_IN = 3.0      # double-column
FIG_H_IN = 1
WSPACE = 0.10

# ── Font sizes (pt) ─────────────────────────────────────────────────────────
FS_BASE = 9
FS_AXIS_LABEL = 9
FS_TITLE = 9
FS_TICK = 9
FS_LEGEND = 8

# ── Style ─────────────────────────────────────────────────────────────────────
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

# ── Paths ─────────────────────────────────────────────────────────────────────
REPO = Path(__file__).resolve().parents[1]
DATA_DIR = Path(os.environ.get("SPECTRA_DATA_DIR", REPO / "results/generated/data"))
OUT_DIR = Path(os.environ.get("SPECTRA_FIG_DIR", REPO / "results/generated/figures"))
DATA_FILE = DATA_DIR / "eval-micro.md"
OUT_PDF = OUT_DIR / "microbench.pdf"

DIRECTION_ARROW_X = 0.965
DIRECTION_TEXT_X = 0.99
DIRECTION_BOTTOM = 0.16
DIRECTION_TOP = 0.88

CASES = ["sr-cr", "sr-rr", "spf-rr", "sr-srw", "sr-swr"]
CASE_TITLES = {
    "sr-cr":  "sd-rd\n+ chk-reuse",
    "sr-rr":  "lsd-rd\n+ rnd-rd",
    "spf-rr": "swpf-rd\n+ rnd-rd",
    "sr-srw": "sd-rd\n+ sd-rw",
    "sr-swr": "sd-rd\n+ sd-wr",
}
SUBFIG_LABELS = ["(a)", "(b)", "(c)", "(d)", "(e)"]
BASELINES = ["W1-P", "W2-P", "TPP", "Colloid", "Alto", "Memtis", "MTTM", "libtiermem"]
LEGEND_LABEL = {"libtiermem": "Spectra"}
SUMMARY_CASES = {
    "sdrd-chksdrd": "sr-cr", "sdprd-rr": "sr-rr", "sdrdpf-rr": "spf-rr",
    "sdrd-sdrw": "sr-srw", "sdrd-sdwr": "sr-swr",
}
SUMMARY_BASELINES = {
    "only-W1-local": "W1-P", "only-W2-local": "W2-P",
    "tpp": "TPP", "colloid": "Colloid", "alto": "Alto",
    "memtis": "Memtis", "mttm": "MTTM", "libtiermem": "libtiermem",
}


# ── Parse data ────────────────────────────────────────────────────────────────
def parse_data_file(path: Path):
    text = path.read_text()
    if "## Normalized to remote case" in text:
        return parse_summary_data(text)
    section = None
    rows = []
    for raw_line in text.splitlines():
        line = raw_line.rstrip()
        stripped = line.lstrip()
        if not stripped:
            continue
        if stripped.startswith("##"):
            section = stripped.lstrip("#").strip().lower()
            continue
        if stripped.startswith("#"):
            continue
        if not line.startswith("|"):
            continue
        if re.match(r"^\|[\s\-:|]+\|$", line):
            continue
        rows.append((section, line))

    def parse_baseline_table(table_rows):
        header = [c.strip() for c in table_rows[0].strip("|").split("|")]
        case_cols = header[1:]
        out = {}
        for row in table_rows[1:]:
            cells = [c.strip() for c in row.strip("|").split("|")]
            out[cells[0]] = {c: float(cells[1 + j]) for j, c in enumerate(case_cols)}
        return out

    means_rows, stds_rows, label_rows = [], [], []
    in_labels = False
    for s, r in rows:
        first_cell = r.strip("|").split("|", 1)[0].strip().lower()
        if first_cell == "case":
            in_labels = True
        if in_labels:
            label_rows.append(r)
        elif s == "means":
            means_rows.append(r)
        elif s == "stds":
            stds_rows.append(r)

    values = parse_baseline_table(means_rows)
    stds = parse_baseline_table(stds_rows)

    labels = {}
    for row in label_rows[1:]:
        cells = [c.strip() for c in row.strip("|").split("|")]
        labels[cells[0]] = (cells[1], cells[2])

    if any("???" in str(v) for d in values.values() for v in d.values()):
        raise ValueError("unfilled cells (???) remain in data file")
    return values, stds, labels


def parse_summary_data(text: str):
    """Read the normalized gmean column of collect_summary.py's report."""
    values = {baseline: {} for baseline in BASELINES}
    stds = {baseline: {} for baseline in BASELINES}
    in_section = False
    case = None
    for line in text.splitlines():
        if line.startswith("## "):
            in_section = line == "## Normalized to remote case"
            case = None
            continue
        if not in_section:
            continue
        match = re.match(r"^### `([^`]+)`", line)
        if match:
            case = SUMMARY_CASES.get(match.group(1))
            continue
        if case is None or not line.startswith("|") or line.startswith("|---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        source = re.sub(r"\s*\(n=\d+\)$", "", cells[0])
        baseline = SUMMARY_BASELINES.get(source)
        if baseline is None:
            continue
        metric = cells[-1]
        if metric == "-":
            raise ValueError(f"missing normalized throughput: {case}/{source}")
        mean, *std = metric.split("±", 1)
        values[baseline][case] = float(mean)
        stds[baseline][case] = float(std[0]) if std else 0.0

    missing = [(baseline, case) for baseline in BASELINES for case in CASES
               if case not in values[baseline]]
    if missing:
        raise ValueError(f"missing microbenchmark figure data: {missing}")
    return values, stds, {}


# ── Plot ──────────────────────────────────────────────────────────────────────
def add_direction_cue(fig, label: str):
    arrow = FancyArrowPatch(
        (DIRECTION_ARROW_X, DIRECTION_BOTTOM),
        (DIRECTION_ARROW_X, DIRECTION_TOP),
        transform=fig.transFigure, arrowstyle="->", mutation_scale=8,
        linewidth=0.8, color="#222222", clip_on=False)
    fig.add_artist(arrow)
    fig.text(DIRECTION_TEXT_X, (DIRECTION_BOTTOM + DIRECTION_TOP) / 2, label,
             rotation=270, va="center", ha="center", fontsize=FS_LEGEND)


def main():
    values, stds, labels = parse_data_file(DATA_FILE)

    COLORS = {
        "W1-P":       "#c0c0c0",
        "W2-P":       "#808a87",
        "TPP":        "#fb6a4a",
        "Colloid":    "#fd8d3c",
        "Alto":       "#fdae6b",
        "Memtis":     "#9ecae1",
        "MTTM":       "#c994c7",
        "libtiermem": "#2166ac",
    }
    BAR_EDGE = "#222222"

    fig, axes = plt.subplots(1, 5, figsize=(FIG_W_IN, FIG_H_IN), sharey=True,
                             constrained_layout=False)

    x = np.arange(len(BASELINES))
    bar_w = 0.80

    for ax, case, sub in zip(axes, CASES, SUBFIG_LABELS):
        heights = [values[b][case] for b in BASELINES]
        errs = [stds[b][case] for b in BASELINES]
        for xi, h, e, base in zip(x, heights, errs, BASELINES):
            ax.bar(xi, h, width=bar_w, color=COLORS[base], edgecolor=BAR_EDGE,
                   linewidth=0.5, zorder=3)
            ax.errorbar(xi, h, yerr=e, fmt="none", ecolor="#222222",
                        elinewidth=0.6, capsize=1.5, capthick=0.6, zorder=4)
        ax.axhline(1.0, color="#666666", linewidth=0.5, linestyle="--", zorder=2)
        ax.set_xticks([])
        ax.set_xlabel(f"{sub} {CASE_TITLES[case]}", fontsize=FS_TITLE, labelpad=3)
        ax.set_ylim(0.85, 2.15)
        ax.set_yticks([1.0, 1.2, 1.4, 1.6, 1.8, 2.0])
        ax.yaxis.grid(True, linestyle="--", alpha=0.3, zorder=1)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    axes[0].set_ylabel("Norm. speedup")

    legend_handles = [
        Patch(facecolor=COLORS[base], edgecolor=BAR_EDGE, linewidth=0.3,
              label=LEGEND_LABEL.get(base, base))
        for base in BASELINES
    ]
    fig.legend(handles=legend_handles, loc="upper center",
               bbox_to_anchor=(0.5, 1.3), ncol=6, frameon=False,
               handlelength=0.6, handletextpad=0.2, columnspacing=1.0)

    add_direction_cue(fig, "Higher is better")
    plt.subplots_adjust(left=0.06, right=0.95, top=0.86, bottom=0.14, wspace=WSPACE)

    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
