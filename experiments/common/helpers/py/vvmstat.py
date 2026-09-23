#!/usr/bin/env python3
"""Sample selected counters from /proc/vmstat and optional cgroup memory.stat."""

from __future__ import annotations

import argparse
import json
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Dict, Iterable, List, Sequence

import pandas as pd

PROC_VMSTAT = Path("/proc/vmstat")
CGROUP_ROOT = Path("/sys/fs/cgroup")


class VmstatError(RuntimeError):
    """Raised when vmstat tracing cannot continue."""


def log_info(message: str) -> None:
    print(f"[INFO] {message}", file=sys.stderr)


def log_warn(message: str) -> None:
    print(f"[WARN] {message}", file=sys.stderr)


def log_error(message: str) -> None:
    print(f"[ERROR] {message}", file=sys.stderr)


def dedupe_preserve_order(items: Iterable[str]) -> List[str]:
    seen = set()
    result: List[str] = []
    for item in items:
        if item not in seen:
            seen.add(item)
            result.append(item)
    return result


def load_config(config_path: Path) -> tuple[List[str], List[str], List[str], List[str]]:
    """Load vmstat and cgroup stat items from a JSON config."""
    if not config_path.is_file():
        raise VmstatError(f"Config file not found: {config_path}")

    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise VmstatError(f"Invalid JSON in config file {config_path}: {exc}") from exc
    except OSError as exc:
        raise VmstatError(f"Failed to read config file {config_path}: {exc}") from exc

    vm_state_items = config.get("vm_state_items", [])
    vm_acc_items = config.get("vm_acc_items", [])
    cg_stat_items = config.get("cg_stat_items", [])
    cg_acc_items = config.get("cg_acc_items", [])

    for key, value in (
        ("vm_state_items", vm_state_items),
        ("vm_acc_items", vm_acc_items),
        ("cg_stat_items", cg_stat_items),
        ("cg_acc_items", cg_acc_items),
    ):
        if not isinstance(value, list):
            raise VmstatError(f"Config field '{key}' must be a JSON array.")
        if not all(isinstance(item, str) and item for item in value):
            raise VmstatError(f"Config field '{key}' must contain non-empty strings only.")

    vm_state_items = dedupe_preserve_order(vm_state_items)
    vm_acc_items = dedupe_preserve_order(vm_acc_items)
    cg_stat_items = dedupe_preserve_order(cg_stat_items)
    cg_acc_items = dedupe_preserve_order(cg_acc_items)

    if set(vm_state_items) & set(vm_acc_items):
        overlap = sorted(set(vm_state_items) & set(vm_acc_items))
        raise VmstatError(
            "Config items cannot appear in both vm_state_items and vm_acc_items: "
            + ", ".join(overlap)
        )

    if set(cg_stat_items) & set(cg_acc_items):
        overlap = sorted(set(cg_stat_items) & set(cg_acc_items))
        raise VmstatError(
            "Config items cannot appear in both cg_stat_items and cg_acc_items: "
            + ", ".join(overlap)
        )

    if not vm_state_items and not vm_acc_items and not cg_stat_items and not cg_acc_items:
        raise VmstatError("Config does not request any items.")

    return vm_state_items, vm_acc_items, cg_stat_items, cg_acc_items


def resolve_cgroup_stats_path(cgroup_name: str) -> Path:
    """Resolve the cgroup memory.stat path."""
    normalized_name = cgroup_name.strip()
    if not normalized_name:
        raise VmstatError("--cgroup-name must not be empty.")

    cgroup_path = PurePosixPath(normalized_name.lstrip("/"))
    if any(part in ("", ".", "..") for part in cgroup_path.parts):
        raise VmstatError(f"Invalid cgroup name: {cgroup_name!r}")

    stats_path = CGROUP_ROOT.joinpath(*cgroup_path.parts) / "memory.stat"
    if not stats_path.is_file():
        raise VmstatError(f"cgroup memory.stat not found: {stats_path}")
    return stats_path


def read_stats(stats_path: Path) -> Dict[str, int]:
    """Read a whitespace-separated key/value stats file into a dictionary."""
    try:
        lines = stats_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise VmstatError(f"Failed to read {stats_path}: {exc}") from exc

    stats: Dict[str, int] = {}
    for line in lines:
        parts = line.split()
        if len(parts) != 2:
            continue

        key, value = parts
        try:
            stats[key] = int(value)
        except ValueError as exc:
            raise VmstatError(f"Unexpected non-integer value in {stats_path}: {line!r}") from exc

    if not stats:
        raise VmstatError(f"No stats entries parsed from {stats_path}")
    return stats


