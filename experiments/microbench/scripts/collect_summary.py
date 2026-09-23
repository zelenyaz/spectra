#!/usr/bin/env python3
"""Collect mean Mops/s and GB/s from a microbenchmark (mbench) run.

mbench prints, per workload:

* per-second samples::

    [  75.0s] stride_read          821.98 Mops/s      6.58 GB/s

* a final totals line::

    [stride_read     ] 20048773120 ops      667.25 Mops/s avg      5.34 GB/s avg

Unlike membench, mbench reports a single ``GB/s`` figure (read BW for read
benches, write BW for write benches), so this collector tracks two metrics per
workload: ``mean_ops`` (Mops/s) and ``mean_GBs`` (GB/s).

Two directory layouts are supported, auto-detected from the run-dir structure:

**Standard layout** (``results-static-*`` or ``results/`` present):

* Static cases (whole-run average from the ``--- totals ---`` line)::

    <run-dir>/results-static-remote/<combo>/static/w{1,2}/stdout.log
    <run-dir>/results-static-local/<combo>/static/w{1,2}/stdout.log
    <run-dir>/results-static-only-W1-local/<combo>/static/w{1,2}/stdout.log
    <run-dir>/results-static-only-W2-local/<combo>/static/w{1,2}/stdout.log

* Dynamic-baseline cases::

    <run-dir>/results/<combo>/<baseline>/w{1,2}/stdout.log          (dual-proc)
    <run-dir>/results/<combo>/<baseline>/stdout.log                 (single-proc)

**Congestion-control layout** (``run{N}`` dirs directly under run-dir, each
containing ``<combo>/<baseline>/...``).  Used for directories such as
``congc-results/``::

    <run-dir>/run{N}/<combo>/<baseline>/w{1,2}/stdout.log           (dual-proc)
    <run-dir>/run{N}/<combo>/<baseline>/stdout.log                  (single-proc)

In the congc layout there are no static cases; all metrics come from
per-second samples (last ``tail-frac`` of the run).  The report normalises
against the first discovered dual-process baseline unless ``--congc-norm-ref``
overrides it.

Multi-run support: each case-root holds ``run*`` repeat subdirectories
(``run1``, ``run2``, ...); repeats are aggregated as mean +/- sample std.

``timeline.log`` (next to w1/w2) provides the W1/W2 workload names.

Emits ``<run-dir>/microbench-summary.md`` (or path passed via ``-o``).
Each metric is reported raw and normalized to a reference baseline.

Usage::

  python3 collect_summary.py <run-dir> [-o OUT.md] [--tail-frac 0.5]
  python3 collect_summary.py congc-results/ [-o OUT.md] [--congc-norm-ref colloid]
"""
import argparse
import math
import re
from pathlib import Path

# (parent dir under run-dir, subdir under each combo, label)
STATIC_CASES = [
    ("results-static-remote", "static", "remote"),
    ("results-static-local", "static", "local"),
    ("results-static-only-W1-local", "static", "only-W1-local"),
    ("results-static-only-W2-local", "static", "only-W2-local"),
]
DYNAMIC_PARENT = "results"

METRICS = ("mean_ops", "mean_GBs")

# [stride_read     ] 20048773120 ops      667.25 Mops/s avg      5.34 GB/s avg
_TOTALS_RE = re.compile(
    r"^\[\s*(\S+?)\s*\]\s+(\d+)\s+ops\s+([\d.]+)\s+Mops/s\s+avg\s+([\d.]+)\s+GB/s\s+avg"
)
# [  75.0s] stride_read          821.98 Mops/s      6.58 GB/s
_SAMPLE_RE = re.compile(
    r"^\[\s*([\d.]+)s\]\s+(\S+)\s+([\d.]+)\s+Mops/s\s+([\d.]+)\s+GB/s"
)


def parse_stdout_final(path: Path):
    """Static case: whole-run average from the final ``--- totals ---`` line.

    If several totals lines are present (multi-bench file), the last matching
    one wins; static w{1,2} files hold a single bench each.
    """
    ops = gbs = None
    with open(path) as f:
        for line in f:
            m = _TOTALS_RE.match(line.strip())
            if m:
                ops = float(m.group(3))
                gbs = float(m.group(4))
    return {"mean_ops": ops, "mean_GBs": gbs}


