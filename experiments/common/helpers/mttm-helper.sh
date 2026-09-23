
# MTTM baseline helper — meant to be sourced by experiment scripts.
# Requires kernel 5.15.145-mttm.

MTTM_KERNEL_VER="5.15.145-mttm"

MTTM_ROOT="${MTTM_ROOT:-/path/to/MTTM_ae_EuroSys26}"
MTTM_RUNNER_WRAPPER="${MTTM_RUNNER_WRAPPER:-${MTTM_ROOT}/scripts/run_bench}"

# --- Minimal logging helpers ---

_mttm_log_info() {
    printf '[MTTM] %s\n' "$*"
}

_mttm_log_error() {
    printf '[MTTM][ERROR] %s\n' "$*" >&2
}

_mttm_die() {
    _mttm_log_error "$*"
    return 1
}

mttm_ensure_run_bench() {
    if [[ -x "${MTTM_RUNNER_WRAPPER}" ]]; then
        return 0
    fi

    _mttm_log_info "Building run_bench in ${MTTM_ROOT}/scripts"
    if ! make -C "${MTTM_ROOT}/scripts" run_bench; then
        _mttm_die "Failed to build run_bench in ${MTTM_ROOT}/scripts." || return 1
    fi
    [[ -x "${MTTM_RUNNER_WRAPPER}" ]] ||
        _mttm_die "MTTM run_bench wrapper not executable: ${MTTM_RUNNER_WRAPPER}"
}

_mttm_write_value() {
    local path="$1"
    local value="$2"
    local description="${3:-${path}}"

    if [[ ! -e "${path}" ]]; then
        _mttm_die "Path not found: ${path}" || return 1
    fi
    if ! printf '%s\n' "${value}" > "${path}" 2>/dev/null; then
        _mttm_die "Failed to set ${description} (${path}=${value})." || return 1
    fi
}

_mttm_sysctl_write_if_needed() {
    local key="$1"
    local value="$2"
    local description="${3:-${key}}"
    local current

    if ! current=$(sysctl -n "${key}" 2>/dev/null); then
        _mttm_die "Failed to read ${description} (${key})." || return 1
    fi
    if [[ "${current}" == "${value}" ]]; then
        return 0
    fi
    if ! sysctl -q "${key}=${value}"; then
        _mttm_die "Failed to set ${description} (${key}=${value})." || return 1
    fi
}

_mttm_check_kernel() {
    local actual_ver
    actual_ver=$(uname -r)
    if [[ "${actual_ver}" != "${MTTM_KERNEL_VER}" ]]; then
        _mttm_die "Expecting kernel ${MTTM_KERNEL_VER}, got ${actual_ver}." || return 1
    fi
}

# --- Kernel parameter setup ---

__mttm_common_setup() {
    _mttm_write_value /proc/sys/vm/drop_caches                1    "drop_caches"                || return 1
    _mttm_write_value /proc/sys/vm/pingpong_reduce_limit      3    "pingpong_reduce_limit"      || return 1
    _mttm_write_value /proc/sys/vm/mig_cputime_threshold      300  "mig_cputime_threshold"      || return 1

    _mttm_write_value /proc/sys/vm/kmigrated_period_in_ms       1000 "kmigrated_period_in_ms"       || return 1
    _mttm_write_value /proc/sys/vm/ksampled_trace_period_in_ms  2000 "ksampled_trace_period_in_ms"  || return 1
    _mttm_write_value /proc/sys/vm/check_stable_sample_rate     1    "check_stable_sample_rate"     || return 1

    # disable DMA migration
    _mttm_write_value /proc/sys/vm/use_dma_migration          0    "use_dma_migration"          || return 1

    _mttm_sysctl_write_if_needed kernel.perf_cpu_time_max_percent 0      "kernel.perf_cpu_time_max_percent" || return 1
    _mttm_sysctl_write_if_needed kernel.perf_event_max_sample_rate 100000 "kernel.perf_event_max_sample_rate" || return 1
    sysctl -q vm.enable_ksampled=0                     || return 1
    sysctl -q vm.enable_ksampled=1                     || return 1
}

# Settings shared by both hugepage and basepage MTTM baselines.
__mttm_baseline_common_writes() {
    _mttm_write_value /proc/sys/vm/use_dram_determination   1     "use_dram_determination"   || return 1
    _mttm_write_value /proc/sys/vm/use_region_separation    1     "use_region_separation"    || return 1
    _mttm_write_value /proc/sys/vm/use_memstrata_policy     0     "use_memstrata_policy"     || return 1
    _mttm_write_value /proc/sys/vm/mttm_local_dram_string   256G  "mttm_local_dram_string"   || return 1
    _mttm_write_value /proc/sys/vm/print_more_info          1     "print_more_info"          || return 1

    _mttm_write_value /proc/sys/vm/mar_weight               1     "mar_weight"               || return 1
    _mttm_write_value /proc/sys/vm/hi_weight                1     "hi_weight"                || return 1
    _mttm_write_value /proc/sys/vm/hugepage_shift_factor    3     "hugepage_shift_factor"    || return 1
    _mttm_write_value /proc/sys/vm/hugepage_period_factor   1     "hugepage_period_factor"   || return 1

    _mttm_write_value /proc/sys/vm/use_pingpong_reduce      1     "use_pingpong_reduce"      || return 1
    _mttm_write_value /proc/sys/vm/reduce_scan              1     "reduce_scan"              || return 1

    _mttm_write_value /proc/sys/vm/use_rxc_monitoring       1     "use_rxc_monitoring"       || return 1
}

