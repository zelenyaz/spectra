#!/usr/bin/env python3
"""Collect per-workload execution time from a macrobench-suite run.

Reads two kinds of cases:

* Static cases (W1/W2 ran concurrently with a fixed memory placement):
    <run-dir>/results-static-remote/<combo>/static/w{1,2}/duration.txt
    <run-dir>/results-static-local/<combo>/static/w{1,2}/duration.txt
    <run-dir>/results-static-only-W1-local/<combo>/static/w{1,2}/duration.txt
    <run-dir>/results-static-only-W2-local/<combo>/static/w{1,2}/duration.txt

* Dynamic-baseline cases (a tiering policy migrates pages while W1/W2 run):
    <run-dir>/results/<combo>/<baseline>/w{1,2}/duration.txt
  Baselines are auto-discovered from subdirectories of <combo>/.

Multi-run support: any case-root may also contain `run*` subdirectories
holding per-run repeats:
    <case-root>/run<N>/<combo>/<sub>/w{1,2}/duration.txt
When a `(combo, case)` pair has at least one such run, the multi-run
results are aggregated (mean +/- sample std) and the single-run directory
directly under <case-root>/<combo> is ignored for that pair.

`timeline.log` (next to w1/w2) provides the W1/W2 workload names.

Emits:
  <run-dir>/macrobench-duration-summary.md  (or path passed via -o)

Each cell is reported raw (seconds) and normalized as case/remote so that
values <1 indicate a speed-up over the all-remote baseline (lower is
better).

Usage:
  python3 collect_duration_summary.py <run-dir> [-o OUT.md]
"""
import argparse
import math
from pathlib import Path
import re

STATIC_CASES = [
    ("results-static-remote", "static", "remote"),
    ("results-static-local", "static", "local"),
    ("results-static-only-W1-local", "static", "only-W1-local"),
    ("results-static-only-W2-local", "static", "only-W2-local"),
]
DYNAMIC_PARENT = "results"


def parse_duration(path: Path, field: str = "duration_sec"):
    """Read a positive, finite duration only from a successful workload."""
    duration = None
    exit_code = None
    prefix = f"{field}="
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("exit_code="):
                exit_code = line.split("=", 1)[1]
            if line.startswith(prefix):
                try:
                    duration = float(line.split("=", 1)[1])
                except ValueError:
                    duration = None
    if exit_code != "0" or duration is None or not math.isfinite(duration) or duration <= 0:
        duration = None
    return {"duration_sec": duration}


_W_RE = re.compile(r"(W[12])\(([^)]+)\):")


def parse_workload_names(timeline: Path):
    """Return {'W1': name, 'W2': name}."""
    names = {}
    if not timeline.exists():
        return names
    with open(timeline) as f:
        for line in f:
            m = _W_RE.search(line)
            if m:
                names[m.group(1)] = m.group(2)
            if "W1" in names and "W2" in names:
                break
    return names


def _is_run_dir(p: Path) -> bool:
    return p.is_dir() and p.name.startswith("run")


def _list_run_subdirs(case_root: Path):
    if not case_root.exists():
        return []
    return sorted([d for d in case_root.iterdir() if _is_run_dir(d)])


def discover_combos(case_root: Path):
    combos = set()
    if not case_root.exists():
        return combos
    for d in case_root.iterdir():
        if not d.is_dir():
            continue
        if _is_run_dir(d):
            for c in d.iterdir():
                if c.is_dir():
                    combos.add(c.name)
        else:
            combos.add(d.name)
    return combos


def find_run_paths(case_root: Path, combo: str, sub: str):
    """Locate the per-repeat run directories for (combo, sub) under case_root.

    Returns (paths, is_multi). If any `<case_root>/run*/<combo>/<sub>/` exists,
    those are returned and the single-run directory directly under
    `<case_root>/<combo>/<sub>` is ignored. Otherwise the single-run path is
    returned (if it exists)."""
    multi = []
    for run_dir in _list_run_subdirs(case_root):
        candidate = run_dir / combo / sub
        if candidate.exists():
            multi.append(candidate)
    if multi:
        return multi, True
    single = case_root / combo / sub
    if single.exists():
        return [single], False
    return [], False


