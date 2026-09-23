#!/usr/bin/env python3
"""
Extract micro-benchmark evaluation data from results/generated/raw/microbench/results.

Produces two data tables under data/:
  - data/eval-micro-attrib.md : per-object attribution breakdown (libtiermem
    single-process runs) with estimated vs. iMC-measured per-node bandwidth and
    the attribution accuracy.
  - data/eval-micro-bw.md     : steady-state per-node read/write iMC bandwidth
    per baseline per pair (for the bandwidth-utilization stacked bars).

Ground truth for attribution accuracy: each micro pair runs two 4 GiB objects
with a 4 GiB DRAM budget, so libtiermem converges to exactly one object on
node 0 (local) and one on node 1 (remote).  The iMC per-node RD/WR counters
in the libtiermem epoch log are therefore the per-object ground truth.
"""

import os
import re
import statistics as st
from datetime import datetime
from glob import glob
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = Path(os.environ.get(
    "SPECTRA_MICRO_RESULTS", REPO / "results/generated/raw/microbench/results"))
DATA = Path(os.environ.get(
    "SPECTRA_DATA_DIR", REPO / "results/generated/data"))
DATA.mkdir(parents=True, exist_ok=True)

# combo id -> (W1 bench, W2 bench)
COMBOS = {
    "sdrd-chksdrd": ("stride_read", "chk_stride_read"),
    "sdprd-rr":     ("stridep_read", "rand_read"),
    "sdrdpf-rr":    ("stride_pf_read", "rand_read"),
    "sdrd-sdrw":    ("stride_read", "stride_rw"),
    "sdrd-sdwr":    ("stride_read", "stride_wr"),
}
KERNEL_BASELINES = ["tpp", "colloid", "alto", "memtis", "mttm"]

# ── libtiermem stderr parsing ──────────────────────────────────────────────
EPOCH_RE = re.compile(r"=== Epoch (\d+) ")
IMC_RD_RE = re.compile(r"iMC RD: node0=([\d.]+) GB/s node1=([\d.]+) GB/s")
IMC_WR_RE = re.compile(r"iMC WR: node0=([\d.]+) GB/s node1=([\d.]+) GB/s")
OBJ_RE = re.compile(
    r"obj (0x[0-9a-f]+) \((\d+) pages, node (\d+), pid \d+\):.*?"
    r"reg_ld=([\d.]+) (?:reg_st_raw=[\d.]+ )?reg_st=([\d.]+) "
    r"dd_rd=([\d.eE+-]+) hw_dp=([\d.eE+-]+) sw_dp=([\d.eE+-]+) "
    r"d_rfo=([\d.eE+-]+) hw_rp=([\d.eE+-]+) shl=([\d.eE+-]+) "
    r"rd_bw=([\d.]+) GB/s wr_bw=([\d.]+) GB/s bw=([\d.]+) GB/s"
)


def parse_libtiermem(path):
    """Return list of epoch dicts: {imc_rd:(n0,n1), imc_wr:(n0,n1), objs:[...]}.
    Each obj: addr,pages,node,reg_ld,reg_st,dd_rd,hw_dp,sw_dp,d_rfo,hw_rp,shl,
              rd_bw,wr_bw,bw."""
    epochs = []
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            if "=== Epoch" in line:
                if cur is not None and cur.get("objs"):
                    epochs.append(cur)
                cur = {"imc_rd": None, "imc_wr": None, "objs": []}
                continue
            if cur is None:
                continue
            m = IMC_RD_RE.search(line)
            if m:
                cur["imc_rd"] = (float(m.group(1)), float(m.group(2)))
                continue
            m = IMC_WR_RE.search(line)
            if m:
                cur["imc_wr"] = (float(m.group(1)), float(m.group(2)))
                continue
            m = OBJ_RE.search(line)
            if m:
                cur["objs"].append({
                    "addr": m.group(1), "pages": int(m.group(2)),
                    "node": int(m.group(3)),
                    "reg_ld": float(m.group(4)), "reg_st": float(m.group(5)),
                    "dd_rd": float(m.group(6)), "hw_dp": float(m.group(7)),
                    "sw_dp": float(m.group(8)), "d_rfo": float(m.group(9)),
                    "hw_rp": float(m.group(10)), "shl": float(m.group(11)),
                    "rd_bw": float(m.group(12)), "wr_bw": float(m.group(13)),
                    "bw": float(m.group(14)),
                })
    if cur is not None and cur.get("objs"):
        epochs.append(cur)
    return epochs