def _tail_mean(samples, tail_frac):
    """samples: list of (t, ops, gbs). Return mean over the last tail_frac."""
    if not samples:
        return {"mean_ops": None, "mean_GBs": None}
    samples.sort()
    n = len(samples)
    start = int(n * (1.0 - tail_frac))
    tail = samples[start:] or samples[-1:]
    k = len(tail)
    return {
        "mean_ops": sum(s[1] for s in tail) / k,
        "mean_GBs": sum(s[2] for s in tail) / k,
    }


def parse_stdout_dynamic(path: Path, tail_frac: float = 0.5):
    """Dynamic case: average per-second samples over the last ``tail_frac``."""
    samples = []
    with open(path) as f:
        for line in f:
            m = _SAMPLE_RE.match(line.strip())
            if m:
                samples.append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4)))
                )
    return _tail_mean(samples, tail_frac)


def parse_stdout_dynamic_concurrent(path: Path, tail_frac: float = 0.5):
    """Single-process concurrent case (e.g. libtiermem): one stdout.log holds
    interleaved per-second samples for multiple benches. Returns
    ``(per_bench_metrics, bench_order)`` where ``bench_order`` lists bench
    names in first-appearance order (= W1, W2, ...).
    """
    samples_by_bench: dict[str, list] = {}
    order: list[str] = []
    with open(path) as f:
        for line in f:
            m = _SAMPLE_RE.match(line.strip())
            if not m:
                continue
            bench = m.group(2)
            if bench not in samples_by_bench:
                samples_by_bench[bench] = []
                order.append(bench)
            samples_by_bench[bench].append(
                (float(m.group(1)), float(m.group(3)), float(m.group(4)))
            )
    result = {b: _tail_mean(s, tail_frac) for b, s in samples_by_bench.items()}
    return result, order


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
    """Locate per-repeat run directories for (combo, sub). Returns
    (paths, is_multi)."""
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


def _aggregate_metrics(metric_dicts):
    """List of {metric: value} -> {metric: mean, metric+'_std': std, '_n': k}."""
    agg = {"_n": len(metric_dicts)}
    for key in METRICS:
        vals = [m[key] for m in metric_dicts if m.get(key) is not None]
        if not vals:
            agg[key] = None
            agg[key + "_std"] = None
            continue
        mean = sum(vals) / len(vals)
        if len(vals) >= 2:
            var = sum((v - mean) ** 2 for v in vals) / (len(vals) - 1)
            std = math.sqrt(var)
        else:
            std = None
        agg[key] = mean
        agg[key + "_std"] = std
    return agg


def collect_case(case_root: Path, sub: str, parser):
    """Per combo: gather w1/w2 stdout metrics, averaging multi-run repeats."""
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
                stdout_path = rp / w / "stdout.log"
                if stdout_path.exists():
                    m = parser(stdout_path)
                    if m and any(v is not None for v in m.values()):
                        metric_dicts.append(m)
            entry[w] = _aggregate_metrics(metric_dicts) if metric_dicts else None
        out[combo] = entry
    return out


def _discover_baselines_in_root(root: Path) -> tuple[list, list]:
    """Return (dual_baselines, single_baselines) by scanning *root* directly.

    *root* must have the shape ``[run{N}/]{combo}/{baseline}/...``.
    dual_baselines have ``w1/`` and ``w2/`` subdirs; single_baselines have a
    top-level ``stdout.log`` (no w1/w2).
    """
    if not root.exists():
        return [], []
    dual: set[str] = set()
    single: set[str] = set()

    def _scan_combo(combo_dir: Path):
        for sub in combo_dir.iterdir():
            if not sub.is_dir():
                continue
            if (sub / "w1").exists() and (sub / "w2").exists():
                dual.add(sub.name)
            elif not (sub / "w1").exists() and not (sub / "w2").exists():
                if (sub / "stdout.log").exists():
                    single.add(sub.name)

    for d in root.iterdir():
        if not d.is_dir():
            continue
        if _is_run_dir(d):
            for combo_dir in d.iterdir():
                if combo_dir.is_dir():
                    _scan_combo(combo_dir)
        else:
            _scan_combo(d)
    return sorted(dual), sorted(single)


