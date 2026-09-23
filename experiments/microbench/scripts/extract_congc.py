#!/usr/bin/env python3
"""
Extract congestion-control time series from results/generated/raw/congestion.

Scenario: both workloads start fully on the local (primary) tier, which is
bandwidth-saturated by the high-intensity stride workload.  We compare how each
scheme relieves the congestion.

Emits data/eval-congc.md with, per scheme, two per-second/per-epoch series:
  - W1 (high-BW stride) throughput in GB/s over time
  - cumulative pages migrated over time

For Colloid, migration = cumsum(pgmigrate_success) from vvmstat.  For MTColloid,
migration = cumsum(htmm_nr_promoted + htmm_nr_demoted + htmm_nr_cong_demoted)
from vvmstat.  Throughput
comes from w1/stdout.log.  For libtiermem, throughput comes from the interleaved
stdout.log and migration comes from the [cc] "migrated N pages" log lines
(epoch-stamped; 1 s epochs).  Also prints headline totals.
"""

import csv
import os
import re
from glob import glob
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CONGC = Path(os.environ.get(
    "SPECTRA_CONGC_RESULTS", REPO / "results/generated/raw/congestion"))
DATA = Path(os.environ.get(
    "SPECTRA_DATA_DIR", REPO / "results/generated/data"))
DATA.mkdir(parents=True, exist_ok=True)

COMBO = "sdprw-rrw"   # stride+rand (most illustrative); W1=stridep_rw W2=rand_rw
W1_BENCH = "stridep_rw"
WARMUP_SEC = 10.0

TPUT_RE = re.compile(r"\[\s*([\d.]+)s\]\s+(\w+)\s+([\d.]+) Mops/s\s+([\d.]+) GB/s")


def parse_tput(stdout_path, bench):
    """Return [(t, GB/s)] for the named bench from a membench stdout."""
    out = []
    if not Path(stdout_path).exists():
        return out
    for line in Path(stdout_path).read_text(errors="replace").splitlines():
        m = TPUT_RE.match(line.strip())
        if m and m.group(2) == bench:
            out.append((float(m.group(1)), float(m.group(4))))
    return out


def parse_vvmstat_migration(csv_path, columns, *, align_warmup=False):
    """Return [(elapsed_sec, cumulative_pages_migrated)] from named counters.

    The kernel vvmstat sampler starts before the 10 s warmup, while Spectra's
    internal congestion log starts after warmup.  For Figure 9(b), align the
    kernel migration timeline to Spectra by subtracting the warmup interval.
    """
    out = []
    if not Path(csv_path).exists():
        return out
    cum = 0
    with Path(csv_path).open(newline="") as f:
        for row in csv.DictReader(f):
            elapsed = float(row["elapsed_sec"])
            if align_warmup:
                elapsed -= WARMUP_SEC
                if elapsed < 0:
                    continue
            cum += sum(int(row[c]) for c in columns)
            out.append((elapsed, cum))
    return out


def parse_libtiermem_migration(stderr_path):
    """Return [(epoch_elapsed_sec, cumulative_pages)] from [cc] migrated lines.
    Epochs are 1 s; we count epoch headers to assign a time axis."""
    out = []
    cum = 0
    epoch_t = 0.0
    if not Path(stderr_path).exists():
        return out
    for line in Path(stderr_path).read_text(errors="replace").splitlines():
        if "=== Epoch" in line:
            mt = re.search(r"T=([\d.]+)s", line)
            epoch_t += float(mt.group(1)) if mt else 1.0
        m = re.search(r"\[cc\] pid \d+ migrated (\d+) pages", line)
        if m:
            cum += int(m.group(1))
            out.append((epoch_t, cum))
    # extend a flat tail so the step is visible until run end
    return out


def parse_libtiermem_cc_latency(stderr_path):
    """Return [(epoch_elapsed, L_local, L_remote, r)] from [cc] L_D lines."""
    out = []
    epoch_t = 0.0
    for line in Path(stderr_path).read_text(errors="replace").splitlines():
        if "=== Epoch" in line:
            mt = re.search(r"T=([\d.]+)s", line)
            epoch_t += float(mt.group(1)) if mt else 1.0
        m = re.search(r"\[cc\] L_D=([\d.]+) L_A=([\d.]+).*?r=([\d.]+)", line)
        if m:
            out.append((epoch_t, float(m.group(1)), float(m.group(2)), float(m.group(3))))
    return out


def parse_colloid_latency(path):
    """Return [(elapsed, L_primary, L_expansion)] from colloid-mon's latency log.
    Lines look like '<unix_ts> local <Lp> remote <Le>, <flag>'.  Time is made
    relative to the first sample (the moment latency monitoring begins)."""
    out = []
    t0 = None
    if not Path(path).exists():
        return out
    for line in Path(path).read_text(errors="replace").splitlines():
        m = re.match(r"([\d.]+)\s+local\s+(\d+)\s+remote\s+(\d+)", line.strip())
        if not m:
            continue
        ts = float(m.group(1))
        t0 = ts if t0 is None else t0
        out.append((ts - t0, int(m.group(2)), int(m.group(3))))
    return out


def parse_mtcolloid_latency(path):
    """Return [(elapsed, L_primary, L_expansion)] from htmm-latency.log.
    Columns: 'timestamp local_lat remote_lat local_rate remote_rate'.  The
    header and any short/truncated trailing lines are skipped.  Time is made
    relative to the first sample."""
    out = []
    t0 = None
    if not Path(path).exists():
        return out
    for line in Path(path).read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) < 3 or not re.match(r"^[\d.]+$", parts[0]):
            continue  # header or malformed
        try:
            ts, lp, le = float(parts[0]), int(parts[1]), int(parts[2])
        except ValueError:
            continue
        t0 = ts if t0 is None else t0
        out.append((ts - t0, lp, le))
    return out