def select_items(stats: Dict[str, int], items: Sequence[str], *, label: str, stats_path: Path) -> Dict[str, int]:
    missing = [item for item in items if item not in stats]
    if missing:
        raise VmstatError(f"Missing {label} item(s) in {stats_path}: {', '.join(missing)}")
    return {item: stats[item] for item in items}


def subtract_dicts(current: Dict[str, int], previous: Dict[str, int]) -> Dict[str, int]:
    """Subtract two dicts with identical keys."""
    missing = sorted(set(current) ^ set(previous))
    if missing:
        raise VmstatError(f"Mismatched stat keys while subtracting counters: {', '.join(missing)}")
    return {key: current[key] - previous[key] for key in current}


def cg_column_name(item: str) -> str:
    return f"cg_{item}"


def rename_cg_columns_for_display(df: pd.DataFrame, items: Sequence[str]) -> pd.DataFrame:
    columns = [cg_column_name(item) for item in items]
    return df[columns].rename(columns={cg_column_name(item): item for item in items})


def label_cg_items(items: Dict[str, int]) -> Dict[str, int]:
    return {cg_column_name(key): value for key, value in items.items()}


@dataclass(frozen=True)
class TraceConfig:
    config_path: Path
    output_prefix: Path
    interval: float
    count: int | None
    cgroup_name: str | None