def discover_dynamic_baselines(run_dir: Path):
    """Dual-process baselines (have w1/ and w2/ subdirs)."""
    dual, _ = _discover_baselines_in_root(run_dir / DYNAMIC_PARENT)
    return dual


def discover_dynamic_single_baselines(run_dir: Path):
    """Single-process baselines (one stdout.log, no w1/w2 subdirs)."""
    _, single = _discover_baselines_in_root(run_dir / DYNAMIC_PARENT)
    return single


def collect_case_single_proc(case_root: Path, sub: str, tail_frac: float):
    """Single-process baselines: one stdout.log per combo with interleaved
    per-bench samples. Bench first-appearance order determines W1 vs W2."""
    out = {}
    if not case_root.exists():
        return out
    for combo in sorted(discover_combos(case_root)):
        run_paths, is_multi = find_run_paths(case_root, combo, sub)
        run_paths = [p for p in run_paths if (p / "stdout.log").exists()]
        if not run_paths:
            continue
        per_bench_dicts: dict[str, list] = {}
        bench_order: list[str] = []
        for rp in run_paths:
            per_bench, order = parse_stdout_dynamic_concurrent(
                rp / "stdout.log", tail_frac
            )
            if not bench_order and order:
                bench_order = order
            for bn, md in per_bench.items():
                if any(v is not None for v in md.values()):
                    per_bench_dicts.setdefault(bn, []).append(md)
        if not bench_order:
            continue
        w1_name = bench_order[0]
        w2_name = bench_order[1] if len(bench_order) > 1 else "?"
        out[combo] = {
            "w1_name": w1_name,
            "w2_name": w2_name,
            "is_multi": is_multi,
            "n_runs": len(run_paths),
            "w1": _aggregate_metrics(per_bench_dicts.get(w1_name, []))
            if w1_name in per_bench_dicts
            else None,
            "w2": _aggregate_metrics(per_bench_dicts.get(w2_name, []))
            if w2_name in per_bench_dicts
            else None,
        }
    return out


def fmt_mean_std(d, key, prec=3):
    if not d:
        return "-"
    v = d.get(key)
    if v is None:
        return "-"
    s = d.get(key + "_std")
    base = f"{v:.{prec}g}"
    return f"{base}±{s:.{prec}g}" if s is not None else base


def _norm_std(c, c_std, r, r_std):
    """Propagated std for norm = c/r via delta method: std(c/r) = (c/r)*sqrt((sc/c)^2+(sr/r)^2)."""
    if not c or not r:
        return None
    rel_c = (c_std / c) if c_std else 0.0
    rel_r = (r_std / r) if r_std else 0.0
    combined = math.sqrt(rel_c**2 + rel_r**2)
    return (c / r) * combined if combined else None


def fmt_norm(norm, std, prec=3):
    if norm is None:
        return "-"
    base = f"{norm:.{prec}f}"
    if std is not None and std > 0:
        return f"{base}±{std:.{prec}f}"
    return base


def _is_congc_layout(run_dir: Path) -> bool:
    """True when run_dir uses the congc layout: run{N}/<combo>/<baseline>/...
    directly under run_dir (no results/ or results-static-* subdirs)."""
    has_standard = any(
        (run_dir / parent).exists() for parent, _, _ in STATIC_CASES
    ) or (run_dir / DYNAMIC_PARENT).exists()
    if has_standard:
        return False
    dual, single = _discover_baselines_in_root(run_dir)
    return bool(dual or single)


