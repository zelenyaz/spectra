#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
export CCACHE_DISABLE="${CCACHE_DISABLE:-1}"

usage() {
    cat <<'EOF'
Usage:
  sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh smoke
  sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh microbench <baseline> <pair>
  sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh congestion <colloid|mtcolloid|libtiermem>
  sudo --preserve-env=SPECTRA_CONFIG ./scripts/run.sh macrobench <baseline> <pair>
  ./scripts/run.sh dry-run <microbench|macrobench> <baseline> <pair>
  ./scripts/run.sh list

Set SPECTRA_RUN_ID (default: run1) to keep repetitions separate.
Results are writable by the sudo caller (or the repository owner when run as root).
Set SPECTRA_RESULTS_UID and SPECTRA_RESULTS_GID to override the result owner.
EOF
}

finish_results() {
    local rc=$?
    trap - EXIT
    if ! python3 "${SCRIPT_DIR}/lib/result_permissions.py" repair "${SPECTRA_ROOT}" \
        "${SPECTRA_RESULTS_UID}" "${SPECTRA_RESULTS_GID}" "${RESULT_PERMISSION_PATHS[@]}"; then
        printf '[spectra][ERROR] Could not restore result permissions\n' >&2
        (( rc != 0 )) || rc=1
    fi
    exit "${rc}"
}

run_with_results() {
    local owner
    owner="$(python3 "${SCRIPT_DIR}/lib/result_permissions.py" owner "${SPECTRA_ROOT}")"
    export SPECTRA_RESULTS_UID="${owner%%:*}"
    export SPECTRA_RESULTS_GID="${owner##*:}"
    # Keep this shell alive so failures also hand partial results to the caller.
    trap finish_results EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    "$@"
}

export_site() {
    spectra_load_config
    export PERF_BIN ALTO_SCAN_SCALE BPFTRACE_BIN
    export TIERINIT_KO MEMEATER_KO COLLOID_MON_KO MTTM_ROOT MEMTIS_ROOT
    export ONEAPI_SETVARS SPEC_DIR LLAMA_BENCH LLAMA_MODEL
    export FAISS_FLAT FAISS_BENCH_SEARCH FAISS_INDEX FAISS_QUERIES
    export DRAMHIT_BIN VHT_BIN
    export NUMA_MEM="${SPECTRA_FAST_NODE}"
    export INITIAL_BUF_NODE="${SPECTRA_SLOW_NODE}"
    export WORKLOAD_CORES="${SPECTRA_WORKLOAD_CORES}"
    export TIERMEM_MONITOR_CPU="${SPECTRA_MONITOR_CPU}"
    export TIERMEM_MIGRATE_CPU_START="${SPECTRA_MIGRATE_CPU_START}"
    export MBENCH="${SPECTRA_ROOT}/benchmarks/mbench/mbench"
}

# Map the user-facing baseline to the suite baseline and its result
# subdirectory. Static placements select the W1/W2 memory nodes.
resolve_baseline() {
    case "${baseline}" in
        static-remote) export W1_MEM_NODE=1 W2_MEM_NODE=1; results_subdir=results-static-remote ;;
        static-local)  export W1_MEM_NODE=0 W2_MEM_NODE=0; results_subdir=results-static-local ;;
        static-w1)     export W1_MEM_NODE=0 W2_MEM_NODE=1; results_subdir=results-static-only-W1-local ;;
        static-w2)     export W1_MEM_NODE=1 W2_MEM_NODE=0; results_subdir=results-static-only-W2-local ;;
        *) results_subdir=results; return ;;
    esac
    baseline=static
}

