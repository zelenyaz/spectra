#!/usr/bin/env bash
#
# Execute one generated one-process mbench configuration.

set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HELPERS_DIR="$(cd -- "${SCRIPT_DIR}/../common/helpers" && pwd)"
DUAL_DIR="$(cd -- "${SCRIPT_DIR}/../common/dual-workload" && pwd)"
PY_DIR="${HELPERS_DIR}/py"

ALTO_SCAN_SCALE="${ALTO_SCAN_SCALE:-}"

# shellcheck source=../common/helpers/setup-baseline.sh disable=SC1091
source "${HELPERS_DIR}/setup-baseline.sh"

die() {
    printf '[microbench][FATAL] %s\n' "$*" >&2
    exit 1
}

info() {
    printf '[microbench] %s\n' "$*"
}

warn() {
    printf '[microbench][WARN] %s\n' "$*" >&2
}

timeline() {
    printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" |
        tee -a "${OUTDIR}/timeline.log"
}

baseline_is_static() {
    [[ "${BASELINE}" == "static" ]]
}

baseline_is_libtiermem() {
    [[ "${BASELINE}" == "libtiermem" ]]
}

baseline_is_memtis() {
    [[ "${BASELINE}" == "memtis" ]]
}

baseline_is_mttm() {
    [[ "${BASELINE}" == "mttm" ]]
}

baseline_is_alto() {
    [[ "${BASELINE}" == "alto" ]]
}

baseline_uses_kernel_tiering() {
    [[ "${BASELINE}" == "tpp" || "${BASELINE}" == "colloid" || "${BASELINE}" == "alto" ]]
}

