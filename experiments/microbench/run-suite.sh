#!/usr/bin/env bash
#
# Run the mbench microbenchmark matrix against tiering baselines.
#
# Execution model:
#   * libtiermem: one mbench process hosting both benches (--benches b1,b2),
#     driven by run-single.sh. This is the single-process
#     LD_PRELOAD configuration (one libtiermem instance managing both benches in
#     a single address space).
#   * static, tpp, colloid, alto, memtis, mtcolloid, mttm: two separate mbench processes
#     (W1=bench1, W2=bench2), driven by ../common/dual-workload/run.sh. Per-socket iMC
#     CAS bandwidth (and kswapd0 pidstat for kernel-tiering baselines) is layered
#     on top by this suite as background profilers, since run.sh does not record
#     it.
#
# Usage:
#   sudo ./run-suite.sh
#   sudo ./run-suite.sh <baseline> <combo-id>
#   ./run-suite.sh --list-combos
#   ./run-suite.sh --dry-run <baseline> <combo-id>

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
RUN_SINGLE="${SCRIPT_DIR}/run-single.sh"
DUAL_DIR="$(cd -- "${SCRIPT_DIR}/../common/dual-workload" && pwd)"
RUN_DUAL="${DUAL_DIR}/run.sh"

MBENCH="${MBENCH:-${REPO_ROOT}/benchmarks/mbench/mbench}"
RESULTS_ROOT="${RESULTS_ROOT:-${SCRIPT_DIR}/results}"
TMP_CONFIG_DIR="${TMP_CONFIG_DIR:-${SCRIPT_DIR}/.generated-configs}"

MB_TIME="${MB_TIME:-80}"
MB_NUMA_CSV_INTERVAL="${MB_NUMA_CSV_INTERVAL:-3}"
WARMUP_SEC="${WARMUP_SEC:-10}"
NUMA_MEM="${NUMA_MEM:-0}"
INITIAL_BUF_NODE="${INITIAL_BUF_NODE:-1}"
DRAM_FREE_MIB="${DRAM_FREE_MIB:-5400}"
MEMTIS_DRAM_BUDGET_MIB="${MEMTIS_DRAM_BUDGET_MIB:-4096}"
MTTM_DRAM_MIB="${MTTM_DRAM_MIB:-4096}"
MTTM_PAGE_TYPE="${MTTM_PAGE_TYPE:-basepage}"
TIERMEM_DRAM_BUDGET="${TIERMEM_DRAM_BUDGET:-4096M}"
WORKLOAD_CORES="${WORKLOAD_CORES:-0-27}"

# Per-baseline runtime / core-placement overrides for the two-process baselines.
STATIC_RUNTIME="${STATIC_RUNTIME:-30}"
MTTM_RUNTIME="${MTTM_RUNTIME:-240}"
MTTM_CORES="${MTTM_CORES:-28-55}"

# Per-workload memory node for the static baseline (overridable via env). The
# four static variants (local/remote/only-W1-local/only-W2-local) are selected
# by the caller through these.
W1_MEM_NODE="${W1_MEM_NODE:-1}"
W2_MEM_NODE="${W2_MEM_NODE:-1}"

TIERMEM_MONITOR_CPU="${TIERMEM_MONITOR_CPU:-1}"
TIERMEM_MIGRATE_CPU_START="${TIERMEM_MIGRATE_CPU_START:-48}"
TIERMEM_OBJ_THRESHOLD="${TIERMEM_OBJ_THRESHOLD:-1G}"
TIERMEM_EPOCH_SEC="${TIERMEM_EPOCH_SEC:-5}"
TIERMEM_LOG_LEVEL="${TIERMEM_LOG_LEVEL:-3}"
TIERMEM_PEBS_PERIOD="${TIERMEM_PEBS_PERIOD:-199}"
TIERMEM_STORE_PERIOD="${TIERMEM_STORE_PERIOD:-10007}"
TIERMEM_ALLLOAD_PERIOD="${TIERMEM_ALLLOAD_PERIOD:-10007}"
TIERMEM_REG_THETA="${TIERMEM_REG_THETA:-0.2}"
TIERMEM_PF_REG_GAMMA="${TIERMEM_PF_REG_GAMMA:-1}"
TIERMEM_PF_SW_KAPPA="${TIERMEM_PF_SW_KAPPA:-0}"
TIERMEM_CONGEST_CTRL="${TIERMEM_CONGEST_CTRL:-0}"