class VmstatTracer:
    """Collect vmstat samples and write reports on exit."""

    def __init__(self, trace_config: TraceConfig):
        self.trace_config = trace_config
        (
            self.vm_state_items,
            self.vm_acc_items,
            self.cg_stat_items,
            self.cg_acc_items,
        ) = load_config(trace_config.config_path)
        if (self.cg_stat_items or self.cg_acc_items) and trace_config.cgroup_name is None:
            raise VmstatError("Config requests cg_* items but --cgroup-name was not provided.")

        self.cg_stats_path = (
            resolve_cgroup_stats_path(trace_config.cgroup_name)
            if trace_config.cgroup_name is not None
            else None
        )
        self.columns = (
            self.vm_state_items
            + self.vm_acc_items
            + [cg_column_name(item) for item in self.cg_stat_items]
            + [cg_column_name(item) for item in self.cg_acc_items]
        )
        self.samples: List[Dict[str, int | float]] = []
        self.summary_written = False
        self.stop_requested = False

        initial_stats = read_stats(PROC_VMSTAT)
        self.acc_init = select_items(
            initial_stats,
            self.vm_acc_items,
            label="vm_acc",
            stats_path=PROC_VMSTAT,
        )
        self.acc_last = dict(self.acc_init)
        if self.cg_stats_path is not None:
            initial_cg_stats = read_stats(self.cg_stats_path)
            self.cg_acc_init = select_items(
                initial_cg_stats,
                self.cg_acc_items,
                label="cg_acc",
                stats_path=self.cg_stats_path,
            )
        else:
            self.cg_acc_init = {}
        self.cg_acc_last = dict(self.cg_acc_init)
        self.start_time = time.time()

        self.trace_config.output_prefix.parent.mkdir(parents=True, exist_ok=True)

    @property
    def output_text_path(self) -> Path:
        return self.trace_config.output_prefix.with_suffix(self.trace_config.output_prefix.suffix + ".mstat")

    @property
    def output_csv_path(self) -> Path:
        return self.trace_config.output_prefix.with_suffix(self.trace_config.output_prefix.suffix + ".mstat.csv")

    def request_stop(self, reason: str) -> None:
        if not self.stop_requested:
            log_info(reason)
        self.stop_requested = True

    def sample_once(self) -> None:
        stats = read_stats(PROC_VMSTAT)
        state = select_items(stats, self.vm_state_items, label="vm_state", stats_path=PROC_VMSTAT)
        acc = select_items(stats, self.vm_acc_items, label="vm_acc", stats_path=PROC_VMSTAT)
        diff_acc = subtract_dicts(acc, self.acc_last)
        now = time.time()

        sample = {
            "timestamp": now,
            "elapsed_sec": now - self.start_time,
            **state,
            **diff_acc,
        }
        if self.cg_stats_path is not None:
            cg_stats = read_stats(self.cg_stats_path)
            cg_stat = select_items(
                cg_stats,
                self.cg_stat_items,
                label="cg_stat",
                stats_path=self.cg_stats_path,
            )
            cg_acc = select_items(
                cg_stats,
                self.cg_acc_items,
                label="cg_acc",
                stats_path=self.cg_stats_path,
            )
            diff_cg_acc = subtract_dicts(cg_acc, self.cg_acc_last)
            sample.update(label_cg_items(cg_stat))
            sample.update(label_cg_items(diff_cg_acc))
            self.cg_acc_last = cg_acc
        self.samples.append(sample)
        self.acc_last = acc

    def run(self) -> None:
        log_info(f"Config file: {self.trace_config.config_path}")
        log_info(f"Output prefix: {self.trace_config.output_prefix}")
        log_info(f"vmstat file: {PROC_VMSTAT}")
        if self.trace_config.cgroup_name is not None:
            log_info(f"Cgroup name: {self.trace_config.cgroup_name}")
            log_info(f"Cgroup stats file: {self.cg_stats_path}")
        log_info(f"Sampling interval: {self.trace_config.interval:.3f} sec")
        if self.trace_config.count is None:
            log_info("Sampling mode: until interrupted")
        else:
            log_info(f"Sampling mode: {self.trace_config.count} sample(s)")

        log_info("Start tracing. Press Ctrl-C to stop.")

        samples_taken = 0
        while not self.stop_requested:
            time.sleep(self.trace_config.interval)
            self.sample_once()
            samples_taken += 1

            if self.trace_config.count is not None and samples_taken >= self.trace_config.count:
                self.request_stop("Requested sample count reached.")

    def build_dataframe(self) -> pd.DataFrame:
        if not self.samples:
            return pd.DataFrame(columns=["timestamp", "elapsed_sec", *self.columns])
        return pd.DataFrame(self.samples)

    def write_summary(self) -> None:
        if self.summary_written:
            return

        df = self.build_dataframe()
        elapsed_seconds = max(time.time() - self.start_time, 0.0)
        sample_count = len(df)

        current_stats = read_stats(PROC_VMSTAT)
        acc_final = select_items(
            current_stats,
            self.vm_acc_items,
            label="vm_acc",
            stats_path=PROC_VMSTAT,
        )
        total_acc = subtract_dicts(acc_final, self.acc_init)
        avg_state = df[self.vm_state_items].mean().to_dict() if self.vm_state_items and not df.empty else {}
        avg_acc_per_sample = (
            {key: total_acc[key] / sample_count for key in total_acc}
            if sample_count > 0
            else {}
        )
        avg_acc_per_sec = (
            {key: total_acc[key] / elapsed_seconds for key in total_acc}
            if elapsed_seconds > 0
            else {}
        )
        if self.cg_stats_path is not None:
            current_cg_stats = read_stats(self.cg_stats_path)
            cg_acc_final = select_items(
                current_cg_stats,
                self.cg_acc_items,
                label="cg_acc",
                stats_path=self.cg_stats_path,
            )
            cg_total_acc = subtract_dicts(cg_acc_final, self.cg_acc_init)
        else:
            cg_total_acc = {}
        cg_avg_stat = (
            rename_cg_columns_for_display(df, self.cg_stat_items).mean().to_dict()
            if self.cg_stat_items and not df.empty
            else {}
        )
        cg_avg_acc_per_sample = (
            {key: cg_total_acc[key] / sample_count for key in cg_total_acc}
            if sample_count > 0
            else {}
        )
        cg_avg_acc_per_sec = (
            {key: cg_total_acc[key] / elapsed_seconds for key in cg_total_acc}
            if elapsed_seconds > 0
            else {}
        )

        lines = [
            f"config_file: {self.trace_config.config_path}",
            f"output_prefix: {self.trace_config.output_prefix}",
            f"vmstat_file: {PROC_VMSTAT}",
            f"cgroup_name: {self.trace_config.cgroup_name or ''}",
            f"cgroup_stats_file: {self.cg_stats_path or ''}",
            f"interval_sec: {self.trace_config.interval}",
            f"samples: {sample_count}",
            f"elapsed_sec: {elapsed_seconds:.3f}",
            "",
        ]

        if self.vm_state_items:
            lines.extend(
                [
                    "[vm_state_samples]",
                    df[self.vm_state_items].to_string(index=False) if not df.empty else "(no samples)",
                    "",
                    "[vm_state_avg]",
                    json.dumps(avg_state, sort_keys=True),
                    "",
                ]
            )

        if self.cg_stat_items:
            lines.extend(
                [
                    "[cg_stat_samples]",
                    (
                        rename_cg_columns_for_display(df, self.cg_stat_items).to_string(index=False)
                        if not df.empty
                        else "(no samples)"
                    ),
                    "",
                    "[cg_stat_avg]",
                    json.dumps(cg_avg_stat, sort_keys=True),
                    "",
                ]
            )

        if self.vm_acc_items:
            lines.extend(
                [
                    "[vm_acc_samples_per_interval]",
                    df[self.vm_acc_items].to_string(index=False) if not df.empty else "(no samples)",
                    "",
                    "[vm_acc_total]",
                    json.dumps(total_acc, sort_keys=True),
                    "",
                    "[vm_acc_avg_per_sample]",
                    json.dumps(avg_acc_per_sample, sort_keys=True),
                    "",
                    "[vm_acc_avg_per_sec]",
                    json.dumps(avg_acc_per_sec, sort_keys=True),
                    "",
                ]
            )

        if self.cg_acc_items:
            lines.extend(
                [
                    "[cg_acc_samples_per_interval]",
                    (
                        rename_cg_columns_for_display(df, self.cg_acc_items).to_string(index=False)
                        if not df.empty
                        else "(no samples)"
                    ),
                    "",
                    "[cg_acc_total]",
                    json.dumps(cg_total_acc, sort_keys=True),
                    "",
                    "[cg_acc_avg_per_sample]",
                    json.dumps(cg_avg_acc_per_sample, sort_keys=True),
                    "",
                    "[cg_acc_avg_per_sec]",
                    json.dumps(cg_avg_acc_per_sec, sort_keys=True),
                    "",
                ]
            )

        try:
            self.output_text_path.write_text("\n".join(lines), encoding="utf-8")
            df.to_csv(self.output_csv_path, index=False)
        except OSError as exc:
            raise VmstatError(f"Failed to write output files: {exc}") from exc

        self.summary_written = True
        log_info(f"Wrote summary to {self.output_text_path}")
        log_info(f"Wrote CSV to {self.output_csv_path}")