def _build_normalized_table(
    lines: list,
    data: dict,
    all_labels: list,
    combos: list,
    ref_label: str,
) -> dict:
    """Append per-combo normalized tables to *lines*; return ops_gmeans_per_case."""
    ops_gmeans_per_case: dict[str, list] = {lbl: [] for lbl in all_labels}
    for combo in combos:
        rem = data[ref_label].get(combo)
        if not rem:
            continue
        w1n, w2n = rem["w1_name"], rem["w2_name"]
        lines.append(f"### `{combo}` — W1=`{w1n}`, W2=`{w2n}`")
        lines.append("")
        lines.append("| case | W1 ops | W1 GB/s | W2 ops | W2 GB/s | gmean(ops) |")
        lines.append("|---|---:|---:|---:|---:|---:|")
        for case_label in all_labels:
            entry = data[case_label].get(combo)
            label_str = case_label
            if entry and entry.get("is_multi"):
                label_str = f"{case_label} (n={entry['n_runs']})"
            row = [label_str]
            ops_norms = []
            for w in ("w1", "w2"):
                ref = rem[w] or {}
                cur = (entry[w] if entry else None) or {}
                for key in METRICS:
                    r = ref.get(key)
                    c = cur.get(key)
                    if r is None or c is None or r == 0:
                        row.append("-")
                    else:
                        norm = c / r
                        ns = _norm_std(c, cur.get(key + "_std"), r, ref.get(key + "_std"))
                        row.append(fmt_norm(norm, ns))
                        if key == "mean_ops":
                            ops_norms.append((norm, ns))
            if len(ops_norms) == 2 and all(v[0] > 0 for v in ops_norms):
                n1, s1 = ops_norms[0]
                n2, s2 = ops_norms[1]
                gm = math.sqrt(n1 * n2)
                rel1 = (s1 / n1) if s1 else 0.0
                rel2 = (s2 / n2) if s2 else 0.0
                combined = math.sqrt((rel1 / 2) ** 2 + (rel2 / 2) ** 2)
                gm_std = gm * combined if combined else None
                row.append(fmt_norm(gm, gm_std))
                ops_gmeans_per_case[case_label].append((gm, gm_std))
            else:
                row.append("-")
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
    return ops_gmeans_per_case


def _build_gmean_table(lines: list, all_labels: list, ops_gmeans_per_case: dict):
    """Append the cross-combo geometric-mean summary table to *lines*."""
    lines.append("| case | gmean(ops) across combos | n combos |")
    lines.append("|---|---:|---:|")
    for case_label in all_labels:
        vals = ops_gmeans_per_case[case_label]
        if vals:
            gm_vals = [v[0] for v in vals]
            gm = math.exp(sum(math.log(v) for v in gm_vals) / len(gm_vals))
            if len(gm_vals) >= 2:
                mean_v = sum(gm_vals) / len(gm_vals)
                var = sum((v - mean_v) ** 2 for v in gm_vals) / (len(gm_vals) - 1)
                std = math.sqrt(var)
                lines.append(f"| {case_label} | {gm:.3f}±{std:.3f} | {len(gm_vals)} |")
            else:
                lines.append(f"| {case_label} | {gm:.3f} | {len(gm_vals)} |")
        else:
            lines.append(f"| {case_label} | - | 0 |")
    lines.append("")


