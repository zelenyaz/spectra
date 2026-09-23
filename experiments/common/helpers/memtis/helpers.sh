
# refer to: https://github.com/cosmoss-jigu/memtis
# Note: make sure the system tiering is been disabled.

memtis_sysfs_settings() {
    local CONFIG_NS=off    # use default value in kernel source code
    local CONFIG_NW=off    # ditto
    local CONFIG_CXL_MODE=on

    echo 199 | tee /sys/kernel/mm/htmm/htmm_sample_period
    echo 100007 | tee /sys/kernel/mm/htmm/htmm_inst_sample_period
    echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_hot
    echo 2 | tee /sys/kernel/mm/htmm/htmm_split_period
    echo 100000 | tee /sys/kernel/mm/htmm/htmm_adaptation_period
    echo 2000000 | tee /sys/kernel/mm/htmm/htmm_cooling_period
    echo 2 | tee /sys/kernel/mm/htmm/htmm_mode
    echo 500 | tee /sys/kernel/mm/htmm/htmm_demotion_period_in_ms
    echo 500 | tee /sys/kernel/mm/htmm/htmm_promotion_period_in_ms
    echo 4 | tee /sys/kernel/mm/htmm/htmm_gamma
    ###  cpu cap (per mille) for ksampled
    echo 30 | tee /sys/kernel/mm/htmm/ksampled_soft_cpu_quota

    if [[ "x${CONFIG_NS}" == "xoff" ]]; then
	echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_split
    else
	echo 0 | tee /sys/kernel/mm/htmm/htmm_thres_split
    fi

    if [[ "x${CONFIG_NW}" == "xoff" ]]; then
	echo 0 | tee /sys/kernel/mm/htmm/htmm_nowarm
    else
	echo 1 | tee /sys/kernel/mm/htmm/htmm_nowarm
    fi

    if [[ "x${CONFIG_CXL_MODE}" == "xon" ]]; then
	echo "enabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    else
	echo "disabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    fi
}

memtis_always_thp() {
    echo always | tee /sys/kernel/mm/transparent_hugepage/enabled
    echo always | tee /sys/kernel/mm/transparent_hugepage/defrag
}

baseline_setup_memtis() {
    sysctl kernel.perf_event_max_sample_rate=100000
    sh -c "echo off > /sys/devices/system/cpu/smt/control"

    memtis_sysfs_settings
    # memtis_always_thp
}

MEMTIS_CGNAME="${MEMTIS_CGNAME:-htmm}"
MEMTIS_CGPATH="/sys/fs/cgroup/${MEMTIS_CGNAME}"
MEMTIS_HELPER_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MEMTIS_KSAMPLINGD_CTL="${MEMTIS_KSAMPLINGD_CTL:-${MEMTIS_HELPER_DIR}/ksamplingd/ctl}"

memtis_ensure_ksamplingd_ctl() {
    if [[ ! -e "${MEMTIS_KSAMPLINGD_CTL}" &&
          "${MEMTIS_KSAMPLINGD_CTL}" == "${MEMTIS_HELPER_DIR}/ksamplingd/ctl" ]]; then
        make -C "${MEMTIS_HELPER_DIR}/ksamplingd" ctl || return 1
    fi

    if [[ ! -x "${MEMTIS_KSAMPLINGD_CTL}" ]]; then
        printf '[ERROR] ksamplingd ctl not found/executable: %s\n' "${MEMTIS_KSAMPLINGD_CTL}" >&2
        return 1
    fi
}

# called before launching the workloads
baseline_prepare_memtis() {
    baseline_setup_memtis

    cgcreate -g memory,cpuset:${MEMTIS_CGNAME}
    echo enabled | tee ${MEMTIS_CGPATH}/memory.htmm_enabled
}

baseline_mlimit_memtis() {
    local mem_in_mib=$1
    local mem_in_bytes=$((mem_in_mib * 1024 * 1024))
    local local_node=${NUMA_MEM:-0}

    echo ${mem_in_bytes} | tee ${MEMTIS_CGPATH}/memory.max_at_node${local_node}    
}

# called after warmup
baseline_enable_memtis() {
    memtis_ensure_ksamplingd_ctl || return 1
    "${MEMTIS_KSAMPLINGD_CTL}" on # start ksamplingd
}

baseline_disable_memtis() {
    memtis_ensure_ksamplingd_ctl || return 1
    "${MEMTIS_KSAMPLINGD_CTL}" off # kill ksamplingd
    echo disabled | tee ${MEMTIS_CGPATH}/memory.htmm_enabled
    cgdelete -g memory,cpuset:${MEMTIS_CGNAME}

    echo madvise | tee /sys/kernel/mm/transparent_hugepage/enabled
    echo madvise | tee /sys/kernel/mm/transparent_hugepage/defrag
}

memtis_track_hotness() {
    local out="$1"
    while true; do
        cat ${MEMTIS_CGPATH}/memory.hotness_stat >> "$out"
        sleep 1
    done
}

# ── mtcolloid: Memtis + colloid-mon congestion demotion ────────────────────
# The "mtcolloid" baseline runs Memtis with the colloid-mon latency sensor
# loaded on top. colloid-mon feeds local/remote loaded latency into the htmm
# kernel and flips htmm_cong_demotion on, so demotion is driven by memory-channel
# congestion instead of a static threshold. The load/unload is delegated to the
# Memtis userspace loader (memtis-userspace/scripts/load_colloid_mon.sh), which
# builds + insmods colloid-mon.ko and writes the htmm_cong_demotion sysfs knob.
# Override MEMTIS_LOAD_COLLOID_MON if the memtis-userspace checkout lives
# elsewhere.
MEMTIS_LOAD_COLLOID_MON="${MEMTIS_LOAD_COLLOID_MON:-${MEMTIS_ROOT:-/path/to/memtis}/memtis-userspace/scripts/load_colloid_mon.sh}"

# Load colloid-mon on top of an already-prepared Memtis baseline. Returns
# non-zero if the loader is missing/not executable or the load fails.
memtis_load_colloid_mon() {
    if [[ ! -x "${MEMTIS_LOAD_COLLOID_MON}" ]]; then
        printf '[ERROR] colloid-mon loader not found/executable: %s\n' "${MEMTIS_LOAD_COLLOID_MON}" >&2
        return 1
    fi
    "${MEMTIS_LOAD_COLLOID_MON}" load
}

# Unload colloid-mon and clear htmm_cong_demotion. Safe to call even if the
# module was never loaded (the loader's unload path tolerates that).
memtis_unload_colloid_mon() {
    [[ -x "${MEMTIS_LOAD_COLLOID_MON}" ]] || return 0
    "${MEMTIS_LOAD_COLLOID_MON}" unload
}
