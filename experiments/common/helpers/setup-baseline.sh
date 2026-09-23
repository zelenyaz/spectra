#!/usr/bin/env bash

# Shell helper for configuring experiment baselines.
# This file is intended to be sourced by other scripts, but it also provides a
# small CLI for manual setup and debugging.

if [[ -z "${BASH_VERSION:-}" ]]; then
    echo "[ERROR] setup-baseline.sh must be run by bash." >&2
    return 1 2>/dev/null || exit 1
fi

readonly SETUP_BASELINE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly MEMTIS_KERNEL_RELEASE="5.15.19-htmm"

declare -Ag BASELINE_SETUP_FUNCS=()
declare -Ag BASELINE_DESCRIPTIONS=()
declare -Ag MODULE_PATHS_BY_KERNEL=()
declare -Ag KERNEL_PROFILE_DESCRIPTIONS=()

if [[ -f "${SETUP_BASELINE_DIR}/memtis/helpers.sh" ]]; then
    # shellcheck source=/dev/null
    source "${SETUP_BASELINE_DIR}/memtis/helpers.sh"
fi

if [[ -f "${SETUP_BASELINE_DIR}/mttm-helper.sh" ]]; then
    # shellcheck source=/dev/null
    source "${SETUP_BASELINE_DIR}/mttm-helper.sh"
fi

log_info() {
    printf '[INFO] %s\n' "$*"
}

log_warn() {
    printf '[WARN] %s\n' "$*" >&2
}

log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

die() {
    log_error "$*"
    return 1
}

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        die "This operation requires root privileges."
        return 1
    fi
}

require_command() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        die "Required command not found: ${cmd}"
        return 1
    fi
}

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        die "Required file not found: ${path}"
        return 1
    fi
}

require_writable_path() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        die "Required path not found: ${path}"
        return 1
    fi
    if [[ ! -w "${path}" ]]; then
        die "Path is not writable: ${path}"
        return 1
    fi
}

show_memory_status() {
    free -h || return 1
    numactl -H | grep free || return 1
}

write_value() {
    local path="$1"
    local value="$2"
    local description="$3"

    require_writable_path "${path}" || return 1
    printf '%s\n' "${value}" > "${path}" || {
        die "Failed to set ${description} (${path}=${value})."
        return 1
    }
    log_info "${description}: ${value}"
}

module_is_loaded() {
    local module="$1"
    lsmod | awk '{print $1}' | grep -Fx "${module}" >/dev/null 2>&1
}

remove_module_if_loaded() {
    local module="$1"

    if ! module_is_loaded "${module}"; then
        return 0
    fi

    log_info "Removing kernel module ${module}"
    rmmod "${module}" >/dev/null 2>&1 || {
        die "Failed to remove kernel module: ${module}"
        return 1
    }
}