def series_to_str(series):
    return ";".join(f"{t:.1f},{v:.2f}" for t, v in series)


def select_run():
    required = ("colloid/w1/stdout.log", "colloid/vvmstat.mstat.csv",
                "mtcolloid/w1/stdout.log", "mtcolloid/vvmstat.mstat.csv",
                "mtcolloid/htmm-latency.log", "libtiermem/stdout.log",
                "libtiermem/stderr.log")
    selected = os.environ.get("SPECTRA_CONGC_RUN_ID")
    if selected:
        candidates = [CONGC / selected]
    else:
        candidates = sorted(
            (p for p in CONGC.glob("run*") if p.is_dir() and p.name[3:].isdigit()),
            key=lambda p: int(p.name[3:]), reverse=True)
    for candidate in candidates:
        if all(path.is_file() and path.stat().st_size > 0
               for path in (candidate / COMBO / name for name in required)):
            return candidate.name
    raise FileNotFoundError(f"no complete congestion run for {COMBO} under {CONGC}")


def main():
    run = select_run()
    base = CONGC / run / COMBO

    schemes = {
        "colloid": ("colloid", "vvmstat"),
        "mtcolloid": ("mtcolloid", "vvmstat"),
        "libtiermem": ("libtiermem", "ltm"),
    }

    tput = {}
    migr = {}
    for name, (d, kind) in schemes.items():
        if kind == "vvmstat":
            tput[name] = parse_tput(base / d / "w1" / "stdout.log", W1_BENCH)
            if name == "mtcolloid":
                columns = ("htmm_nr_promoted", "htmm_nr_demoted", "htmm_nr_cong_demoted")
            else:
                columns = ("pgmigrate_success",)
            migr[name] = parse_vvmstat_migration(
                base / d / "vvmstat.mstat.csv",
                columns,
                align_warmup=True,
            )
        else:
            tput[name] = parse_tput(base / d / "stdout.log", W1_BENCH)
            migr[name] = parse_libtiermem_migration(base / d / "stderr.log")

    lat = parse_libtiermem_cc_latency(base / "libtiermem" / "stderr.log")
    colloid_lat = parse_colloid_latency(base / "colloid" / "colloid-latency.log")
    mtcolloid_lat = parse_mtcolloid_latency(base / "mtcolloid" / "htmm-latency.log")
    for name in schemes:
        if not tput[name] or not migr[name]:
            raise ValueError(f"missing congestion throughput or migration samples: {run}/{name}")
    if not lat or not mtcolloid_lat:
        raise ValueError(f"missing congestion latency samples in {run}")

    # ── write data file ──
    lines = [f"# Congestion-control time series, combo={COMBO} (W1={W1_BENCH}), {run}",
             "# Both workloads start on the local tier; the high-BW stride",
             "# workload saturates it. Schemes: colloid, mtcolloid (Memtis+Colloid),",
             "# libtiermem. Throughput = W1 GB/s; migration = cumulative pages moved.",
             "#",
             "## w1-throughput  (scheme: t,GBps;t,GBps;...)"]
    for name in schemes:
        lines.append(f"{name}: {series_to_str(tput[name])}")
    lines.append("")
    lines.append("## migration-cumulative  (scheme: t,pages;...)")
    for name in schemes:
        lines.append(f"{name}: {series_to_str(migr[name])}")
    lines.append("")
    lines.append("## libtiermem-cc-latency  (t,L_local,L_remote,r)")
    lines.append("series: " + ";".join(f"{t:.1f},{ld:.0f},{la:.0f},{r:.3f}"
                                         for t, ld, la, r in lat))
    lines.append("")
    lines.append("## colloid-latency  (t,L_primary,L_expansion)  CHA clk, t rel. to monitor start")
    lines.append("series: " + ";".join(f"{t:.1f},{lp:.0f},{le:.0f}"
                                         for t, lp, le in colloid_lat))
    lines.append("")
    lines.append("## mtcolloid-latency  (t,L_primary,L_expansion)  CHA clk, t rel. to monitor start")
    lines.append("series: " + ";".join(f"{t:.1f},{lp:.0f},{le:.0f}"
                                         for t, lp, le in mtcolloid_lat))
    (DATA / "eval-congc.md").write_text("\n".join(lines) + "\n")
    print("wrote data/eval-congc.md")

    # ── headline totals (all runs of the plotted combo) ──
    print("\n# Headline migration/throughput totals:")
    for combo in (COMBO,):
        print(f"\n  {combo}:")
        for run in sorted(glob(str(CONGC / "run*"))):
            b = Path(run) / combo
            if not b.exists():
                continue
            for name, (d, kind) in schemes.items():
                if kind == "vvmstat":
                    if name == "mtcolloid":
                        columns = ("htmm_nr_promoted", "htmm_nr_demoted", "htmm_nr_cong_demoted")
                    else:
                        columns = ("pgmigrate_success",)
                    s = parse_vvmstat_migration(
                        b / d / "vvmstat.mstat.csv",
                        columns,
                        align_warmup=True,
                    )
                    tot = s[-1][1] if s else 0
                else:
                    s = parse_libtiermem_migration(b / d / "stderr.log")
                    tot = s[-1][1] if s else 0
                print(f"    {name:12s} migrated_pages={tot}")


if __name__ == "__main__":
    main()