def steady_epochs(epochs):
    """Return the last-50% epochs that have 2 objects + iMC reading."""
    good = [e for e in epochs if len(e["objs"]) == 2 and e["imc_rd"]]
    if not good:
        return []
    return good[len(good) // 2:] if len(good) >= 2 else good


def attribution_rows():
    """Per combo, per object: averaged traffic breakdown + est vs measured BW.

    Returns dict combo -> { 'W1'/'W2' -> {bench, node, shares..., est_bw,
    meas_bw, err_pct} } aggregated over runs (mean)."""
    out = {}
    for combo, (b1, b2) in COMBOS.items():
        # accumulate per-bench across runs
        acc = {b1: [], b2: []}
        for run_dir in sorted(glob(str(RESULTS / "run*" / combo / "libtiermem"))):
            stderr = Path(run_dir) / "stderr.log"
            if not stderr.exists():
                continue
            stdout = Path(run_dir) / "stdout.log"
            # map object addr order to bench: parse stdout header order
            bench_order = parse_bench_addrs(stdout)
            eps = steady_epochs(parse_libtiermem(stderr))
            if not eps:
                continue
            # average object metrics over steady epochs, keyed by addr
            per_addr = {}
            for e in eps:
                for o in e["objs"]:
                    per_addr.setdefault(o["addr"], []).append((o, e))
            for addr, lst in per_addr.items():
                bench = bench_order.get(addr)
                if bench is None:
                    continue
                # traffic components (lines, 64B each) -> fractions
                comp = {k: st.mean([o[k] for o, _ in lst])
                        for k in ("dd_rd", "hw_dp", "sw_dp", "d_rfo", "hw_rp", "shl")}
                node = round(st.mean([o["node"] for o, _ in lst]))
                est_bw = st.mean([o["bw"] for o, _ in lst])
                # measured = iMC RD+WR on the object's node
                meas = []
                for o, e in lst:
                    n = o["node"]
                    rd = e["imc_rd"][n] if e["imc_rd"] else 0.0
                    wr = e["imc_wr"][n] if e["imc_wr"] else 0.0
                    meas.append(rd + wr)
                meas_bw = st.mean(meas)
                acc[bench].append({
                    "node": node, "comp": comp,
                    "est_bw": est_bw, "meas_bw": meas_bw,
                })
        # aggregate across runs (mean of per-run means)
        res = {}
        for slot, bench in (("W1", b1), ("W2", b2)):
            runs = acc[bench]
            if not runs:
                continue
            comp = {k: st.mean([r["comp"][k] for r in runs])
                    for k in ("dd_rd", "hw_dp", "sw_dp", "d_rfo", "hw_rp", "shl")}
            est = st.mean([r["est_bw"] for r in runs])
            meas = st.mean([r["meas_bw"] for r in runs])
            node = round(st.mean([r["node"] for r in runs]))
            err = abs(est - meas) / meas * 100 if meas > 0 else float("nan")
            res[slot] = {"bench": bench, "node": node, "comp": comp,
                         "est_bw": est, "meas_bw": meas, "err_pct": err,
                         "n": len(runs)}
        out[combo] = res
    return out


def parse_bench_addrs(stdout_path):
    """Map object start address -> bench name from membench stdout header.
    Header lines:
      [<bench>] order=...
          buffer: start=0x... size=...
    The tracked object start addr differs from the buffer start by a small
    offset (allocator metadata).  We map by matching the high bits."""
    addr_to_bench = {}
    if not Path(stdout_path).exists():
        return addr_to_bench
    cur_bench = None
    for line in Path(stdout_path).read_text(errors="replace").splitlines():
        m = re.match(r"\[(\w+)\] order=", line)
        if m:
            cur_bench = m.group(1)
            continue
        m = re.search(r"buffer: start=(0x[0-9a-f]+)", line)
        if m and cur_bench:
            addr_to_bench[m.group(1)] = cur_bench
    return addr_to_bench


# ── iMC bandwidth-utilization parsing ──────────────────────────────────────
CAS_RE = re.compile(r"^\s*([\d.]+)\s+([\d,]+)\s+unc_m_cas_count\.(rd|wr)")
PERF_START_RE = re.compile(r"# started on (.*)")
TIMELINE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+(.*)$")


def parse_perf_start(path):
    """Return perf's absolute start time from an imc-cas log."""
    for line in Path(path).read_text(errors="replace").splitlines():
        m = PERF_START_RE.match(line)
        if m:
            return datetime.strptime(m.group(1).strip(), "%a %b %d %H:%M:%S %Y")
    return None


def parse_workload_window(run_dir):
    """Return (launch, finish) timestamps from timeline.log."""
    timeline = Path(run_dir) / "timeline.log"
    launch = None
    finishes = []
    if not timeline.exists():
        return None
    for line in timeline.read_text(errors="replace").splitlines():
        m = TIMELINE_RE.match(line)
        if not m:
            continue
        ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
        msg = m.group(2)
        if "Launching workloads" in msg:
            launch = ts
        elif re.search(r"W[12]\([^)]*\) finished", msg):
            finishes.append(ts)
    if launch is None or not finishes:
        return None
    return launch, max(finishes)


def parse_imc_cas(path):
    """Return list of (t, rd_count, wr_count) per second from perf -I log."""
    rows = {}
    if not Path(path).exists():
        return []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = CAS_RE.match(line)
        if not m:
            continue
        t = float(m.group(1))
        cnt = int(m.group(2).replace(",", ""))
        kind = m.group(3)
        rows.setdefault(t, {})[kind] = cnt
    series = []
    for t in sorted(rows):
        r = rows[t]
        if "rd" in r and "wr" in r:
            series.append((t, r["rd"], r["wr"]))
    return series


def imc_bw_workload_steady(run_dir, socket, interval=1.0):
    """Mean read/write BW over the last half of the active workload window.

    The suite-level perf logs start before workload launch and can continue
    through profiler shutdown and cleanup.  Align to timeline.log so the
    steady-state mean excludes setup and teardown samples.
    """
    path = Path(run_dir) / f"imc-cas-C{socket}.log"
    series = parse_imc_cas(path)
    start = parse_perf_start(path)
    window = parse_workload_window(run_dir)
    if not series or start is None or window is None:
        return None

    launch, finish = window
    launch_s = (launch - start).total_seconds()
    finish_s = (finish - start).total_seconds()
    if finish_s <= launch_s:
        return None
    mid_s = launch_s + (finish_s - launch_s) / 2.0
    active = [(t, r, w) for t, r, w in series if mid_s <= t <= finish_s]
    if not active:
        windowed = [(t, r, w) for t, r, w in series if launch_s <= t <= finish_s]
        active = windowed[len(windowed) // 2:]
    if not active:
        return None
    rd = st.mean([r * 64 / 1e9 / interval for _, r, _ in active])
    wr = st.mean([w * 64 / 1e9 / interval for _, _, w in active])
    return rd, wr


def bandwidth_rows():
    """Per combo, per baseline: (node0_rd, node0_wr, node1_rd, node1_wr) GB/s,
    averaged across runs."""
    out = {}
    for combo in COMBOS:
        out[combo] = {}
        for base in KERNEL_BASELINES:
            n0r, n0w, n1r, n1w = [], [], [], []
            for d in sorted(glob(str(RESULTS / "run*" / combo / base))):
                c0 = imc_bw_workload_steady(Path(d), 0)
                c28 = imc_bw_workload_steady(Path(d), 28)
                if c0 and c28:
                    n0r.append(c0[0]); n0w.append(c0[1])
                    n1r.append(c28[0]); n1w.append(c28[1])
            if n0r:
                out[combo][base] = (st.mean(n0r), st.mean(n0w),
                                    st.mean(n1r), st.mean(n1w), len(n0r))
        # libtiermem: from stderr iMC lines
        n0r, n0w, n1r, n1w = [], [], [], []
        for d in sorted(glob(str(RESULTS / "run*" / combo / "libtiermem"))):
            eps = steady_epochs(parse_libtiermem(Path(d) / "stderr.log"))
            if not eps:
                continue
            n0r.append(st.mean([e["imc_rd"][0] for e in eps]))
            n1r.append(st.mean([e["imc_rd"][1] for e in eps]))
            n0w.append(st.mean([e["imc_wr"][0] for e in eps]))
            n1w.append(st.mean([e["imc_wr"][1] for e in eps]))
        if n0r:
            out[combo]["libtiermem"] = (st.mean(n0r), st.mean(n0w),
                                        st.mean(n1r), st.mean(n1w), len(n0r))
    return out


# ── emit ───────────────────────────────────────────────────────────────────
def fmt_e(x):
    return f"{x:.2e}"


def write_attrib(rows):
    lines = ["# Micro-benchmark per-object attribution (libtiermem single-proc)",
             "#",
             "# Ground truth: each pair runs 2x 4GiB objects with a 4GiB DRAM budget,",
             "# so the converged placement is 1 object on node0 (local) + 1 on node1",
             "# (remote). iMC per-node RD+WR is therefore the per-object ground truth.",
             "# Traffic components are 64B-line counts per epoch (means over last-50%",
             "# epochs, then over runs). est_bw/meas_bw in GB/s; err = |est-meas|/meas.",
             "#",
             "# combo | slot | bench | node | dd_rd | hw_dp | sw_dp | d_rfo | hw_rp | shl | est_bw | meas_bw | err_pct | n",
             ""]
    lines.append("| combo | slot | bench | node | dd_rd | hw_dp | sw_dp | d_rfo | hw_rp | shl | est_bw | meas_bw | err% | n |")
    lines.append("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    errs = []
    for combo in COMBOS:
        for slot in ("W1", "W2"):
            r = rows.get(combo, {}).get(slot)
            if not r:
                continue
            c = r["comp"]
            errs.append(r["err_pct"])
            lines.append(
                f"| {combo} | {slot} | {r['bench']} | {r['node']} | "
                f"{fmt_e(c['dd_rd'])} | {fmt_e(c['hw_dp'])} | {fmt_e(c['sw_dp'])} | "
                f"{fmt_e(c['d_rfo'])} | {fmt_e(c['hw_rp'])} | {fmt_e(c['shl'])} | "
                f"{r['est_bw']:.2f} | {r['meas_bw']:.2f} | {r['err_pct']:.2f} | {r['n']} |")
    lines.append("")
    lines.append(f"# mean |err| across objects = {st.mean(errs):.2f}%  "
                 f"median = {st.median(errs):.2f}%  max = {max(errs):.2f}%  "
                 f"(n={len(errs)} objects)")
    (DATA / "eval-micro-attrib.md").write_text("\n".join(lines) + "\n")
    print("wrote data/eval-micro-attrib.md")
    print(f"  attribution: mean|err|={st.mean(errs):.2f}% median={st.median(errs):.2f}% max={max(errs):.2f}%")


def write_bw(rows):
    lines = ["# Micro-benchmark steady-state iMC bandwidth utilization (GB/s)",
             "# node0 = local/primary tier, node1 = remote/expansion tier.",
             "# Means over the last half of each active workload window, then over runs.",
             "#",
             "| combo | baseline | n0_rd | n0_wr | n1_rd | n1_wr | n |",
             "|---|---|---:|---:|---:|---:|---:|"]
    order = ["tpp", "colloid", "alto", "memtis", "mttm", "libtiermem"]
    for combo in COMBOS:
        for base in order:
            v = rows.get(combo, {}).get(base)
            if not v:
                continue
            lines.append(f"| {combo} | {base} | {v[0]:.1f} | {v[1]:.1f} | "
                         f"{v[2]:.1f} | {v[3]:.1f} | {v[4]} |")
    (DATA / "eval-micro-bw.md").write_text("\n".join(lines) + "\n")
    print("wrote data/eval-micro-bw.md")


if __name__ == "__main__":
    arows = attribution_rows()
    write_attrib(arows)
    brows = bandwidth_rows()
    write_bw(brows)
