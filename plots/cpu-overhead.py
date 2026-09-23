#!/usr/bin/env python3
"""
Tiering CPU overhead (Table 4): mean %CPU of one core spent in the active
tiering mechanism per macro-benchmark pair.

Data: data/eval-cpu-overhead.md, mirrored from Table 4 in main.tex.
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# -- Layout knobs -------------------------------------------------------------
FIG_W_IN = 3.33
FIG_H_IN = 1.1
Spectra_LABEL_DX = 0.13

# -- Font sizes (pt) ----------------------------------------------------------
FS_BASE       = 9
FS_AXIS_LABEL = 9
FS_TITLE      = 9
FS_TICK       = 8
FS_LEGEND     = 8
FS_ANNOT      = 8

# -- Global style -------------------------------------------------------------
plt.rcParams.update({
    "font.family":        "serif",
    "font.serif":         ["Times New Roman", "Times", "DejaVu Serif"],
    "font.size":          FS_BASE,
    "axes.labelsize":     FS_AXIS_LABEL,
    "axes.titlesize":     FS_TITLE,
    "xtick.labelsize":    FS_TICK,
    "ytick.labelsize":    FS_TICK,
    "legend.fontsize":    FS_LEGEND,
    "figure.dpi":         300,
    "savefig.bbox":       "tight",
    "savefig.pad_inches": 0.02,
    "axes.linewidth":     0.6,
    "xtick.major.width":  0.5,
    "ytick.major.width":  0.5,
    "xtick.major.size":   3,
    "ytick.major.size":   3,
    "hatch.linewidth":    0.5,
})

# -- Paths --------------------------------------------------------------------
REPO = Path(__file__).resolve().parents[1]
DATA_DIR = Path(os.environ.get("SPECTRA_DATA_DIR", REPO / "results/generated/data"))
OUT_DIR = Path(os.environ.get("SPECTRA_FIG_DIR", REPO / "results/generated/figures"))
DATA_FILE = DATA_DIR / "eval-cpu-overhead.md"
OUT_PDF = OUT_DIR / "cpu-overhead.pdf"

SCHEMES = ["TPP", "Memtis", "Spectra"]
DATA_COLUMNS = {"TPP": "TPP", "Memtis": "Memtis", "Spectra": "Spectra"}
COLORS = {
    "TPP": "#fb6a4a",
    "Memtis": "#9ecae1",
    "Spectra": "#2166ac",
}
HATCHES = {
    "TPP": "",
    "Memtis": "///",
    "Spectra": "...",
}
PAIR_LABELS = {
    "SPEC607+FA-poly": "SPEC607+\nFA-poly",
    "Llama+FA-flat": "Llama+\nFA-flat",
    "SPEC619+Llama": "SPEC619+\nLlama",
    "VHT+DRAMhit": "VHT+\nDRAMhit",
    "VHT+FA-poly": "VHT+\nFA-poly",
    "mean": "Mean",
}


def parse_overhead_table(path: Path):
    header = None
    rows = []

    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if not line.startswith("|"):
            continue
        if re.match(r"^\|[\s\-:|]+\|$", line):
            continue

        cells = [cell.strip().strip("*") for cell in line.strip("|").split("|")]
        if cells[0].lower() == "pair":
            header = cells
            continue
        if header is None:
            raise ValueError(f"data row appears before header: {line}")
        if any(cell == "???" for cell in cells):
            raise ValueError("unfilled cells (???) remain in data file")

        entry = dict(zip(header, cells))
        rows.append({
            "pair": entry["pair"],
            **{scheme: float(entry[DATA_COLUMNS[scheme]]) for scheme in SCHEMES},
        })

    if not rows:
        raise ValueError(f"no data rows found in {path}")

    missing_labels = [row["pair"] for row in rows if row["pair"] not in PAIR_LABELS]
    if missing_labels:
        raise ValueError(f"missing display labels for pairs: {missing_labels}")
    return rows


def main():
    rows = parse_overhead_table(DATA_FILE)
    pairs = [row["pair"] for row in rows]
    labels = [PAIR_LABELS[pair] for pair in pairs]

    fig, ax = plt.subplots(1, 1, figsize=(FIG_W_IN, FIG_H_IN),
                           constrained_layout=True)

    x = np.arange(len(pairs))
    width = 0.23
    offsets = np.linspace(-width, width, len(SCHEMES))

    bars_by_scheme = {}
    for offset, scheme in zip(offsets, SCHEMES):
        values = [row[scheme] for row in rows]
        bars_by_scheme[scheme] = ax.bar(
            x + offset,
            values,
            width,
            label=scheme,
            color=COLORS[scheme],
            edgecolor="#222222",
            linewidth=0.45,
            hatch=HATCHES[scheme],
            zorder=3,
        )

    for bar, row in zip(bars_by_scheme["Spectra"], rows):
        val = row["Spectra"]
        ax.text(
            bar.get_x() + bar.get_width() / 2 + Spectra_LABEL_DX,
            val + 0.9,
            f"{val:.2f}",
            ha="center",
            va="bottom",
            fontsize=FS_ANNOT,
            color=COLORS["Spectra"],
        )

    ax.axvline(x=len(pairs) - 1.5, color="#999999", linewidth=0.6,
               linestyle="--", zorder=2)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("%CPU", labelpad=2)
    ax.set_ylim(0, 50)
    ax.set_yticks([0, 10, 20, 30, 40, 50])
    ax.yaxis.grid(True, linestyle="--", alpha=0.3, linewidth=0.4)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(
        loc="upper right",
        frameon=False,
        handlelength=1.1,
        handletextpad=0.35,
        ncol=3,
        columnspacing=0.7,
        borderaxespad=0.1,
    )

    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    plt.close(fig)
    print(f"Saved -> {OUT_PDF}")


if __name__ == "__main__":
    main()