# id|bench1|bench2  (each bench spec already carries name:threads:opts:size:bufnode)
COMBOS=(
    "sdrd-chksdrd|stride_read:12:ssz=8:size=4G:bufnode=1|chk_stride_read:12:ssz=8:x=1M:y=10:size=4G:bufnode=1"
    "sdprd-rr|stridep_read:16:np=6:size=4G:bufnode=1|rand_read:4:size=4G:bufnode=1"
    "sdrdpf-rr|stride_pf_read:12:ssz=8:pd=32:size=4G:bufnode=1|rand_read:8:size=4G:bufnode=1"
    "sdrd-sdrw|stride_read:12:ssz=8:size=4G:bufnode=1|stride_rw:12:ssz=8:size=4G:bufnode=1"
    "sdrd-sdwr|stride_read:8:ssz=8:size=4G:bufnode=1|stride_wr:16:ssz=8:size=4G:bufnode=1"

    # for congestion control
    "sdprw-rrw|stridep_rw:20:np=6:size=4G:bufnode=0|rand_rw:4:size=4G:bufnode=1"
)

BASELINES=(static tpp colloid alto libtiermem memtis mtcolloid mttm)

log() {
    printf '[microbench] %s\n' "$*"
}

warn() {
    printf '[microbench][WARN] %s\n' "$*" >&2
}

die() {
    printf '[microbench][FATAL] %s\n' "$*" >&2
    exit 1
}

combo_summary() {
    local entry combo bench1 bench2
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r combo bench1 bench2 <<< "${entry}"
        printf '%-13s %s,%s\n' "${combo}" "${bench1}" "${bench2}"
    done
}

lookup_combo() {
    local target="$1"
    local entry combo bench1 bench2
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r combo bench1 bench2 <<< "${entry}"
        if [[ "${combo}" == "${target}" ]]; then
            printf '%s|%s\n' "${bench1}" "${bench2}"
            return 0
        fi
    done
    die "Unknown combo id: ${target}"
}

is_known_baseline() {
    local target="$1"
    local baseline
    for baseline in "${BASELINES[@]}"; do
        [[ "${baseline}" == "${target}" ]] && return 0
    done
    return 1
}

baseline_is_libtiermem() {
    [[ "$1" == "libtiermem" ]]
}

render_bash_array() {
    local -n array_ref="$1"
    local rendered="("
    local arg
    for arg in "${array_ref[@]}"; do
        rendered+="$(printf ' %q' "${arg}")"
    done
    rendered+=" )"
    printf '%s' "${rendered}"
}

# Drop any 'bufnode=<value>' token from a colon-separated mbench bench spec.
strip_bufnode() {
    local spec="$1"
    local out="" tok
    local -a toks
    IFS=':' read -r -a toks <<< "${spec}"
    for tok in "${toks[@]}"; do
        [[ "${tok}" == bufnode=* ]] && continue
        out+="${out:+:}${tok}"
    done
    printf '%s' "${out}"
}