def _aggregate_durations(metric_dicts):
    """Aggregate a list of {'duration_sec': v} dicts.

    Returns {'duration_sec': mean, 'duration_sec_std': std-or-None,
             '_n': k, 'bad_runs': count_of_missing_duration}.
    """
    n = len(metric_dicts)
    good = [m for m in metric_dicts if m.get("duration_sec") is not None]
    bad = n - len(good)
    if not good:
        return {"duration_sec": None, "duration_sec_std": None,
                "_n": n, "bad_runs": bad}
    vals = [m["duration_sec"] for m in good]
    mean = sum(vals) / len(vals)
    if len(vals) >= 2:
        var = sum((v - mean) ** 2 for v in vals) / (len(vals) - 1)
        std = math.sqrt(var)
    else:
        std = None
    return {"duration_sec": mean, "duration_sec_std": std,
            "_n": n, "bad_runs": bad}


def collect_case(case_root: Path, sub: str, duration_field: str = "duration_sec"):
    """For each combo under case_root, gather w1/w2 duration metrics.

    Returns {combo: {'w1_name', 'w2_name', 'is_multi', 'n_runs',
                     'w1': agg-or-None, 'w2': agg-or-None}}.
    """
    out = {}
    if not case_root.exists():
        return out
    for combo in sorted(discover_combos(case_root)):
        run_paths, is_multi = find_run_paths(case_root, combo, sub)
        if not run_paths:
            continue
        names = {}
        for rp in run_paths:
            names = parse_workload_names(rp / "timeline.log")
            if names:
                break
        entry = {
            "w1_name": names.get("W1", "?"),
            "w2_name": names.get("W2", "?"),
            "is_multi": is_multi,
            "n_runs": len(run_paths),
        }
        for w in ("w1", "w2"):
            metric_dicts = []
            for rp in run_paths:
                dur_path = rp / w / "duration.txt"
                if dur_path.exists():
                    metric_dicts.append(parse_duration(dur_path, duration_field))
            entry[w] = _aggregate_durations(metric_dicts) if metric_dicts else None
        out[combo] = entry
    return out


def discover_dynamic_baselines(run_dir: Path):
    """Return a sorted list of baseline names found under
    <run-dir>/results/<combo>/<baseline>/ -- including baselines that appear
    only inside `run*` repeat subdirectories."""
    root = run_dir / DYNAMIC_PARENT
    if not root.exists():
        return []
    found = set()

    def _scan_combo(combo_dir: Path):
        for sub in combo_dir.iterdir():
            if sub.is_dir() and (sub / "w1").exists() and (sub / "w2").exists():
                found.add(sub.name)

    for d in root.iterdir():
        if not d.is_dir():
            continue
        if _is_run_dir(d):
            for combo_dir in d.iterdir():
                if combo_dir.is_dir():
                    _scan_combo(combo_dir)
        else:
            _scan_combo(d)
    return sorted(found)


def fmt_mean_std(d, prec=4):
    if not d:
        return "-"
    v = d.get("duration_sec")
    if v is None:
        return "-"
    s = d.get("duration_sec_std")
    base = f"{v:.{prec}g}"
    return f"{base}+/-{s:.{prec}g}" if s is not None else base


def build_report(run_dir: Path, duration_field: str = "duration_sec"):
    cases = []
    for parent, sub, label in STATIC_CASES:
        cases.append((label, run_dir / parent, sub))
    dynamic_baselines = discover_dynamic_baselines(run_dir)
    for baseline in dynamic_baselines:
        cases.append((baseline, run_dir / DYNAMIC_PARENT, baseline))

    data = {label: collect_case(root, sub, duration_field) for label, root, sub in cases}
    combos = sorted({c for case in data.values() for c in case.keys()})

    all_labels = [c[0] for c in cases]

    lines = []
    lines.append(f"# Macrobench duration summary ({run_dir.name})")
    lines.append("")
    lines.append(f"Source: `{run_dir}`")
    lines.append("")
    lines.append(
        "Per-workload execution time read from "
        f"`<combo>/<case>/w{{1,2}}/duration.txt` (field `{duration_field}`)."
    )
    lines.append("")
    lines.append("Static-baseline memory placements:")
    lines.append("- **remote**: W1 & W2 on node 1 (remote)")
    lines.append("- **local**: W1 & W2 on node 0 (local)")
    lines.append("- **only-W1-local**: W1 on node 0, W2 on node 1")
    lines.append("- **only-W2-local**: W1 on node 1, W2 on node 0")
    lines.append("")
    if dynamic_baselines:
        lines.append(
            "Dynamic baselines discovered: "
            + ", ".join(f"`{b}`" for b in dynamic_baselines)
        )
        lines.append("")
    lines.append("Workloads pinned to node-0 cores in all cases.")
    lines.append("")

    lines.append("## Raw execution time (seconds)")
    lines.append("")
    lines.append(
        "Multi-run cells show `mean+/-std` (sample std, n>1) with the run "
        "count annotated on the case label as `(n=k)`."
    )
    lines.append("")
    for combo in combos:
        entry_ref = next(
            (data[lbl][combo] for lbl in all_labels if combo in data[lbl]), None
        )
        if entry_ref is None:
            continue
        w1n, w2n = entry_ref["w1_name"], entry_ref["w2_name"]
        lines.append(f"### `{combo}` -- W1=`{w1n}`, W2=`{w2n}`")
        lines.append("")
        lines.append("| case | W1 duration (s) | W2 duration (s) |")
        lines.append("|---|---:|---:|")
        for case_label in all_labels:
            entry = data[case_label].get(combo)
            if not entry:
                lines.append(f"| {case_label} | - | - |")
                continue
            label_str = case_label
            if entry.get("is_multi"):
                label_str = f"{case_label} (n={entry['n_runs']})"
            lines.append(
                f"| {label_str} | "
                f"{fmt_mean_std(entry['w1'])} | {fmt_mean_std(entry['w2'])} |"
            )
        lines.append("")

    _emit_normalized(lines, data, combos, all_labels, ref_label="remote")
    _emit_normalized(lines, data, combos, all_labels, ref_label="local")

    return "\n".join(lines) + "\n"