# Called before launching the workloads.
baseline_prepare_mttm_hugepage() {
    _mttm_check_kernel || return 1
    dmesg --clear

    __mttm_baseline_common_writes || return 1

    _mttm_write_value /proc/sys/vm/pebs_init_period         4999   "pebs_init_period"        || return 1
    _mttm_write_value /proc/sys/vm/pebs_stable_period         10007   "pebs_stable_period"        || return 1
    _mttm_write_value /proc/sys/vm/pingpong_reduce_threshold  500    "pingpong_reduce_threshold" || return 1
    _mttm_write_value /sys/kernel/mm/transparent_hugepage/enabled always "transparent_hugepage/enabled" || return 1

    __mttm_common_setup || return 1
    _mttm_log_info "MTTM hugepage baseline prepared."
}

baseline_prepare_mttm_basepage() {
    _mttm_check_kernel || return 1
    dmesg --clear

    __mttm_baseline_common_writes || return 1

    _mttm_write_value /proc/sys/vm/pebs_init_period         1009    "pebs_init_period"        || return 1
    _mttm_write_value /proc/sys/vm/pebs_stable_period         4999    "pebs_stable_period"        || return 1
    _mttm_write_value /proc/sys/vm/pingpong_reduce_threshold  200    "pingpong_reduce_threshold" || return 1
    _mttm_write_value /sys/kernel/mm/transparent_hugepage/enabled madvise "transparent_hugepage/enabled" || return 1
    # Target cooling period / increasing granularity (basepage-only knobs).
    _mttm_write_value /proc/sys/vm/basepage_shift_factor      9      "basepage_shift_factor"     || return 1
    _mttm_write_value /proc/sys/vm/basepage_period_factor     40     "basepage_period_factor"    || return 1

    __mttm_common_setup || return 1
    _mttm_log_info "MTTM basepage baseline prepared."
}

# $1: such as 4G
mttm_limit_local_dram_size() {
    local size="${1:-}"
    if [[ -z "${size}" ]]; then
        _mttm_die "Usage: mttm_limit_local_dram_size <size, e.g. 4G>" || return 1
    fi
    _mttm_write_value /proc/sys/vm/mttm_local_dram_string "${size}" "mttm_local_dram_string"
}

# $1: order of tenant
# $2: workload name (short string, used for logging)
# $3: literal "--" separator
# $4..: workload_cmd
# Note: DO NOT set cpuset for the memcg, use `numactl` or `taskset` in outer runner.
#       ${CGMEM_DIR}/memory.max_at_node0 is set by MTTM (partitioning mechanism), not by the user.
mttm_single_tenant_runner() {
    if [[ $# -lt 4 || "$3" != "--" ]]; then
        _mttm_die "Usage: mttm_single_tenant_runner <tenant_order> <workload_name> -- <cmd> [args...]" || return 1
    fi
    mttm_ensure_run_bench || return 1
    local tenant_order="$1"
    local workload_name="$2"
    local CGMEM_DIR="/sys/fs/cgroup/memory/mttm_${tenant_order}"

    cgdelete -g "memory:mttm_${tenant_order}" 2>/dev/null || true
    cgcreate -g "memory:mttm_${tenant_order}" || {
        _mttm_die "cgcreate failed for mttm_${tenant_order}" || return 1
    }
    _mttm_write_value "${CGMEM_DIR}/memory.use_mig"  disabled "memory.use_mig" || return 1  # enable after warmup
    _mttm_write_value "${CGMEM_DIR}/memory.use_warm" enabled  "memory.use_warm" || return 1
    _mttm_write_value "${CGMEM_DIR}/cgroup.procs"    "$$"     "cgroup.procs"    || return 1

    "$MTTM_RUNNER_WRAPPER" "${workload_name}" "${@:4}"
}

# Called after warmup.
baseline_enable_mttm() {
    local cgdir
    for cgdir in /sys/fs/cgroup/memory/mttm_*; do
        if [[ -d "${cgdir}" ]]; then
            _mttm_write_value "${cgdir}/memory.use_mig" enabled "memory.use_mig" || return 1
        fi
    done
}

baseline_disable_mttm() {
    _mttm_write_value /proc/sys/vm/use_dram_determination  0 "use_dram_determination"  || return 1
    _mttm_write_value /proc/sys/vm/use_region_separation   0 "use_region_separation"   || return 1
    _mttm_write_value /proc/sys/vm/use_memstrata_policy    0 "use_memstrata_policy"    || return 1
    _mttm_write_value /proc/sys/vm/print_more_info         0 "print_more_info"         || return 1

    _mttm_write_value /proc/sys/vm/mar_weight              0 "mar_weight"              || return 1
    _mttm_write_value /proc/sys/vm/hi_weight               0 "hi_weight"               || return 1
    _mttm_write_value /proc/sys/vm/hugepage_shift_factor   0 "hugepage_shift_factor"   || return 1
    _mttm_write_value /proc/sys/vm/hugepage_period_factor  0 "hugepage_period_factor"  || return 1

    _mttm_write_value /proc/sys/vm/use_pingpong_reduce         0 "use_pingpong_reduce"         || return 1
    _mttm_write_value /proc/sys/vm/pingpong_reduce_threshold   0 "pingpong_reduce_threshold"   || return 1
    _mttm_write_value /proc/sys/vm/reduce_scan                 0 "reduce_scan"                 || return 1
    _mttm_write_value /proc/sys/vm/use_rxc_monitoring          0 "use_rxc_monitoring"          || return 1
    _mttm_write_value /sys/kernel/mm/transparent_hugepage/enabled madvise "transparent_hugepage/enabled" || return 1

    sysctl -q vm.enable_ksampled=0 || return 1
}
