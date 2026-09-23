#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
spectra_load_config
spectra_require_root

out="${SPECTRA_ROOT}/results/generated/smoke"
spectra_require_empty_dir "${out}"

lib="${SPECTRA_ROOT}/src/libtiermem/libtiermem.so"
mbench="${SPECTRA_ROOT}/benchmarks/mbench/mbench"
[[ -f "${lib}" && -x "${mbench}" ]] || spectra_die "Run ./scripts/prepare.sh --build first."

export LD_PRELOAD="${lib}"
export TIERMEM_TARGET=mbench
export TIERMEM_DRAM_BUDGET=256M
export TIERMEM_OBJ_THRESHOLD=32M
export TIERMEM_EPOCH_SEC=2
export TIERMEM_WARMUP_SEC=2
export TIERMEM_MONITOR_CPU="${SPECTRA_MONITOR_CPU}"
export TIERMEM_MIGRATE_CPU_START="${SPECTRA_MIGRATE_CPU_START}"
export TIERMEM_LOG_LEVEL=2

spectra_info "running 16-second Spectra smoke experiment"
"${mbench}" \
    --benches "stride_read:4:size=256M:bufnode=${SPECTRA_SLOW_NODE}:cpus=2-5,rand_read:4:size=256M:bufnode=${SPECTRA_SLOW_NODE}:cpus=6-9" \
    --time 16 --numa-csv="${out}/numa.csv:2" \
    >"${out}/stdout.log" 2>"${out}/stderr.log"

grep -q '=== Epoch' "${out}/stderr.log" \
    || spectra_die "No completed Spectra epoch found; inspect ${out}/stderr.log"
grep -q '^time_s,bench' "${out}/numa.csv" \
    || spectra_die "NUMA placement CSV was not produced"
spectra_info "smoke PASS; output: ${out}"