# ── libtiermem: single mbench process hosting both benches ──────────────
write_libtiermem_config() {
    local config="$1"
    local combo="$2"
    local outdir="$3"
    local bench1="$4"
    local bench2="$5"

    local benches="${bench1},${bench2}"
    # shellcheck disable=SC2034 # Consumed by render_bash_array through a nameref.
    local -a workload_cmd=(
        "${MBENCH}"
        --benches "${benches}"
        --time "${MB_TIME}"
        "--numa-csv=${outdir}/numa.csv:${MB_NUMA_CSV_INTERVAL}"
    )
    local workload_cmd_literal
    workload_cmd_literal="$(render_bash_array workload_cmd)"

    mkdir -p "$(dirname -- "${config}")"
    cat > "${config}" <<EOF
#!/usr/bin/env bash
# Auto-generated by experiments/microbench/run-suite.sh. Do not hand-edit.

BASELINE="libtiermem"
WORKLOAD_NAME="${combo}"
WORKLOAD_CMD=${workload_cmd_literal}
WORKLOAD_CORES="${WORKLOAD_CORES}"
WORKLOAD_CORES_AFTER=""
WORKLOAD_TARGET_EXE="$(basename -- "${MBENCH}")"

OUTDIR="${outdir}"
TURBO="on"
WARMUP_SEC=${WARMUP_SEC}
NUMA_MEM=${NUMA_MEM}
DRAM_FREE_MIB=""

LIBTIERMEM_PATH="${REPO_ROOT}/src/libtiermem/libtiermem.so"
TIERMEM_DRAM_BUDGET="${TIERMEM_DRAM_BUDGET}"
TIERMEM_MONITOR_CPU=${TIERMEM_MONITOR_CPU}
TIERMEM_MIGRATE_CPU_START=${TIERMEM_MIGRATE_CPU_START}
TIERMEM_OBJ_THRESHOLD="${TIERMEM_OBJ_THRESHOLD}"
TIERMEM_EPOCH_SEC="${TIERMEM_EPOCH_SEC}"
TIERMEM_LOG_LEVEL="${TIERMEM_LOG_LEVEL}"
TIERMEM_PEBS_PERIOD="${TIERMEM_PEBS_PERIOD}"
TIERMEM_STORE_PERIOD="${TIERMEM_STORE_PERIOD}"
TIERMEM_ALLLOAD_PERIOD="${TIERMEM_ALLLOAD_PERIOD}"
TIERMEM_REG_THETA="${TIERMEM_REG_THETA}"
TIERMEM_PF_REG_GAMMA="${TIERMEM_PF_REG_GAMMA}"
TIERMEM_PF_SW_KAPPA="${TIERMEM_PF_SW_KAPPA}"
TIERMEM_CONGEST_CTRL="${TIERMEM_CONGEST_CTRL}"

VVMSTAT_INTERVAL=1
NUMA_MAPS_INTERVAL=2
ENABLE_VVMSTAT=0
ENABLE_NUMA_MAPS=0
ENABLE_IMC_CAS=0
IMC_CAS_INTERVAL=1000
EOF
}

# ── static/tpp/colloid/alto/memtis/mttm: two mbench processes via run.sh ──
write_dual_config() {
    local config="$1"
    local baseline="$2"
    local combo="$3"
    local outdir="$4"
    local bench1="$5"
    local bench2="$6"

    local runtime="${MB_TIME}"
    local w1_cores="${WORKLOAD_CORES}" w2_cores="${WORKLOAD_CORES}"
    local w1_cores_after="" w2_cores_after=""
    local dram_free=""
    local enable_vvmstat=1
    local enable_numa_maps=0
    local static_mem_block="" mttm_block=""
    local spec1="${bench1}" spec2="${bench2}"

    case "${baseline}" in
        static)
            # Memory placement comes from numactl -m (run.sh applies W*_MEM_NODE
            # for the static baseline); drop bufnode so it does not conflict.
            runtime="${STATIC_RUNTIME}"
            enable_vvmstat=0
            spec1="$(strip_bufnode "${bench1}")"
            spec2="$(strip_bufnode "${bench2}")"
            static_mem_block="W1_MEM_NODE=${W1_MEM_NODE}