cmd="${1:-}"
case "${cmd}" in
    smoke)
        export_site
        RESULT_PERMISSION_PATHS=("${SPECTRA_ROOT}/results/generated/smoke")
        run_with_results "${SCRIPT_DIR}/smoke.sh"
        ;;
    microbench)
        [[ $# -eq 3 ]] || { usage >&2; exit 2; }
        export_site
        spectra_require_root
        run_id="${SPECTRA_RUN_ID:-run1}"
        baseline="$2"
        resolve_baseline
        export RESULTS_ROOT="${SPECTRA_ROOT}/results/generated/raw/microbench/${results_subdir}/${run_id}"
        export TMP_CONFIG_DIR="${SPECTRA_ROOT}/results/generated/configs/${run_id}/microbench"
        RESULT_PERMISSION_PATHS=("${RESULTS_ROOT}/$3/${baseline}" "${TMP_CONFIG_DIR}")
        run_with_results "${SPECTRA_ROOT}/experiments/microbench/run-suite.sh" "${baseline}" "$3"
        ;;
    congestion)
        [[ $# -eq 2 ]] || { usage >&2; exit 2; }
        case "$2" in colloid|mtcolloid|libtiermem) ;; *) spectra_die "congestion baseline must be colloid, mtcolloid, or libtiermem" ;; esac
        export_site
        spectra_require_root
        run_id="${SPECTRA_RUN_ID:-run1}"
        export RESULTS_ROOT="${SPECTRA_ROOT}/results/generated/raw/congestion/${run_id}"
        export TMP_CONFIG_DIR="${SPECTRA_ROOT}/results/generated/configs/${run_id}/congestion"
        export DRAM_FREE_MIB=5600
        export TIERMEM_CONGEST_CTRL=1
        export TIERMEM_EPOCH_SEC=1
        export TIERMEM_CC_DELTA_IN=0.05
        export TIERMEM_CC_DELTA_OUT=0.05
        export TIERMEM_CC_KP=0.5
        export TIERMEM_CC_DR_MAX=0.05
        export TIERMEM_CC_R0=0.1
        export TIERMEM_CC_INTERLEAVE_G=128
        export TIERMEM_CC_LAT_EWMA=0.7
        export TIERMEM_NO_THREAD_PAUSE=1
        RESULT_PERMISSION_PATHS=("${RESULTS_ROOT}/sdprw-rrw/$2" "${TMP_CONFIG_DIR}")
        run_with_results "${SPECTRA_ROOT}/experiments/microbench/run-suite.sh" "$2" sdprw-rrw
        ;;
    macrobench)
        [[ $# -eq 3 ]] || { usage >&2; exit 2; }
        export_site
        spectra_require_root
        run_id="${SPECTRA_RUN_ID:-run1}"
        baseline="$2"
        resolve_baseline
        export RESULTS_ROOT="${SPECTRA_ROOT}/results/generated/raw/macrobench/${results_subdir}/${run_id}"
        export TMP_CONFIG_DIR="${SPECTRA_ROOT}/results/generated/configs/${run_id}/macrobench"
        RESULT_PERMISSION_PATHS=("${RESULTS_ROOT}/$3/${baseline}" "${TMP_CONFIG_DIR}")
        run_with_results "${SPECTRA_ROOT}/experiments/macrobench/run-suite.sh" "${baseline}" "$3"
        ;;
    dry-run)
        [[ $# -eq 4 ]] || { usage >&2; exit 2; }
        export_site
        baseline="$3"
        resolve_baseline
        export RESULTS_ROOT="${SPECTRA_ROOT}/results/generated/dry-run/$2/${results_subdir}"
        export TMP_CONFIG_DIR="${SPECTRA_ROOT}/results/generated/dry-run/configs/$2/${results_subdir}"
        RESULT_PERMISSION_PATHS=("${RESULTS_ROOT}/$4/${baseline}" "${TMP_CONFIG_DIR}")
        case "$2" in
            microbench)
                run_with_results "${SPECTRA_ROOT}/experiments/microbench/run-suite.sh" --dry-run "${baseline}" "$4"
                ;;
            macrobench)
                export SPECTRA_DRY_RUN=1
                run_with_results "${SPECTRA_ROOT}/experiments/macrobench/run-suite.sh" "${baseline}" "$4"
                ;;
            *) spectra_die "dry-run suite must be microbench or macrobench" ;;
        esac
        ;;
    list)
        printf 'Microbenchmark pairs:\n'
        "${SPECTRA_ROOT}/experiments/microbench/run-suite.sh" --list-combos
        cat <<'EOF'

Baselines and required kernels:
  static-{remote,local,w1,w2}, tpp, colloid, alto, libtiermem:
                                          6.3.0-colloid-alto
  memtis, mtcolloid:                     5.15.19-htmm
  mttm:                                  5.15.145-mttm

Macrobenchmark pairs:
  faissbench-spec607
  llama-faissflat
  spec619-llama
  vht-dramhit
  vht-faissbench
EOF
        ;;
    -h|--help|help|'') usage ;;
    *) spectra_die "Unknown command: ${cmd}. Run ./scripts/run.sh --help." ;;
esac