def build_congc_report(run_dir: Path, tail_frac: float, norm_ref: str | None) -> str:
    """Standalone report for a congc-layout run dir (run{N}/<combo>/<baseline>)."""
    dual_baselines, single_baselines = _discover_baselines_in_root(run_dir)
    all_labels = dual_baselines + single_baselines

    data: dict[str, dict] = {}
    for baseline in dual_baselines:
        parser = lambda p, _tf=tail_frac: parse_stdout_dynamic(p, _tf)
        data[baseline] = collect_case(run_dir, baseline, parser)
    for baseline in single_baselines:
        data[baseline] = collect_case_single_proc(run_dir, baseline, tail_frac)

    combos = sorted({c for case in data.values() for c in case.keys()})

    ref_label = norm_ref
    if ref_label is None:
        ref_label = dual_baselines[0] if dual_baselines else (single_baselines[0] if single_baselines else None)

    lines: list[str] = []
    lines.append(f"# Congestion-control (mbench) summary ({run_dir.name})")
    lines.append("")
    lines.append(f"Source: `{run_dir}`")
    lines.append("")
    lines.append(
        f"Metrics from per-second samples over the last {tail_frac:.0%} of each run "
        "(post-migration steady state)."
    )
    lines.append("")
    if dual_baselines:
        lines.append(
            "Dual-process baselines (separate w1/w2 processes): "
            + ", ".join(f"`{b}`" for b in dual_baselines)
        )
    if single_baselines:
        lines.append(
            "Single-process baselines (interleaved benches in one stdout.log): "
            + ", ".join(f"`{b}`" for b in single_baselines)
        )
    if ref_label:
        lines.append(f"Normalization reference: `{ref_label}`")
    lines.append("")

    # Raw metrics
    lines.append("## Raw metrics")
    lines.append("")
    lines.append(
        "Multi-run cells show `mean±std` (sample std, n>1); run count annotated as `(n=k)`."
    )
    lines.append("")
    for combo in combos:
        entry_ref = next(
            (data[lbl][combo] for lbl in all_labels if combo in data[lbl]), None
        )
        if entry_ref is None:
            continue
        w1n, w2n = entry_ref["w1_name"], entry_ref["w2_name"]
        lines.append(f"### `{combo}` — W1=`{w1n}`, W2=`{w2n}`")
        lines.append("")
        lines.append("| case | W1 Mops/s | W1 GB/s | W2 Mops/s | W2 GB/s |")
        lines.append("|---|---:|---:|---:|---:|")
        for case_label in all_labels:
            entry = data[case_label].get(combo)
            if not entry:
                lines.append(f"| {case_label} | - | - | - | - |")
                continue
            label_str = case_label
            if entry.get("is_multi"):
                label_str = f"{case_label} (n={entry['n_runs']})"
            w1 = entry["w1"] or {}
            w2 = entry["w2"] or {}
            lines.append(
                f"| {label_str} | "
                f"{fmt_mean_std(w1, 'mean_ops')} | {fmt_mean_std(w1, 'mean_GBs')} | "
                f"{fmt_mean_std(w2, 'mean_ops')} | {fmt_mean_std(w2, 'mean_GBs')} |"
            )
        lines.append("")

    # Normalized metrics
    if ref_label and ref_label in data:
        lines.append(f"## Normalized to `{ref_label}`")
        lines.append("")
        lines.append(
            f"Each cell is `metric(case) / metric({ref_label})`. "
            "Values >1 indicate a speed-up over the reference baseline. "
            "`gmean(ops)` is the geometric mean of normalised W1 and W2 ops."
        )
        lines.append("")
        ops_gmeans = _build_normalized_table(lines, data, all_labels, combos, ref_label)

        lines.append(f"## Geometric mean across combos (vs `{ref_label}`)")
        lines.append("")
        lines.append(
            "For each case, `gmean(W1_ops, W2_ops)` per combo, then geometric mean "
            "across all combos. Std is the sample std of per-combo gmeans."
        )
        lines.append("")
        _build_gmean_table(lines, all_labels, ops_gmeans)

    return "\n".join(lines) + "\n"