W2_MEM_NODE=${W2_MEM_NODE}"
            ;;
        tpp)
            dram_free="${DRAM_FREE_MIB}"
            ;;
        colloid|alto)
            dram_free="${DRAM_FREE_MIB}"
            ;;
        memtis|mtcolloid)
            # mtcolloid is memtis + colloid-mon congestion demotion; same DRAM
            # budget and dual-process layout as plain memtis. run.sh handles the
            # colloid-mon load/unload from BASELINE="mtcolloid".
            dram_free="${MEMTIS_DRAM_BUDGET_MIB}"
            ;;
        mttm)
            runtime="${MTTM_RUNTIME}"
            w1_cores="${MTTM_CORES}"; w2_cores="${MTTM_CORES}"
            w1_cores_after="${WORKLOAD_CORES}"; w2_cores_after="${WORKLOAD_CORES}"
            dram_free="${MTTM_DRAM_MIB}"
            enable_vvmstat=0
            mttm_block="MTTM_PAGE_TYPE=\"${MTTM_PAGE_TYPE}\""
            ;;
        *)
            die "write_dual_config: unsupported baseline ${baseline}"
            ;;
    esac

    local target_exe
    target_exe="$(basename -- "${MBENCH}")"
    local w1_name="${spec1%%:*}" w2_name="${spec2%%:*}"

    # shellcheck disable=SC2034 # Consumed by render_bash_array through a nameref.
    local -a w1_cmd=(
        "${MBENCH}"
        --benches "${spec1}"
        --time "${runtime}"
        "--numa-csv=${outdir}/w1/numa.csv:${MB_NUMA_CSV_INTERVAL}"
    )
    # shellcheck disable=SC2034 # Consumed by render_bash_array through a nameref.
    local -a w2_cmd=(
        "${MBENCH}"
        --benches "${spec2}"
        --time "${runtime}"
        "--numa-csv=${outdir}/w2/numa.csv:${MB_NUMA_CSV_INTERVAL}"
    )
    local w1_cmd_literal w2_cmd_literal
    w1_cmd_literal="$(render_bash_array w1_cmd)"
    w2_cmd_literal="$(render_bash_array w2_cmd)"

    mkdir -p "$(dirname -- "${config}")"
    cat > "${config}" <<EOF
#!/usr/bin/env bash
# Auto-generated by experiments/microbench/run-suite.sh. Do not hand-edit.

BASELINE="${baseline}"
WARMUP_SEC=${WARMUP_SEC}
OUTDIR="${outdir}"
TURBO="on"

NUMA_MEM=${NUMA_MEM}
DRAM_FREE_MIB="${dram_free}"

${static_mem_block}
${mttm_block}

W1_NAME="${w1_name}"
W1_CMD=${w1_cmd_literal}
W1_ENV=()
W1_CWD=""
W1_SOURCE=""
W1_CORES="${w1_cores}"
W1_CORES_AFTER="${w1_cores_after}"
W1_TARGET_EXE="${target_exe}"

W2_NAME="${w2_name}"
W2_CMD=${w2_cmd_literal}
W2_ENV=()
W2_CWD=""
W2_SOURCE=""
W2_CORES="${w2_cores}"
W2_CORES_AFTER="${w2_cores_after}"
W2_TARGET_EXE="${target_exe}"

VVMSTAT_INTERVAL=1
NUMA_MAPS_INTERVAL=2
ENABLE_VVMSTAT=${enable_vvmstat}
ENABLE_NUMA_MAPS=${enable_numa_maps}

TARGET_PID_TIMEOUT_SEC=60
EOF
}

# Background bandwidth/migration profilers layered on top of run.sh for the
# two-process baselines. iMC CAS (per socket) is the bandwidth ground truth;
# kswapd0 pidstat attributes kernel-tiering migration overhead.
SUITE_PROFILER_PIDS=()

start_suite_profilers() {
    local baseline="$1"
    local outdir="$2"
    SUITE_PROFILER_PIDS=()

    if [[ -x "${PERF_BIN:-}" ]]; then
        local imc_events="unc_m_cas_count.rd,unc_m_cas_count.wr"
        "${PERF_BIN}" stat -e "${imc_events}" -I 1000 -C 0 -o "${outdir}/imc-cas-C0.log" &
        SUITE_PROFILER_PIDS+=("$!")
        "${PERF_BIN}" stat -e "${imc_events}" -I 1000 -C 28 -o "${outdir}/imc-cas-C28.log" &
        SUITE_PROFILER_PIDS+=("$!")
        log "iMC CAS rd/wr profilers started (sockets 0 and 1)"
    else
        warn "PERF_BIN is unset or not executable; skipping iMC CAS bandwidth profiling"
    fi

    case "${baseline}" in
        tpp|colloid|alto)
            local kswapd_pid
            kswapd_pid="$(pgrep -x kswapd0 || true)"
            if [[ -z "${kswapd_pid}" ]]; then
                warn "kswapd0 not found; skipping pidstat for ${baseline}"
            elif command -v pidstat >/dev/null 2>&1; then
                pidstat -h -u -p "${kswapd_pid}" 1 > "${outdir}/kswapd0-pidstat.log" 2>&1 &
                SUITE_PROFILER_PIDS+=("$!")
                log "kswapd0 pidstat started (pid=${kswapd_pid})"
            else
                warn "pidstat not found; skipping kswapd0 tracking"
            fi
            ;;
    esac
}

