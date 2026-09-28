#!/usr/bin/env python3
"""Reboot-resilient driver for the complete Spectra experiment matrix."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from lib.result_permissions import repair_results, results_owner


ROOT = Path(__file__).resolve().parent.parent
KERNEL_SWITCHER = ROOT / "experiments/common/kernel_version_switcher.sh"
STATE_DIR = Path("/var/lib/spectra-all-runner")
STATE_FILE = STATE_DIR / "state.json"
UNIT_NAME = "spectra-all-runner.service"
UNIT_FILE = Path("/etc/systemd/system") / UNIT_NAME
KERNELS = ("6.3.0-colloid-alto", "5.15.19-htmm", "5.15.145-mttm")
MICRO = ("sdrd-chksdrd", "sdprd-rr", "sdrdpf-rr", "sdrd-sdrw", "sdrd-sdwr")
MACRO = ("faissbench-spec607", "llama-faissflat", "spec619-llama",
         "vht-dramhit", "vht-faissbench")
BASELINES = (
    ("static-local", "static-remote", "static-w1", "static-w2", "tpp", "colloid", "alto", "libtiermem"),
    ("memtis",),
    ("mttm",),
)
CONGESTION = (("colloid", "libtiermem"), ("mtcolloid",), ())


def require_root():
    if os.geteuid() != 0:
        raise RuntimeError("This command requires root")


def run_command(args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    with tmp.open("w") as out:
        json.dump(value, out, indent=2)
        out.write("\n")
        out.flush()
        os.fsync(out.fileno())
    os.replace(tmp, path)


def load_state():
    if not STATE_FILE.exists():
        raise RuntimeError(f"No campaign state at {STATE_FILE}; run start first")
    return json.loads(STATE_FILE.read_text())


def result_env(state=None):
    env = os.environ.copy()
    if state is not None and "results_uid" in state and "results_gid" in state:
        uid, gid = state["results_uid"], state["results_gid"]
    else:
        uid, gid = results_owner(ROOT)
    env["SPECTRA_RESULTS_UID"] = str(uid)
    env["SPECTRA_RESULTS_GID"] = str(gid)
    return env


def make_plan(runs, run_start):
    cells = []
    for group, kernel in enumerate(KERNELS):
        for suite, baselines, pairs in (
            ("microbench", BASELINES[group], MICRO),
            ("congestion", CONGESTION[group], ("sdprw-rrw",)),
            ("macrobench", BASELINES[group], MACRO),
        ):
            for baseline in baselines:
                for pair in pairs:
                    for run_no in range(run_start, run_start + runs):
                        cells.append({"kernel": kernel, "suite": suite,
                                      "baseline": baseline, "pair": pair,
                                      "run_id": f"run{run_no}"})
    return cells


def cell_key(cell):
    return "-".join((cell["suite"], cell["baseline"], cell["pair"], cell["run_id"]))


def result_dir(cell):
    suite, baseline, pair, run_id = (cell[k] for k in ("suite", "baseline", "pair", "run_id"))
    if suite == "congestion":
        base = ROOT / "results/generated/raw/congestion" / run_id
    else:
        static = {"static-local": "results-static-local", "static-remote": "results-static-remote",
                  "static-w1": "results-static-only-W1-local",
                  "static-w2": "results-static-only-W2-local"}
        base = ROOT / "results/generated/raw" / suite / static.get(baseline, "results") / run_id
    return base / pair / ("static" if baseline.startswith("static-") else baseline)


def nonempty(path):
    return path.exists() and (not path.is_dir() or any(path.iterdir()))


def preflight(cells, config, env=None):
    if not config.is_file():
        raise RuntimeError(f"Site config is missing: {config}")
    if not KERNEL_SWITCHER.is_file() or not os.access(KERNEL_SWITCHER, os.X_OK):
        raise RuntimeError(f"Kernel switcher is missing or not executable: {KERNEL_SWITCHER}")
    for kernel in KERNELS:
        if not (Path("/boot") / f"vmlinuz-{kernel}").is_file():
            raise RuntimeError(f"Kernel image is missing: /boot/vmlinuz-{kernel}")
    collisions = [str(result_dir(cell)) for cell in cells if nonempty(result_dir(cell))]
    if collisions:
        raise RuntimeError(f"Existing result directory: {collisions[0]}; use --run-start to select fresh runs")
    env = result_env() if env is None else env.copy()
    env["SPECTRA_CONFIG"] = str(config)
    with (STATE_DIR / "preflight.log").open("w") as log:
        for pair in MICRO:
            run_command([str(ROOT / "scripts/run.sh"), "dry-run", "microbench", "libtiermem", pair],
                        env=env, stdout=log, stderr=subprocess.STDOUT)
        for pair in MACRO:
            run_command([str(ROOT / "scripts/run.sh"), "dry-run", "macrobench", "libtiermem", pair],
                        env=env, stdout=log, stderr=subprocess.STDOUT)


def install(_args):
    require_root()
    if any(ch.isspace() for ch in str(ROOT)):
        raise RuntimeError("Repository path must not contain whitespace for the systemd unit")
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    UNIT_FILE.write_text(f"""[Unit]
Description=Spectra complete experiment runner
After=multi-user.target
ConditionPathExists={STATE_FILE}

