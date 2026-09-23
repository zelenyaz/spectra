#!/usr/bin/env python3
"""
Macrobench convergence: local-tier residency of the bandwidth-dominant workload
over time, for two representative pairs.  Shows how fast and how completely each
scheme moves the right workload onto the fast tier after tiering is enabled, and
the number of pages TPP promotes per second during the promotion phase.
Data: data/eval-macro-converge.md (experiments/macrobench/scripts/extract_placement.py)
      and <run>/*/tpp/w*/migrate_pages.log under SPECTRA_MIGRATE_ROOT.
"""

import os
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ── Layout knobs ──────────────────────────────────────────────────────────────
FIG_W_IN = 4     # 1.5-column
FIG_H_IN = 2.45
WSPACE = 0.2
HSPACE = 1

FS_BASE = 9
FS_AXIS_LABEL = 9
FS_TITLE = 9
FS_TICK = 9
FS_LEGEND = 9

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
DATA_FILE = DATA_DIR / "eval-macro-converge.md"
OUT_PDF = OUT_DIR / "macro-converge.pdf"

COLORS = {"tpp": "#fb6a4a", "colloid": "#fd8d3c", "alto": "#fdae6b",
          "memtis": "#9ecae1", "mttm": "#c994c7", "libtiermem": "#2166ac"}
DISP = {"tpp": "TPP", "colloid": "Colloid", "alto": "Alto",
        "memtis": "Memtis", "mttm": "MTTM", "libtiermem": "Spectra"}
ORDER = ["tpp", "colloid", "alto", "memtis", "mttm", "libtiermem"]
PANEL_TITLE = {
    "llama-faissflat": "(a) Llama+FA-flat: Llama",
    "spec619-llama":   "(b) SPEC619+Llama: SPEC619",
}
SYS_PANEL_TITLE = {
    "llama-faissflat": "(c) Llama+FA-flat",
    "spec619-llama":   "(d) SPEC619+Llama",
}
WORKLOAD_LABEL = {
    ("llama-faissflat", "w1"): "Llama",
    ("llama-faissflat", "w2"): "FA-flat",
    ("spec619-llama", "w1"): "SPEC619",
    ("spec619-llama", "w2"): "Llama",
}
WORKLOAD_COLOR = {"w1": "#009E73", "w2": "#CC79A7"}
WORKLOAD_STYLE = {"w1": "-", "w2": "-"}
WORKLOAD_ZORDER = {"w1": 4, "w2": 3}


def parse():
    txt = DATA_FILE.read_text()
    panels = {}
    for block in txt.split("## ")[1:]:
        lines = block.splitlines()
        pair = lines[0].split()[0]
        series = {}
        for line in lines[1:]:
            m = re.match(r"^(\w+): (.+)$", line)
            if not m:
                continue
            pts = [p.split(",") for p in m.group(2).split(";") if p]
            series[m.group(1)] = [(float(t), float(v)) for t, v in pts]
        panels[pair] = series
    return panels


_TP_SUCC_RE = re.compile(r'^@tp_succ\[\d+\]: (\d+)$')
_PID_HDR_RE = re.compile(r'^-- pid=\d+ --$')


def parse_migrate_log(path):
    """Parse migrate_pages.log; returns list of (second, pages_promoted) pairs."""
    points = []
    second = -1
    current_pages = 0
    in_second = False
    for line in path.read_text().splitlines():
        line = line.strip()
        if _PID_HDR_RE.match(line):
            if in_second:
                points.append((second, current_pages))
            second += 1
            current_pages = 0
            in_second = True
        else:
            m = _TP_SUCC_RE.match(line)
            if m:
                current_pages += int(m.group(1))
    if in_second:
        points.append((second, current_pages))
    return points


def migration_root():
    configured = os.environ.get("SPECTRA_MIGRATE_ROOT")
    if configured:
        return Path(configured)
    match = re.search(r"^# workload over time .*?, (run\d+)\.$",
                      DATA_FILE.read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f"missing source run in {DATA_FILE}")
    return REPO / "results/generated/raw/macrobench/results" / match.group(1)


def parse_migrate_pages(pairs):
    panels = {}
    root = migration_root()
    for pair in pairs:
        series = {}
        for wdir in sorted((root / pair / "tpp").glob("w*")):
            log = wdir / "migrate_pages.log"
            if not log.exists():
                continue
            pts = parse_migrate_log(log)
            if pts:
                series[wdir.name] = pts
        panels[pair] = series
    return panels


def main():
    panels = parse()
    pairs = ["llama-faissflat", "spec619-llama"]
    migrate_panels = parse_migrate_pages(pairs)
    for pair in pairs:
        missing = {"w1", "w2"} - migrate_panels[pair].keys()
        if missing:
            raise ValueError(f"missing TPP migration samples for {pair}: {sorted(missing)}")
    fig, axes = plt.subplots(2, 2, figsize=(FIG_W_IN, FIG_H_IN))

    for ax, pair in zip(axes[0], pairs):
        series = panels[pair]
        for base in ORDER:
            if base not in series:
                continue
            ts = [t for t, _ in series[base]]
            ys = [v for _, v in series[base]]
            lw = 1.7 if base == "libtiermem" else 1.5
            z = 5 if base == "libtiermem" else 3
            ax.plot(ts, ys, color=COLORS[base], linewidth=lw, label=DISP[base],
                    zorder=z)
        ax.set_xlabel("Time (s)")
        ax.set_title(PANEL_TITLE[pair], fontsize=FS_TITLE, pad=2)
        ax.set_ylim(0, None)
        ax.set_xlim(0, None)
        ax.yaxis.grid(True, linestyle="--", alpha=0.3)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    axes[0, 0].set_ylabel("Data on P-tier (GB)")

    for ax, pair in zip(axes[1], pairs):
        for workload, pts in migrate_panels[pair].items():
            ts = [t for t, _ in pts]
            ys = [v / 1000 for _, v in pts]
            total = sum(v for _, v in pts)
            total_str = f"{total / 1_000_000:.2f}M" if total >= 1_000_000 else f"{total / 1000:.0f}K"
            label = f"{WORKLOAD_LABEL.get((pair, workload), workload)} ({total_str})"
            ax.plot(ts, ys, color=WORKLOAD_COLOR.get(workload, "#444444"),
                    linestyle=WORKLOAD_STYLE.get(workload, "-"), linewidth=1.5,
                    label=label,
                    zorder=WORKLOAD_ZORDER.get(workload, 3))
        ax.set_xlabel("Time (s)")
        ax.set_title(SYS_PANEL_TITLE[pair], fontsize=FS_TITLE, pad=4)
        ax.set_ylim(0, 200)
        ax.set_xlim(0, None)
        ax.yaxis.grid(True, linestyle="--", alpha=0.3)
        ax.set_axisbelow(True)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.legend(loc="upper right", frameon=False, handlelength=1,
                  handletextpad=0.2, bbox_to_anchor=(1.0, 1.15))
    axes[1, 0].set_ylabel("Pages promoted (K/s)")

    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 1.1),
               ncol=6, frameon=False, handlelength=1, columnspacing=1.0,
               handletextpad=0.4)
    plt.subplots_adjust(left=0.12, right=0.98, top=0.90, bottom=0.13,
                        wspace=WSPACE, hspace=HSPACE)
    OUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight", dpi=300)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