stop_suite_profilers() {
    local pid
    for pid in "${SUITE_PROFILER_PIDS[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        kill "${pid}" 2>/dev/null || true
    done
    for pid in "${SUITE_PROFILER_PIDS[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        wait "${pid}" 2>/dev/null || true
    done
    SUITE_PROFILER_PIDS=()
}

run_one() {
    local dry_run="$1"
    local baseline="$2"
    local combo="$3"

    is_known_baseline "${baseline}" ||
        die "Unknown baseline: ${baseline} (valid: ${BASELINES[*]})"

    local bench_pair bench1 bench2
    bench_pair="$(lookup_combo "${combo}")"
    IFS='|' read -r bench1 bench2 <<< "${bench_pair}"

    local outdir="${RESULTS_ROOT}/${combo}/${baseline}"
    local config="${TMP_CONFIG_DIR}/${combo}-${baseline}.sh"
    if baseline_is_libtiermem "${baseline}"; then
        write_libtiermem_config "${config}" "${combo}" "${outdir}" "${bench1}" "${bench2}"
    else
        write_dual_config "${config}" "${baseline}" "${combo}" "${outdir}" "${bench1}" "${bench2}"
    fi

    if (( dry_run )); then
        cat "${config}"
        return 0
    fi

    [[ "${EUID}" -eq 0 ]] || die "Must run as root"
    [[ -x "${MBENCH}" ]] || die "mbench not found or not executable: ${MBENCH}"
    if [[ -d "${outdir}" ]] && find "${outdir}" -mindepth 1 -print -quit | grep -q .; then
        die "Refusing to overwrite nonempty result directory: ${outdir}"
    fi

    log "baseline=${baseline} combo=${combo} outdir=${outdir}"
    if baseline_is_libtiermem "${baseline}"; then
        [[ -x "${RUN_SINGLE}" ]] || die "run-single.sh not executable: ${RUN_SINGLE}"
        "${RUN_SINGLE}" "${config}"
    else
        [[ -x "${RUN_DUAL}" ]] || die "dual-workload run.sh not executable: ${RUN_DUAL}"
        mkdir -p "${outdir}"
        start_suite_profilers "${baseline}" "${outdir}"
        local rc=0
        "${RUN_DUAL}" "${config}" || rc=$?
        stop_suite_profilers
        return "${rc}"
    fi
}

run_all() {
    local entry combo _rest baseline
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r combo _rest <<< "${entry}"
        for baseline in "${BASELINES[@]}"; do
            run_one 0 "${baseline}" "${combo}"
            log "sleeping 15s between runs"
            sleep 15
        done
    done
}

case $# in
    0)
        run_all
        ;;
    1)
        [[ "$1" == "--list-combos" ]] || die "Unknown option: $1"
        combo_summary
        ;;
    2)
        run_one 0 "$1" "$2"
        ;;
    3)
        [[ "$1" == "--dry-run" ]] || die "Unknown option: $1"
        run_one 1 "$2" "$3"
        ;;
    *)
        cat >&2 <<EOF
Usage:
  sudo $0
  sudo $0 <baseline> <combo-id>
  $0 --list-combos
  $0 --dry-run <baseline> <combo-id>

Baselines: ${BASELINES[*]}
EOF
        exit 1
        ;;
esac