[Service]
Type=oneshot
User=root
WorkingDirectory={ROOT}
ExecStart=/usr/bin/python3 {ROOT}/scripts/run-all.py run
StandardOutput=append:{STATE_DIR}/driver.log
StandardError=append:{STATE_DIR}/driver.log
TimeoutStartSec=infinity

[Install]
WantedBy=multi-user.target
""")
    run_command(["systemctl", "daemon-reload"])
    run_command(["systemctl", "enable", UNIT_NAME])
    print(f"Installed {UNIT_NAME}; state: {STATE_DIR}")


def start(args):
    require_root()
    if not UNIT_FILE.exists():
        raise RuntimeError("Install the service first: sudo ./scripts/run-all.py install")
    if STATE_FILE.exists():
        raise RuntimeError("Campaign state already exists; use status/resume or reset after review")
    cells = make_plan(args.runs, args.run_start)
    config = Path(args.config).expanduser().resolve()
    uid, gid = results_owner(ROOT)
    owner_state = {"results_uid": uid, "results_gid": gid}
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    preflight(cells, config, result_env(owner_state))
    state = {"active": True, "status": "running", "index": 0, "attempt": 0,
             **owner_state,
             "pending_kernel": None, "runs": args.runs, "run_start": args.run_start,
             "validated_kernels": [],
             "sleep": args.sleep, "boot_delay": args.boot_delay,
             "max_attempts": args.max_attempts, "config": str(config),
             "cells": cells, "last_error": None}
    atomic_json(STATE_FILE, state)
    run_command(["systemctl", "enable", UNIT_NAME], stdout=subprocess.DEVNULL)
    run_command(["systemctl", "start", "--no-block", UNIT_NAME])
    print(f"Started {len(cells)} cells across {len(KERNELS)} kernels")


def archive_partial(cell, attempt, owner=None):
    source = result_dir(cell)
    configs = ROOT / "results/generated/configs" / cell["run_id"] / cell["suite"]
    owner = owner if owner is not None else results_owner(ROOT)
    if not nonempty(source):
        repair_results(ROOT, [source, configs], owner)
        return
    target = ROOT / "results/generated/failed/all-in-one" / cell_key(cell) / f"attempt{attempt}"
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        raise RuntimeError(f"Failure archive already exists: {target}")
    source.rename(target)
    # A power loss may have skipped run.sh's cleanup. Also repair the original
    # parent directories and configs, even when this was the final attempt.
    repair_results(ROOT, [target, source, configs], owner)
    print(f"Archived partial result: {target}", flush=True)


def fail(state, message):
    state["status"] = "failed"
    state["active"] = False
    state["last_error"] = message
    atomic_json(STATE_FILE, state)
    raise RuntimeError(message)


def result_is_valid(cell):
    """Require successful workloads and evidence that Spectra actually ran."""
    output = result_dir(cell)
    paths = ([output / "duration.txt"] if cell["suite"] in ("microbench", "congestion") and
             cell["baseline"] == "libtiermem" else
             [output / "w1/duration.txt", output / "w2/duration.txt"])
    for path in paths:
        if not path.is_file():
            return False
        values = dict(line.strip().split("=", 1) for line in path.read_text().splitlines() if "=" in line)
        if values.get("exit_code") != "0":
            return False
    if cell["baseline"] == "libtiermem":
        logs = []
        for path in paths:
            stderr = path.with_name("stderr.log")
            if not stderr.is_file():
                return False
            log = stderr.read_text(errors="replace")
            if ("Initialization completed, PID=" not in log or
                    "init failed" in log or "initialization failed" in log):
                return False
            logs.append(log)
        # In multi-process mode only the monitor owner emits epoch records.
        if not any("=== Epoch" in log for log in logs):
            return False
        if cell["suite"] == "congestion" and not any("congest_ctrl enabled:" in log for log in logs):
            return False
    return True


def drive(_args):
    require_root()
    with (STATE_DIR / "lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("Runner is already active") from exc
        state = load_state()
        if not state["active"]:
            print(f"Campaign is {state['status']}; nothing to run")
            return
        # Older campaigns did not record the caller; use the repository owner
        # when the boot service has no sudo environment, then persist it.
        if "results_uid" not in state or "results_gid" not in state:
            state["results_uid"], state["results_gid"] = results_owner(ROOT)
            atomic_json(STATE_FILE, state)
        owner = (state["results_uid"], state["results_gid"])
        cells = state["cells"]
        while state["index"] < len(cells):
            cell = cells[state["index"]]
            key = cell_key(cell)
            current_kernel = os.uname().release
            if current_kernel != cell["kernel"]:
                if state["pending_kernel"] == cell["kernel"]:
                    fail(state, f"Booted {current_kernel}, expected {cell['kernel']} after reboot")
                state["pending_kernel"] = cell["kernel"]
                atomic_json(STATE_FILE, state)
                print(f"Switching kernel {current_kernel} -> {cell['kernel']}", flush=True)
                try:
                    run_command([str(KERNEL_SWITCHER), "-s", cell["kernel"]])
                    run_command(["sync"])
                    time.sleep(state["boot_delay"])
                    run_command(["systemctl", "reboot"])
                except subprocess.CalledProcessError as exc:
                    fail(state, f"Kernel switch/reboot failed: {exc}")
                return
            if state["pending_kernel"]:
                state["pending_kernel"] = None
                atomic_json(STATE_FILE, state)
            if cell["kernel"] not in state["validated_kernels"]:
                env = result_env(state)
                env["SPECTRA_CONFIG"] = state["config"]
                check_log = STATE_DIR / f"preflight-{cell['kernel']}.log"
                with check_log.open("w") as log:
                    result = subprocess.run([str(ROOT / "scripts/prepare.sh"), "--check"],
                                            env=env, stdout=log, stderr=subprocess.STDOUT,
                                            check=False)
                if result.returncode:
                    fail(state, f"Kernel preflight failed; see {check_log}")
                state["validated_kernels"].append(cell["kernel"])
                atomic_json(STATE_FILE, state)
            # A service interrupted mid-cell has an uncommitted attempt.
            if state["attempt"]:
                archive_partial(cell, state["attempt"], owner)
                if state["attempt"] >= state["max_attempts"]:
                    fail(state, f"{key} exhausted {state['max_attempts']} attempts")
            state["attempt"] += 1
            atomic_json(STATE_FILE, state)
            attempt = state["attempt"]
            log_path = STATE_DIR / "logs" / f"{key}-attempt{attempt}.log"
            log_path.parent.mkdir(parents=True, exist_ok=True)
            env = result_env(state)
            env["SPECTRA_CONFIG"] = state["config"]
            env["SPECTRA_RUN_ID"] = cell["run_id"]
            command = [str(ROOT / "scripts/run.sh"), cell["suite"], cell["baseline"]]
            if cell["suite"] != "congestion":
                command.append(cell["pair"])
            print(f"[{state['index'] + 1}/{len(cells)}] attempt {attempt}: {key}", flush=True)
            with log_path.open("w") as log:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        check=False)
            if result.returncode or not result_is_valid(cell):
                print(f"Failed (runner rc={result.returncode}, output valid={result_is_valid(cell)}); "
                      f"see {log_path}", flush=True)
                if attempt >= state["max_attempts"]:
                    archive_partial(cell, attempt, owner)
                    fail(state, f"{key} failed after {attempt} attempts; see {log_path}")
                continue
            state["index"] += 1
            state["attempt"] = 0
            state["last_error"] = None
            atomic_json(STATE_FILE, state)
            if state["index"] < len(cells) and state["sleep"]:
                time.sleep(state["sleep"])
        state["active"] = False
        state["status"] = "complete"
        atomic_json(STATE_FILE, state)
        subprocess.run(["systemctl", "disable", UNIT_NAME], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"Complete: {len(cells)} cells", flush=True)


def status(_args):
    state = load_state()
    print(f"status={state['status']} completed={state['index']}/{len(state['cells'])} "
          f"runs={state['runs']} kernel={os.uname().release}")
    if state["index"] < len(state["cells"]):
        print(f"next={cell_key(state['cells'][state['index']])} attempt={state['attempt']}")
    if state["last_error"]:
        print(f"error={state['last_error']}")
    print(f"log={STATE_DIR / 'driver.log'}")


def stop(_args):
    require_root()
    run_command(["systemctl", "stop", UNIT_NAME])
    state = load_state()
    state["active"] = False
    state["status"] = "paused"
    atomic_json(STATE_FILE, state)


def resume(_args):
    require_root()
    state = load_state()
    if state["status"] == "complete":
        raise RuntimeError("Campaign is complete")
    state["active"] = True
    state["status"] = "running"
    if state["attempt"] >= state["max_attempts"]:
        state["max_attempts"] += 3
    atomic_json(STATE_FILE, state)
    run_command(["systemctl", "start", "--no-block", UNIT_NAME])


def reset(_args):
    require_root()
    state = load_state()
    if state["active"]:
        raise RuntimeError("Stop the active campaign first")
    target = STATE_DIR / f"state-{int(time.time())}.json"
    STATE_FILE.rename(target)
    print(f"Archived campaign state: {target}")


def uninstall(_args):
    require_root()
    run_command(["systemctl", "disable", "--now", UNIT_NAME])
    UNIT_FILE.unlink(missing_ok=True)
    run_command(["systemctl", "daemon-reload"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    for name in ("install", "run", "status", "stop", "resume", "reset", "uninstall"):
        subs.add_parser(name)
    for name in ("plan", "start"):
        p = subs.add_parser(name)
        p.add_argument("--runs", type=int, default=3, help="repetitions per cell (default: 3)")
        p.add_argument("--run-start", type=int, default=1, help="first run number (default: 1)")
        if name == "start":
            p.add_argument("--config", default=os.environ.get("SPECTRA_CONFIG", str(ROOT / "configs/site.conf")))
            p.add_argument("--sleep", type=int, default=10, help="seconds between cells (default: 10)")
            p.add_argument("--boot-delay", type=int, default=10, help="seconds before reboot")
            p.add_argument("--max-attempts", type=int, default=3)
    args = parser.parse_args()
    try:
        if args.command in ("plan", "start"):
            for attr in ("runs", "run_start"):
                if getattr(args, attr) < 1:
                    raise RuntimeError(f"--{attr.replace('_', '-')} must be positive")
        if args.command == "start":
            if min(args.sleep, args.boot_delay) < 0 or args.max_attempts < 1:
                raise RuntimeError("sleep and boot-delay must be nonnegative; max-attempts must be positive")
        if args.command == "plan":
            cells = make_plan(args.runs, args.run_start)
            for kernel in KERNELS:
                group = [c for c in cells if c["kernel"] == kernel]
                print(f"{kernel}: {len(group)} cells")
                for suite in ("microbench", "congestion", "macrobench"):
                    print(f"  {suite}: {sum(c['suite'] == suite for c in group)}")
            print(f"total: {len(cells)} cells")
        else:
            {"install": install, "start": start, "run": drive, "status": status,
             "stop": stop, "resume": resume, "reset": reset, "uninstall": uninstall}[args.command](args)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"[all-in-one] {exc}\n")


if __name__ == "__main__":
    main()