load_module() {
    local module_path="$1"
    shift

    require_file "${module_path}" || return 1
    log_info "Loading kernel module ${module_path}"
    if (($# > 0)); then
        log_info "Module parameters: $*"
    fi

    insmod "${module_path}" "$@" || {
        die "Failed to load kernel module: ${module_path}"
        return 1
    }
}

compute_memeater_size_mib() {
    local numa_node="$1"
    local target_free_mib="$2"

    require_command numastat || return 1

    local computed
    computed="$(numastat -m | awk -v nidx="${numa_node}" -v target="${target_free_mib}" '
        $1 == "MemFree" {
            node_col = 2 + nidx
            if (node_col > NF) {
                exit 2
            }
            result = int($(node_col) - target)
            print result
            exit 0
        }
        END {
            if (NR == 0) {
                exit 3
            }
        }
    ')" || {
        die "Unable to read free memory for NUMA node ${numa_node} from numastat -m."
        return 1
    }

    if [[ ! "${computed}" =~ ^-?[0-9]+$ ]]; then
        die "Unexpected memeater size computed for NUMA node ${numa_node}: ${computed}"
        return 1
    fi

    if (( computed < 0 )); then
        die "Requested free memory ${target_free_mib} MiB exceeds current free memory on NUMA node ${numa_node}."
        return 1
    fi

    printf '%s\n' "${computed}"
}

register_baseline() {
    local name="$1"
    local setup_func="$2"
    local description="$3"

    BASELINE_SETUP_FUNCS["${name}"]="${setup_func}"
    BASELINE_DESCRIPTIONS["${name}"]="${description}"
}

register_kernel_module_profile() {
    local kernel_release="$1"
    local tierinit_path="$2"
    local memeater_path="$3"
    local colloid_path="$4"
    local description="$5"

    MODULE_PATHS_BY_KERNEL["${kernel_release}:tierinit"]="${tierinit_path}"
    MODULE_PATHS_BY_KERNEL["${kernel_release}:memeater"]="${memeater_path}"
    MODULE_PATHS_BY_KERNEL["${kernel_release}:colloid-mon"]="${colloid_path}"
    KERNEL_PROFILE_DESCRIPTIONS["${kernel_release}"]="${description}"
}

current_kernel_release() {
    require_command uname || return 1
    uname -r
}

require_kernel_release() {
    local expected_release="$1"
    local actual_release

    actual_release="$(current_kernel_release)" || return 1
    if [[ "${actual_release}" != "${expected_release}" ]]; then
        die "Kernel release mismatch: expected '${expected_release}', got '${actual_release}'."
        return 1
    fi
}

baseline_require_memtis_kernel() {
    require_kernel_release "${MEMTIS_KERNEL_RELEASE}"
}

list_supported_kernels() {
    local kernel_release

    log_info "Supported kernel module profiles:"
    for kernel_release in $(printf '%s\n' "${!KERNEL_PROFILE_DESCRIPTIONS[@]}" | sort); do
        printf '  %-24s %s\n' "${kernel_release}" "${KERNEL_PROFILE_DESCRIPTIONS[${kernel_release}]}"
    done
}

resolve_module_path() {
    local module_name="$1"
    local kernel_release
    local resolved_path
    local module_dir

    kernel_release="$(current_kernel_release)" || return 1
    resolved_path="${MODULE_PATHS_BY_KERNEL["${kernel_release}:${module_name}"]:-}"

    if [[ -z "${resolved_path}" ]]; then
        log_error "No module path mapping is configured for kernel '${kernel_release}' and module '${module_name}'."
        list_supported_kernels >&2
        return 1
    fi

    if [[ ! -f "${resolved_path}" ]]; then
        module_dir="$(dirname -- "${resolved_path}")"
        if [[ ! -f "${module_dir}/Makefile" ]]; then
            die "Module file is missing and no Makefile was found: ${resolved_path}"
            return 1
        fi
        require_command make || return 1
        log_info "Building kernel module ${resolved_path}" >&2
        make -C "${module_dir}" >&2 || {
            die "Failed to build kernel module: ${resolved_path}"
            return 1
        }
        if [[ ! -f "${resolved_path}" ]]; then
            die "Module build did not produce: ${resolved_path}"
            return 1
        fi
    fi

    printf '%s\n' "${resolved_path}"
}

list_baselines() {
    local name

    log_info "Available baselines:"
    for name in $(printf '%s\n' "${!BASELINE_SETUP_FUNCS[@]}" | sort); do
        printf '  %-12s %s\n' "${name}" "${BASELINE_DESCRIPTIONS[${name}]}"
    done
}

setup_baseline() {
    local baseline_name="${1:-}"
    shift || true

    if [[ -z "${baseline_name}" ]]; then
        die "Missing baseline name. Use: setup_baseline <baseline>"
        return 1
    fi

    if [[ -z "${BASELINE_SETUP_FUNCS[${baseline_name}]:-}" ]]; then
        log_error "Unknown baseline: ${baseline_name}"
        list_baselines >&2
        return 1
    fi

    log_info "Configuring baseline '${baseline_name}'"
    "${BASELINE_SETUP_FUNCS[${baseline_name}]}" "$@"
}

# $1: local NUMA node, default 0
# $2: remaining free memory in MiB after filling
usemem() {
    local numa_node="${1:-0}"
    local target_free_mib="${2:-}"
    local module_path
    local size_mib
    local -a module_args=()

    require_root || return 1
    require_command numactl || return 1
    require_command free || return 1

    if [[ -z "${target_free_mib}" ]]; then
        die "Usage: usemem <numa-node> <remaining-free-mib>"
        return 1
    fi

    if [[ ! "${numa_node}" =~ ^[0-9]+$ ]]; then
        die "NUMA node must be a non-negative integer: ${numa_node}"
        return 1
    fi

    if [[ ! "${target_free_mib}" =~ ^[0-9]+$ ]]; then
        die "Remaining free memory must be a non-negative integer in MiB: ${target_free_mib}"
        return 1
    fi

    module_path="$(resolve_module_path "memeater")" || return 1

    remove_module_if_loaded "memeater" || return 1

    size_mib="$(compute_memeater_size_mib "${numa_node}" "${target_free_mib}")" || return 1

    module_args=("sizeMiB=${size_mib}")
    if [[ -n "${ENABLE_THP:-}" ]]; then
        log_info "ENABLE_THP is set; loading memeater with huge pages."
        module_args+=("PGSIZE=2097152" "PGORDER=9")
    fi

    log_info "Setting NUMA node ${numa_node} free memory target to ${target_free_mib} MiB."
    load_module "${module_path}" "${module_args[@]}" || return 1
    show_memory_status || return 1
    log_info "Local memory target applied on NUMA node ${numa_node}."
}

freemem() {
    require_root || return 1
    require_command free || return 1

    remove_module_if_loaded "memeater" || return 1
    show_memory_status || return 1
    log_info "Freed local memory."
}

# Sysfs THP knobs expose their current selection wrapped in brackets, e.g.
# "always [madvise] never". Compare against "[<mode>]" to detect the active mode.
thp_enable_always() {
    require_root || return 1

    local enabled_path="/sys/kernel/mm/transparent_hugepage/enabled"
    local defrag_path="/sys/kernel/mm/transparent_hugepage/defrag"
    local current_enabled current_defrag

    require_writable_path "${enabled_path}" || return 1
    require_writable_path "${defrag_path}" || return 1

    current_enabled="$(<"${enabled_path}")" || {
        die "Failed to read ${enabled_path}."
        return 1
    }
    current_defrag="$(<"${defrag_path}")" || {
        die "Failed to read ${defrag_path}."
        return 1
    }

    if [[ "${current_enabled}" == *"[always]"* ]]; then
        log_info "THP enabled is already 'always'."
    else
        write_value "${enabled_path}" "always" "THP enabled" || return 1
    fi

    if [[ "${current_defrag}" == *"[always]"* ]]; then
        log_info "THP defrag is already 'always'."
    else
        write_value "${defrag_path}" "always" "THP defrag" || return 1
    fi
}

thp_restore_madvise() {
    require_root || return 1

    local enabled_path="/sys/kernel/mm/transparent_hugepage/enabled"
    local defrag_path="/sys/kernel/mm/transparent_hugepage/defrag"
    local current_enabled current_defrag

    require_writable_path "${enabled_path}" || return 1
    require_writable_path "${defrag_path}" || return 1

    current_enabled="$(<"${enabled_path}")" || {
        die "Failed to read ${enabled_path}."
        return 1
    }
    current_defrag="$(<"${defrag_path}")" || {
        die "Failed to read ${defrag_path}."
        return 1
    }

    if [[ "${current_enabled}" == *"[madvise]"* ]]; then
        log_info "THP enabled is already 'madvise'."
    else
        write_value "${enabled_path}" "madvise" "THP enabled" || return 1
    fi

    if [[ "${current_defrag}" == *"[madvise]"* ]]; then
        log_info "THP defrag is already 'madvise'."
    else
        write_value "${defrag_path}" "madvise" "THP defrag" || return 1
    fi
}

setup_common() {
    require_root || return 1
    require_command numactl || return 1
    require_command free || return 1
    require_command sync || return 1
    require_command swapoff || return 1

    log_info "Preparing the system for baseline setup."
    write_value "/proc/sys/kernel/nmi_watchdog" "0" "NMI watchdog" || return 1
    sync || {
        die "sync failed."
        return 1
    }
    write_value "/proc/sys/vm/drop_caches" "3" "Page cache drop" || return 1
    swapoff -a || {
        die "Failed to disable swap."
        return 1
    }
    log_info "Swap disabled."
    show_memory_status || return 1
}

baseline_enable_tpp() {
    local tierinit_module_path
    tierinit_module_path="$(resolve_module_path "tierinit")" || return 1
    write_value "/sys/kernel/mm/numa/demotion_enabled" "1" "NUMA demotion" || return 1
    write_value "/proc/sys/kernel/numa_balancing" "2" "NUMA balancing mode" || return 1
    load_module "${tierinit_module_path}" || return 1
    show_memory_status || return 1
    log_info "TPP baseline is ready."
}

baseline_setup_tpp() {
    setup_common || return 1
    baseline_enable_tpp
}

baseline_enable_colloid() {
    local tierinit_module_path
    local colloid_module_path

    tierinit_module_path="$(resolve_module_path "tierinit")" || return 1
    colloid_module_path="$(resolve_module_path "colloid-mon")" || return 1
    write_value "/sys/kernel/mm/numa/demotion_enabled" "1" "NUMA demotion" || return 1
    write_value "/proc/sys/kernel/numa_balancing" "6" "NUMA balancing mode" || return 1
    load_module "${tierinit_module_path}" || return 1
    load_module "${colloid_module_path}" || return 1
    show_memory_status || return 1
    log_info "Colloid baseline is ready."
}

baseline_setup_colloid() {
    setup_common || return 1
    baseline_enable_colloid
}

baseline_setup_alto() {
    baseline_setup_colloid "$@"
}

baseline_setup_memtis_cli() {
    declare -F baseline_setup_memtis >/dev/null 2>&1 || {
        die "memtis helpers are not available."
        return 1
    }

    baseline_require_memtis_kernel || return 1
    setup_common || return 1
    disable_tiering || return 1
    baseline_setup_memtis "$@"
}

# mtcolloid = memtis baseline + colloid-mon congestion-demotion sensor. Prepares
# the Memtis sysfs tuning, then loads colloid-mon.ko (enabling htmm_cong_demotion).
# Undo with disable-tiering (removes colloid_mon) plus the usual memtis teardown.
baseline_setup_mtcolloid_cli() {
    declare -F baseline_setup_memtis >/dev/null 2>&1 || {
        die "memtis helpers are not available."
        return 1
    }
    declare -F memtis_load_colloid_mon >/dev/null 2>&1 || {
        die "memtis colloid-mon helpers are not available."
        return 1
    }

    baseline_require_memtis_kernel || return 1
    setup_common || return 1
    disable_tiering || return 1
    baseline_setup_memtis "$@" || return 1
    memtis_load_colloid_mon
}

# $1: page_type — "hugepage" or "basepage"
baseline_setup_mttm_cli() {
    declare -F baseline_prepare_mttm_hugepage >/dev/null 2>&1 || {
        die "mttm helpers are not available."
        return 1
    }

    local page_type="${1:-}"
    if [[ -z "${page_type}" ]]; then
        die "Usage: setup-baseline mttm <hugepage|basepage>"
        return 1
    fi

    require_kernel_release "${MTTM_KERNEL_VER}" || return 1
    setup_common || return 1
    disable_tiering || return 1

    case "${page_type}" in
        hugepage)
            baseline_prepare_mttm_hugepage
            ;;
        basepage)
            baseline_prepare_mttm_basepage
            ;;
        *)
            die "Invalid page_type '${page_type}': must be 'hugepage' or 'basepage'."
            return 1
            ;;
    esac
}