def parse_args(argv: Sequence[str]) -> TraceConfig:
    parser = argparse.ArgumentParser(
        description="Read selected items from /proc/vmstat and optional cgroup memory.stat, then dump summaries."
    )
    parser.add_argument("config_file", type=Path, help="JSON config file")
    parser.add_argument("output_prefix", type=Path, help="Output file prefix")
    parser.add_argument(
        "-i",
        "--interval",
        type=float,
        default=1.0,
        help="Sampling interval in seconds (default: 1.0)",
    )
    parser.add_argument(
        "-n",
        "--count",
        type=int,
        default=None,
        help="Stop after N samples instead of waiting for Ctrl-C",
    )
    parser.add_argument(
        "--cgroup-name",
        type=str,
        default=None,
        help="Also read /sys/fs/cgroup/<name>/memory.stat for configured cg_* items",
    )

    args = parser.parse_args(argv)

    if args.interval <= 0:
        parser.error("--interval must be > 0")
    if args.count is not None and args.count <= 0:
        parser.error("--count must be > 0")

    return TraceConfig(
        config_path=args.config_file,
        output_prefix=args.output_prefix,
        interval=args.interval,
        count=args.count,
        cgroup_name=args.cgroup_name,
    )


def install_signal_handlers(tracer: VmstatTracer) -> None:
    def _handle_signal(signum: int, _frame) -> None:
        signal_name = signal.Signals(signum).name
        tracer.request_stop(f"Received {signal_name}; stopping trace.")

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        trace_config = parse_args(argv if argv is not None else sys.argv[1:])
        tracer = VmstatTracer(trace_config)
        install_signal_handlers(tracer)

        try:
            tracer.run()
        finally:
            tracer.write_summary()

        return 0
    except VmstatError as exc:
        log_error(str(exc))
        return 1
    except KeyboardInterrupt:
        log_warn("Interrupted before tracer cleanup completed.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