def build_report(run_dir: Path, tail_frac: float):
    data = {}
    all_labels: list[str] = []

    for parent, sub, label in STATIC_CASES:
        data[label] = collect_case(run_dir / parent, sub, parse_stdout_final)
        all_labels.append(label)

    dynamic_baselines = discover_dynamic_baselines(run_dir)
    for baseline in dynamic_baselines:
        parser = lambda p, _tf=tail_frac: parse_stdout_dynamic(p, _tf)
        data[baseline] = collect_case(run_dir / DYNAMIC_PARENT, baseline, parser)
        all_labels.append(baseline)

    single_baselines = discover_dynamic_single_baselines(run_dir)
    for baseline in single_baselines:
        data[baseline] = collect_case_single_proc(
            run_dir / DYNAMIC_PARENT, baseline, tail_frac
        )
        all_labels.append(baseline)

    combos = sorted({c for case in data.values() for c in case.keys()})

    lines = []
    lines.append(f"# New-micro (mbench) summary ({run_dir.name})")
    lines.append("")
    lines.append(f"Source: `{run_dir}`")
    lines.append("")
    lines.append("Per-workload metrics from `<combo>/<case>/w{1,2}/stdout.log`:")
    lines.append(
        "- `mean_ops` = throughput in Mops/s; `mean_GBs` = bandwidth in GB/s "
        "(read BW for read benches, write BW for write benches)"
    )
    lines.append(
        "- Static cases use the whole-run average (`--- totals --- ... avg`)."
    )
    lines.append(
        f"- Dynamic baselines use the mean of per-second samples over the last "
        f"{tail_frac:.0%} of the run (post-migration steady state)."
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
            "Dual-process dynamic baselines: "
            + ", ".join(f"`{b}`" for b in dynamic_baselines)
        )
        lines.append("")
    if single_baselines:
        lines.append(
            "Single-process baselines (one stdout.log per combo, split by "
            "bench name): " + ", ".join(f"`{b}`" for b in single_baselines)
        )
        lines.append("")
    lines.append("Workloads pinned to node-0 cores (0-27) in all cases.")
    lines.append("")

    lines.append("## Raw metrics")
    lines.append("")
    lines.append(
        "Multi-run cells show `mean±std` (sample std, n>1); run count is "
        "annotated on the case label as `(n=k)`."
    )
    lines.append("")
    for combo in combos:
        entry_ref = next(
            (data[lbl][combo] for lbl in all_labels if combo in data[lbl]), None
        )
        if entry_ref is None:
            continue
        w1n, w2n = entry_ref["w1_name"], entry_ref["w2_name"]
        lines.append(f"### `{combo}` — W1=`{w1n}`, W2=`{w2n}`")
        lines.append("")
        lines.append(
            "| case | W1 Mops/s | W1 GB/s | W2 Mops/s | W2 GB/s |"
        )
        lines.append("|---|---:|---:|---:|---:|")
        for case_label in all_labels:
            entry = data[case_label].get(combo)
            if not entry:
                lines.append(f"| {case_label} | - | - | - | - |")
                continue
            label_str = case_label
            if entry.get("is_multi"):
                label_str = f"{case_label} (n={entry['n_runs']})"
            w1 = entry["w1"] or {}
            w2 = entry["w2"] or {}
            lines.append(
                f"| {label_str} | "
                f"{fmt_mean_std(w1, 'mean_ops')} | {fmt_mean_std(w1, 'mean_GBs')} | "
                f"{fmt_mean_std(w2, 'mean_ops')} | {fmt_mean_std(w2, 'mean_GBs')} |"
            )
        lines.append("")

    lines.append("## Normalized to remote case")
    lines.append("")
    lines.append(
        "Each cell is `metric(case) / metric(remote)`. Values >1 indicate a "
        "speed-up over the all-remote baseline. `gmean(ops)` is the geometric "
        "mean of normalized W1 ops and W2 ops for that combo/case."
    )
    lines.append("")
    ops_gmeans_per_case = _build_normalized_table(lines, data, all_labels, combos, "remote")

    lines.append("## Geometric mean of normalized ops across combos")
    lines.append("")
    lines.append(
        "For each case, `gmean(W1_ops, W2_ops)` is computed per combo, then a "
        "geometric mean across all combos (vs static-remote = 1.000). "
        "Std shown is the sample std of per-combo gmeans."
    )
    lines.append("")
    _build_gmean_table(lines, all_labels, ops_gmeans_per_case)

    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("run_dir", type=Path, help="Path to the microbenchmark run dir")
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--tail-frac", type=float, default=0.5)
    ap.add_argument(
        "--congc-norm-ref",
        metavar="LABEL",
        default=None,
        help="Reference baseline for normalisation in congc layout "
             "(default: first discovered dual-process baseline)",
    )
    args = ap.parse_args()

    if not (0.0 < args.tail_frac <= 1.0):
        ap.error("--tail-frac must be in (0, 1]")

    run_dir = args.run_dir.resolve()
    out = args.output or (run_dir / "microbench-summary.md")

    if _is_congc_layout(run_dir):
        text = build_congc_report(run_dir, args.tail_frac, args.congc_norm_ref)
    else:
        text = build_report(run_dir, args.tail_frac)

    out.write_text(text)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