disable_tiering() {
    require_root || return 1

    log_info "Disabling tiering-related kernel settings."
    write_value "/sys/kernel/mm/numa/demotion_enabled" "0" "NUMA demotion" || return 1
    write_value "/proc/sys/kernel/numa_balancing" "0" "NUMA balancing mode" || return 1
    sleep 2
    remove_module_if_loaded "tierinit" || return 1
    sleep 2
    remove_module_if_loaded "colloid_mon" || return 1
    log_info "Tiering disabled."
}

usage() {
    cat <<'EOF'
Usage:
  setup-baseline.sh list-baselines
  setup-baseline.sh list-supported-kernels
  setup-baseline.sh setup-baseline <baseline>
  setup-baseline.sh setup-tpp
  setup-baseline.sh setup-colloid
  setup-baseline.sh setup-alto
  setup-baseline.sh setup-memtis
  setup-baseline.sh setup-mtcolloid
  setup-baseline.sh setup-mttm <hugepage|basepage>
  setup-baseline.sh disable-tiering
  setup-baseline.sh usemem <numa-node> <remaining-free-mib>
  setup-baseline.sh freemem

Environment:
  ENABLE_THP=1    Load memeater.ko with huge-page settings.

Examples:
  setup-baseline.sh list-baselines
  setup-baseline.sh list-supported-kernels
  setup-baseline.sh setup-baseline tpp
  setup-baseline.sh setup-baseline colloid
  setup-baseline.sh setup-baseline alto
  setup-baseline.sh setup-baseline memtis
  setup-baseline.sh setup-baseline mtcolloid
  setup-baseline.sh setup-baseline mttm hugepage
EOF
}