def _emit_normalized(lines, data, combos, all_labels, ref_label):
    """Append a `## Normalized to <ref_label>` section plus a cross-combo
    gmean table. Each cell is `duration(case) / duration(ref_label)` so
    lower is better."""
    lines.append(f"## Normalized duration vs. {ref_label} baseline")
    lines.append("")
    lines.append(
        f"Each cell is `duration(case) / duration({ref_label})`. Values <1 "
        f"indicate a speed-up over the all-{ref_label} baseline (lower is "
        f"better); >1 indicates a slowdown."
    )
    lines.append(
        "`gmean` is the geometric mean of the W1 and W2 normalized durations "
        "for that combo/case."
    )
    lines.append("")
    gmeans_per_case = {label: [] for label in all_labels}
    for combo in combos:
        ref = data.get(ref_label, {}).get(combo)
        if not ref:
            continue
        w1n, w2n = ref["w1_name"], ref["w2_name"]
        lines.append(f"### `{combo}` -- W1=`{w1n}`, W2=`{w2n}`")
        lines.append("")
        lines.append("| case | W1 norm | W2 norm | gmean |")
        lines.append("|---|---:|---:|---:|")
        for case_label in all_labels:
            entry = data[case_label].get(combo)
            label_str = case_label
            if entry and entry.get("is_multi"):
                label_str = f"{case_label} (n={entry['n_runs']})"
            row = [label_str]
            ratios = []
            for w in ("w1", "w2"):
                r_dur = (ref[w] or {}).get("duration_sec")
                cur = ((entry[w] if entry else None) or {}).get("duration_sec")
                if r_dur is None or cur is None or r_dur == 0:
                    row.append("-")
                else:
                    r = cur / r_dur
                    row.append(f"{r:.3f}")
                    ratios.append(r)
            if len(ratios) == 2 and all(v > 0 for v in ratios):
                gm = math.sqrt(ratios[0] * ratios[1])
                row.append(f"{gm:.3f}")
                gmeans_per_case[case_label].append(gm)
            else:
                row.append("-")
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")

    lines.append(
        f"### Geometric mean of (case / {ref_label}) across combos"
    )
    lines.append("")
    lines.append(
        "For each case, `gmean(W1, W2)` is computed per combo, then a "
        "geometric mean across all combos. Lower is better."
    )
    lines.append("")
    lines.append(f"| case | gmean (case / {ref_label}) across combos |")
    lines.append("|---|---:|")
    for case_label in all_labels:
        vals = gmeans_per_case[case_label]
        if vals:
            log_sum = sum(math.log(v) for v in vals)
            gm = math.exp(log_sum / len(vals))
            lines.append(f"| {case_label} | {gm:.3f} |")
        else:
            lines.append(f"| {case_label} | - |")
    lines.append("")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument(
        "run_dir",
        type=Path,
        help="Path to the macrobenchmark result root (e.g. results/generated/raw/macrobench)",
    )
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output markdown path (default: <run-dir>/macrobench-duration-summary.md)",
    )
    ap.add_argument(
        "-p",
        "--post-warmup",
        action="store_true",
        help="Read `post_warmup_duration_sec` from duration.txt instead of `duration_sec`.",
    )
    args = ap.parse_args()

    run_dir = args.run_dir.resolve()
    out = args.output or (run_dir / "macrobench-duration-summary.md")
    duration_field = "post_warmup_duration_sec" if args.post_warmup else "duration_sec"
    out.write_text(build_report(run_dir, duration_field))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