baseline_uses_dram_fill() {
    baseline_uses_kernel_tiering
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

proc_exe_basename() {
    local pid="$1"
    local exe_path
    exe_path="$(readlink -f "/proc/${pid}/exe" 2>/dev/null || true)"
    if [[ -n "${exe_path}" ]]; then
        basename -- "${exe_path}"
        return
    fi
    [[ -f "/proc/${pid}/comm" ]] || return 1
    tr -d '\n' < "/proc/${pid}/comm"
}

list_descendant_pids() {
    local root_pid="$1"
    local child_pid
    while read -r child_pid; do
        [[ -n "${child_pid}" ]] || continue
        printf '%s\n' "${child_pid}"
        list_descendant_pids "${child_pid}"
    done < <(pgrep -P "${root_pid}" 2>/dev/null || true)
}

find_workload_target_pid() {
    local root_pid="$1"
    local target_exe="$2"
    local pid exe

    exe="$(proc_exe_basename "${root_pid}" 2>/dev/null || true)"
    if [[ "${exe}" == "${target_exe}" ]]; then
        printf '%s\n' "${root_pid}"
        return
    fi

    while read -r pid; do
        [[ -n "${pid}" ]] || continue
        exe="$(proc_exe_basename "${pid}" 2>/dev/null || true)"
        if [[ "${exe}" == "${target_exe}" ]]; then
            printf '%s\n' "${pid}"
            return
        fi
    done < <(list_descendant_pids "${root_pid}")
    return 1
}

resolve_workload_profile_pid() {
    local root_pid="$1"
    local target_exe="$2"
    local deadline=$(( $(date +%s) + 60 ))
    local pid

    while true; do
        pid="$(find_workload_target_pid "${root_pid}" "${target_exe}" || true)"
        if [[ -n "${pid}" ]]; then
            printf '%s\n' "${pid}"
            return
        fi
        kill -0 "${root_pid}" 2>/dev/null ||
            die "workload exited before target executable ${target_exe} appeared"
        (( $(date +%s) < deadline )) ||
            die "timed out waiting for target executable ${target_exe}"
        sleep 0.2
    done
}

kill_workload_tree() {
    local root_pid="$1"
    local pid
    local -a descendants=()
    [[ -n "${root_pid}" ]] || return
    kill -0 "${root_pid}" 2>/dev/null || return
    while read -r pid; do
        [[ -n "${pid}" ]] && descendants+=("${pid}")
    done < <(list_descendant_pids "${root_pid}")
    for (( pid=${#descendants[@]} - 1; pid >= 0; pid-- )); do
        kill "${descendants[pid]}" 2>/dev/null || true
    done
    kill "${root_pid}" 2>/dev/null || true
}

stop_pid() {
    local pid="$1"
    [[ -n "${pid}" ]] || return
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

inject_libtiermem_env() {
    WORKLOAD_ENV+=(
        "LD_PRELOAD=${LIBTIERMEM_PATH}"
        "TIERMEM_TARGET=${WORKLOAD_TARGET_EXE}"
        "TIERMEM_DRAM_BUDGET=${TIERMEM_DRAM_BUDGET}"
        "TIERMEM_WARMUP_SEC=${WARMUP_SEC}"
        "TIERMEM_MONITOR_CPU=${TIERMEM_MONITOR_CPU}"
        "TIERMEM_MIGRATE_CPU_START=${TIERMEM_MIGRATE_CPU_START}"
        "TIERMEM_OBJ_THRESHOLD=${TIERMEM_OBJ_THRESHOLD}"
        "TIERMEM_EPOCH_SEC=${TIERMEM_EPOCH_SEC}"
        "TIERMEM_LOG_LEVEL=${TIERMEM_LOG_LEVEL}"
        "TIERMEM_PEBS_PERIOD=${TIERMEM_PEBS_PERIOD}"
        "TIERMEM_STORE_PERIOD=${TIERMEM_STORE_PERIOD}"
        "TIERMEM_ALLLOAD_PERIOD=${TIERMEM_ALLLOAD_PERIOD}"
        "TIERMEM_REG_THETA=${TIERMEM_REG_THETA}"
        "TIERMEM_PF_REG_GAMMA=${TIERMEM_PF_REG_GAMMA}"
        "TIERMEM_PF_SW_KAPPA=${TIERMEM_PF_SW_KAPPA}"
        "TIERMEM_CONGEST_CTRL=${TIERMEM_CONGEST_CTRL}"
    )
}

enable_kernel_tiering() {
    case "${BASELINE}" in
        tpp)
            baseline_enable_tpp
            ;;
        colloid|alto)
            baseline_enable_colloid
            ;;
        *)
            die "Cannot enable kernel tiering for ${BASELINE}"
            ;;
    esac
}

start_alto_scan_scale_controller() {
    PYTHONUNBUFFERED=1 python3 "${ALTO_SCAN_SCALE}" "${OUTDIR}/system.perf.log" \
        > "${OUTDIR}/system.scale.log" 2>&1 &
    PROFILER_PIDS+=("$!")
    timeline "ALTO scan-scale controller started (PID=$!)"
}

reset_alto_scan_scale() {
    local path="/proc/sys/kernel/numa_balancing_pte_scale"
    baseline_is_alto || return
    [[ -w "${path}" ]] || return
    printf '16\n' > "${path}" || true
}

start_colloid_latency_monitor() {
    # colloid_mon.ko (inserted by enable_kernel_tiering for colloid/alto) exposes
    # a moving-average access latency; sample it once a second for the rest of
    # the run. No-op for TPP, which does not load the module.
    [[ "${BASELINE}" == "colloid" || "${BASELINE}" == "alto" ]] || return 0
    local path="/sys/kernel/colloid/latency"
    if [[ ! -r "${path}" ]]; then
        warn "${path} not readable after enabling ${BASELINE}; skipping latency monitor"
        return 0
    fi
    (
        while true; do
            printf '%s %s\n' "$(date +%s.%N)" "$(cat "${path}" 2>/dev/null)"
            sleep 1
        done
    ) > "${OUTDIR}/colloid-latency.log" 2>&1 &
    PROFILER_PIDS+=("$!")
    timeline "colloid-latency monitor started (PID=$!)"
}

start_ksamplingd_pidstat() {
    # Memtis migrates pages from the ksamplingd kernel thread; track its CPU
    # usage with pidstat once memtis is enabled.
    local pid
    pid="$(pgrep -x ksamplingd 2>/dev/null | head -1 || true)"
    if [[ -z "${pid}" ]]; then
        warn "ksamplingd not running after baseline_enable_memtis; skipping pidstat"
        return 0
    fi
    pidstat -p "${pid}" 1 > "${OUTDIR}/memtis-ksamplingd.pidstat.log" 2>&1 &
    PROFILER_PIDS+=("$!")
    timeline "pidstat ksamplingd started (PID=$! ksamplingd=${pid})"
}

write_duration() {
    local start_ns="$1"
    local post_warmup_ns="$2"
    local end_ns="$3"
    local rc="$4"
    local duration post_warmup_duration

    duration="$(awk "BEGIN {printf \"%.3f\", (${end_ns} - ${start_ns}) / 1000000000}")"
    post_warmup_duration="$(awk "BEGIN {printf \"%.3f\", (${end_ns} - ${post_warmup_ns}) / 1000000000}")"
    cat > "${OUTDIR}/duration.txt" <<EOF
duration_sec=${duration}
post_warmup_duration_sec=${post_warmup_duration}
exit_code=${rc}
start_epoch_ns=${start_ns}
post_warmup_start_epoch_ns=${post_warmup_ns}
end_epoch_ns=${end_ns}
EOF
    printf '%s\n' "${duration}"
}

CONFIG_FILE="${1:-}"
[[ -n "${CONFIG_FILE}" ]] || die "Usage: sudo $0 <config-file>"
[[ -f "${CONFIG_FILE}" ]] || die "Config file not found: ${CONFIG_FILE}"

BASELINE=""
WORKLOAD_NAME=""
WORKLOAD_CMD=()
WORKLOAD_ENV=()
WORKLOAD_CORES=""
WORKLOAD_CORES_AFTER=""
WORKLOAD_TARGET_EXE=""
OUTDIR=""
TURBO="off"
WARMUP_SEC=0
NUMA_MEM=0
DRAM_FREE_MIB=""
ENABLE_VVMSTAT=1
ENABLE_NUMA_MAPS=0
ENABLE_IMC_CAS=1
VVMSTAT_INTERVAL=1
NUMA_MAPS_INTERVAL=2
IMC_CAS_INTERVAL=1000
LIBTIERMEM_PATH=""
TIERMEM_DRAM_BUDGET=""
TIERMEM_MONITOR_CPU=1
TIERMEM_MIGRATE_CPU_START=48
TIERMEM_OBJ_THRESHOLD=""
TIERMEM_EPOCH_SEC=""
TIERMEM_LOG_LEVEL=""
TIERMEM_PEBS_PERIOD=""
TIERMEM_STORE_PERIOD=""
TIERMEM_ALLLOAD_PERIOD=""
TIERMEM_REG_THETA=""
TIERMEM_PF_REG_GAMMA=""
TIERMEM_PF_SW_KAPPA=""
TIERMEM_CONGEST_CTRL="0"
MTTM_PAGE_TYPE="basepage"

# shellcheck source=/dev/null
source "${CONFIG_FILE}"

: "${BASELINE:?BASELINE not set}"
: "${WORKLOAD_NAME:?WORKLOAD_NAME not set}"
: "${WORKLOAD_CORES:?WORKLOAD_CORES not set}"
: "${WORKLOAD_TARGET_EXE:?WORKLOAD_TARGET_EXE not set}"
: "${OUTDIR:?OUTDIR not set}"
[[ ${#WORKLOAD_CMD[@]} -gt 0 ]] || die "WORKLOAD_CMD not set"

case "${BASELINE}" in
    static|tpp|colloid|alto|libtiermem|memtis|mttm) ;;
    *) die "Unsupported baseline: ${BASELINE}" ;;
esac

if baseline_is_libtiermem; then
    : "${LIBTIERMEM_PATH:?LIBTIERMEM_PATH not set}"
    : "${TIERMEM_DRAM_BUDGET:?TIERMEM_DRAM_BUDGET not set}"
    [[ -f "${LIBTIERMEM_PATH}" ]] || die "libtiermem.so not found: ${LIBTIERMEM_PATH}"
fi
if baseline_is_memtis; then
    baseline_require_memtis_kernel || die "memtis requires kernel ${MEMTIS_KERNEL_RELEASE}"
    : "${DRAM_FREE_MIB:?DRAM_FREE_MIB not set for memtis}"
fi
if baseline_is_mttm; then
    require_kernel_release "${MTTM_KERNEL_VER}" || die "mttm requires kernel ${MTTM_KERNEL_VER}"
    : "${DRAM_FREE_MIB:?DRAM_FREE_MIB not set for mttm}"
fi
[[ "${EUID}" -eq 0 ]] || die "Must run as root"
[[ -x "${WORKLOAD_CMD[0]}" ]] || die "mbench not executable: ${WORKLOAD_CMD[0]}"
require_command numactl
require_command pgrep
require_command taskset
if baseline_is_memtis; then
    require_command cgcreate
    require_command cgdelete
    require_command cgexec
    require_command pidstat
fi
if baseline_is_mttm; then
    require_command cgcreate
    require_command cgdelete
    mttm_ensure_run_bench || die "MTTM wrapper unavailable: ${MTTM_RUNNER_WRAPPER}"
fi
if baseline_is_alto; then
    [[ -x "${PERF_BIN:-}" ]] || die "PERF_BIN is unset or not executable: ${PERF_BIN:-}"
    [[ -f "${ALTO_SCAN_SCALE}" ]] || die "ALTO scan-scale controller missing: ${ALTO_SCAN_SCALE}"
fi
if baseline_uses_kernel_tiering; then
    require_command pidstat
fi
if (( ENABLE_NUMA_MAPS )); then
    [[ -f "${PY_DIR}/parse_numa_maps.py" ]] || die "parse_numa_maps.py missing"
fi
if (( ENABLE_IMC_CAS )); then
    [[ -x "${PERF_BIN:-}" ]] || die "PERF_BIN is unset or not executable: ${PERF_BIN:-}"
fi

VVMSTAT_CONFIG="${DUAL_DIR}/vvmstat-tiering.json"
baseline_is_memtis && VVMSTAT_CONFIG="${DUAL_DIR}/vvmstat-memtis.json"
if (( ENABLE_VVMSTAT )); then
    [[ -f "${VVMSTAT_CONFIG}" ]] || die "vvmstat config missing: ${VVMSTAT_CONFIG}"
fi

mkdir -p "${OUTDIR}"
cp "${CONFIG_FILE}" "${OUTDIR}/config.sh"
if (( ENABLE_VVMSTAT )); then
    cp "${VVMSTAT_CONFIG}" "${OUTDIR}/vvmstat-config.json"
fi
: > "${OUTDIR}/timeline.log"

timeline "Experiment start"
timeline "baseline=${BASELINE} workload=${WORKLOAD_NAME} cores=${WORKLOAD_CORES}"
timeline "command=${WORKLOAD_CMD[*]}"

PROFILER_PIDS=()
WORKLOAD_PID=""
PROFILE_PID=""
DRAM_FILLED=0
TIERING_ENABLED=0
MEMTIS_PREPARED=0
MTTM_PREPARED=0
CLEANUP_DONE=0

cleanup() {
    (( CLEANUP_DONE )) && return
    CLEANUP_DONE=1
    set +e

    info "Cleaning up"
    kill_workload_tree "${WORKLOAD_PID}"
    sleep 1
    local pid
    for pid in "${PROFILER_PIDS[@]}"; do
        stop_pid "${pid}"
    done
    PROFILER_PIDS=()

    reset_alto_scan_scale
    if (( MEMTIS_PREPARED )); then
        baseline_disable_memtis
        MEMTIS_PREPARED=0
    fi
    if (( MTTM_PREPARED )); then
        dmesg > "${OUTDIR}/mttm-dmesg.log" 2>/dev/null || true
        baseline_disable_mttm
        MTTM_PREPARED=0
    fi
    if (( DRAM_FILLED )); then
        freemem
        DRAM_FILLED=0
    fi
    if (( TIERING_ENABLED )); then
        disable_tiering
        TIERING_ENABLED=0
    fi
    if baseline_is_libtiermem; then
        rm -f /dev/shm/tiermem_shm 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [[ "${TURBO}" == "off" ]]; then
    printf '1\n' > /sys/devices/system/cpu/intel_pstate/no_turbo
    timeline "Turbo OFF"
else
    printf '0\n' > /sys/devices/system/cpu/intel_pstate/no_turbo
    timeline "Turbo ON"
fi
printf '%s\n' -1 > /proc/sys/kernel/perf_event_paranoid

setup_common || die "common setup failed"
timeline "Disabling tiering"
disable_tiering || die "failed to disable tiering"
sync
printf '3\n' > /proc/sys/vm/drop_caches

if baseline_uses_dram_fill; then
    timeline "Filling node ${NUMA_MEM} to ${DRAM_FREE_MIB} MiB free"
    DRAM_FILLED=1
    usemem "${NUMA_MEM}" "${DRAM_FREE_MIB}" || die "failed to fill DRAM"
fi

if baseline_is_memtis; then
    MEMTIS_PREPARED=1
    baseline_prepare_memtis || die "failed to prepare memtis"
fi
if baseline_is_mttm; then
    MTTM_PREPARED=1
    case "${MTTM_PAGE_TYPE}" in
        hugepage) baseline_prepare_mttm_hugepage || die "failed to prepare MTTM hugepage" ;;
        basepage) baseline_prepare_mttm_basepage || die "failed to prepare MTTM basepage" ;;
        *) die "invalid MTTM_PAGE_TYPE: ${MTTM_PAGE_TYPE}" ;;
    esac
    mttm_limit_local_dram_size "${DRAM_FREE_MIB}M" || die "failed to set MTTM DRAM limit"
fi
if baseline_is_libtiermem; then
    rm -f /dev/shm/tiermem_shm 2>/dev/null || true
    inject_libtiermem_env
fi

if (( ENABLE_VVMSTAT )); then
    vvmstat_args=("${OUTDIR}/vvmstat-config.json" "${OUTDIR}/vvmstat" -i "${VVMSTAT_INTERVAL}")
    if baseline_is_memtis; then
        vvmstat_args+=(--cgroup-name "${MEMTIS_CGNAME}")
    fi
    python3 "${PY_DIR}/vvmstat.py" "${vvmstat_args[@]}" &
    PROFILER_PIDS+=("$!")
    timeline "vvmstat started (PID=$!)"
fi
if (( ENABLE_IMC_CAS )); then
    # Per-socket iMC CAS counts (read + write) via perf uncore counters for the
    # full run. Core 0 selects socket 0's iMC, core 28 selects socket 1's.
    IMC_CAS_EVENTS="unc_m_cas_count.rd,unc_m_cas_count.wr"
    "${PERF_BIN}" stat -e "${IMC_CAS_EVENTS}" -I "${IMC_CAS_INTERVAL}" -C 0 \
        -o "${OUTDIR}/imc-cas-C0.log" &
    PROFILER_PIDS+=("$!")
    timeline "iMC CAS rd/wr socket 0 (core 0) started (PID=$!)"
    "${PERF_BIN}" stat -e "${IMC_CAS_EVENTS}" -I "${IMC_CAS_INTERVAL}" -C 28 \
        -o "${OUTDIR}/imc-cas-C28.log" &
    PROFILER_PIDS+=("$!")
    timeline "iMC CAS rd/wr socket 1 (core 28) started (PID=$!)"
fi
if baseline_is_alto; then
    "${PERF_BIN}" stat -a \
        -e instructions,cycles,CYCLE_ACTIVITY.STALLS_L3_MISS,OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD,OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD,OFFCORE_REQUESTS.DEMAND_DATA_RD \
        -I 1000 -o "${OUTDIR}/system.perf.log" &
    PROFILER_PIDS+=("$!")
    timeline "ALTO perf stat started (PID=$!)"
    sleep 2
    start_alto_scan_scale_controller
fi
if baseline_uses_kernel_tiering; then
    kswapd_pid="$(pgrep -x kswapd0 || true)"
    if [[ -n "${kswapd_pid}" ]]; then
        pidstat -h -u -p "${kswapd_pid}" 1 > "${OUTDIR}/kswapd0-pidstat.log" 2>&1 &
        PROFILER_PIDS+=("$!")
    fi
fi

WORKLOAD_START_NS="$(date +%s%N)"
WORKLOAD_POST_WARMUP_NS="${WORKLOAD_START_NS}"
timeline "Launching workload"
if baseline_is_memtis; then
    cgexec -g "memory,cpuset:${MEMTIS_CGNAME}" \
        numactl -C "${WORKLOAD_CORES}" env "${WORKLOAD_ENV[@]}" "${WORKLOAD_CMD[@]}" \
        > "${OUTDIR}/stdout.log" 2> "${OUTDIR}/stderr.log" &
elif baseline_is_mttm; then
    (
        mttm_single_tenant_runner 1 "${WORKLOAD_NAME}" -- \
            numactl -C "${WORKLOAD_CORES}" env "${WORKLOAD_ENV[@]}" "${WORKLOAD_CMD[@]}"
    ) > "${OUTDIR}/stdout.log" 2> "${OUTDIR}/stderr.log" &
else
    numactl -C "${WORKLOAD_CORES}" env "${WORKLOAD_ENV[@]}" "${WORKLOAD_CMD[@]}" \
        > "${OUTDIR}/stdout.log" 2> "${OUTDIR}/stderr.log" &
fi
WORKLOAD_PID=$!
PROFILE_PID="$(resolve_workload_profile_pid "${WORKLOAD_PID}" "${WORKLOAD_TARGET_EXE}")"
printf '%s\n' "${WORKLOAD_PID}" > "${OUTDIR}/root_pid.txt"
printf '%s\n' "${PROFILE_PID}" > "${OUTDIR}/profile_pid.txt"
timeline "workload root_pid=${WORKLOAD_PID} profile_pid=${PROFILE_PID}"

sleep 2
kill -0 "${WORKLOAD_PID}" 2>/dev/null || die "workload failed to start; check ${OUTDIR}/stderr.log"

if (( ENABLE_NUMA_MAPS )); then
    python3 "${PY_DIR}/parse_numa_maps.py" --monitor --pid "${PROFILE_PID}" \
        --interval "${NUMA_MAPS_INTERVAL}" > "${OUTDIR}/numa_maps.log" 2>&1 &
    PROFILER_PIDS+=("$!")
fi
if baseline_is_memtis; then
    memtis_track_hotness "${OUTDIR}/memtis-hotness.log" \
        > "${OUTDIR}/memtis-hotness.stderr.log" 2>&1 &
    PROFILER_PIDS+=("$!")
fi

if ! baseline_is_static; then
    timeline "Warmup phase ${WARMUP_SEC}s"
    sleep "${WARMUP_SEC}"
    WORKLOAD_POST_WARMUP_NS="$(date +%s%N)"
    kill -0 "${WORKLOAD_PID}" 2>/dev/null || warn "workload exited during warmup"

    if [[ -n "${WORKLOAD_CORES_AFTER}" ]] && kill -0 "${PROFILE_PID}" 2>/dev/null; then
        taskset -acp "${WORKLOAD_CORES_AFTER}" "${PROFILE_PID}" >/dev/null ||
            warn "failed to rebind workload to ${WORKLOAD_CORES_AFTER}"
    fi

    if baseline_uses_kernel_tiering; then
        TIERING_ENABLED=1
        enable_kernel_tiering || die "failed to enable ${BASELINE}"
        start_colloid_latency_monitor
    elif baseline_is_memtis; then
        baseline_mlimit_memtis "${DRAM_FREE_MIB}" || die "failed to set memtis limit"
        baseline_enable_memtis || die "failed to enable memtis"
        start_ksamplingd_pidstat
    elif baseline_is_mttm; then
        baseline_enable_mttm || die "failed to enable MTTM"
    fi
fi

timeline "Waiting for workload"
WORKLOAD_RC=0
wait "${WORKLOAD_PID}" || WORKLOAD_RC=$?
WORKLOAD_END_NS="$(date +%s%N)"
WORKLOAD_PID=""
timeline "workload finished rc=${WORKLOAD_RC}"

WORKLOAD_DURATION="$(write_duration "${WORKLOAD_START_NS}" "${WORKLOAD_POST_WARMUP_NS}" "${WORKLOAD_END_NS}" "${WORKLOAD_RC}")"
cleanup
timeline "Experiment complete"

printf '\n[microbench] output=%s baseline=%s workload=%s duration=%ss rc=%s\n' \
    "${OUTDIR}" "${BASELINE}" "${WORKLOAD_NAME}" "${WORKLOAD_DURATION}" "${WORKLOAD_RC}"
exit "${WORKLOAD_RC}"