main() {
    local cmd="${1:-}"
    shift || true

    case "${cmd}" in
        list-baselines)
            list_baselines
            ;;
        list-supported-kernels)
            list_supported_kernels
            ;;
        setup-baseline)
            setup_baseline "$@"
            ;;
        setup-tpp)
            baseline_setup_tpp "$@"
            ;;
        setup-colloid)
            baseline_setup_colloid "$@"
            ;;
        setup-alto)
            baseline_setup_alto "$@"
            ;;
        setup-memtis)
            baseline_setup_memtis_cli "$@"
            ;;
        setup-mtcolloid)
            baseline_setup_mtcolloid_cli "$@"
            ;;
        setup-mttm)
            baseline_setup_mttm_cli "$@"
            ;;
        disable-tiering)
            disable_tiering
            ;;
        usemem)
            usemem "$@"
            ;;
        freemem)
            freemem
            ;;
        ""|-h|--help|help)
            usage
            ;;
        *)
            log_error "Unknown command: ${cmd}"
            usage >&2
            return 1
            ;;
    esac
}

register_baseline "colloid" "baseline_setup_colloid" "Tierinit + colloid monitor baseline"
register_baseline "alto" "baseline_setup_alto" "Alias of colloid baseline"
register_baseline "memtis" "baseline_setup_memtis_cli" "memtis baseline (requires kernel 5.15.19-htmm)"
register_baseline "mtcolloid" "baseline_setup_mtcolloid_cli" "memtis + colloid-mon congestion demotion (requires kernel 5.15.19-htmm)"
register_baseline "tpp" "baseline_setup_tpp" "Linux default memory tiering baseline"
register_baseline "mttm" "baseline_setup_mttm_cli" "MTTM baseline (requires kernel 5.15.145-mttm); arg: hugepage|basepage"
register_kernel_module_profile \
    "6.3.0-colloid-alto" \
    "${TIERINIT_KO:-}" \
    "${MEMEATER_KO:-}" \
    "${COLLOID_MON_KO:-}" \
    "Kernel-specific TPP modules"

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
