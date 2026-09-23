#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

packages=(
    build-essential libnuma-dev numactl
    python3 python3-venv python3-pip python3-matplotlib python3-numpy python3-pandas
    sysstat cgroup-tools bpftrace kmod procps util-linux grub2-common
    git cmake curl gnupg podman sudo
    bc bison cpio debhelper dpkg-dev dwarves fakeroot flex libelf-dev
    libncurses-dev libssl-dev openssl rsync xz-utils zstd
)
kernels=(6.3.0-colloid-alto 5.15.19-htmm 5.15.145-mttm)
macro_paths=(ONEAPI_SETVARS LLAMA_BENCH LLAMA_MODEL FAISS_FLAT
             FAISS_BENCH_SEARCH FAISS_INDEX FAISS_QUERIES DRAMHIT_BIN VHT_BIN)
failures=0

usage() {
    cat <<'EOF'
Usage: ./scripts/prepare.sh [--build|--check|--print-deps|--install-deps]

Without arguments, check build dependencies and build Spectra and mbench,
offer to install missing kernels and prepare macrobenchmarks, then check the
remaining experiment prerequisites. Installation prompts require an answer.
--build only checks build dependencies and builds the two targets.
--check is read-only and checks the currently booted baseline kernel.
All options check CPU identity and topology; mismatches only produce warnings.
EOF
}

pass() { printf '[PASS] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
fail() { printf '[FAIL] %s\n' "$*" >&2; failures=$((failures + 1)); }
check_cmd() {
    if command -v "$1" >/dev/null 2>&1; then pass "command: $1"; else fail "missing command: $1"; fi
}

check_cpu_topology() {
    local info vendor family model sockets cores nodes
    if info="$(LC_ALL=C lscpu 2>/dev/null)"; then
        IFS='|' read -r vendor family model sockets cores nodes < <(
            printf '%s\n' "$info" | awk -F: '
                { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2) }
                $1 == "Vendor ID" { vendor = $2 }
                $1 == "CPU family" { family = $2 }
                $1 == "Model" { model = $2 }
                $1 == "Socket(s)" { sockets = $2 }
                $1 == "Core(s) per socket" { cores = $2 }
                $1 == "NUMA node(s)" { nodes = $2 }
                END { printf "%s|%s|%s|%s|%s|%s\n", vendor, family, model, sockets, cores, nodes }
            '
        )
        # Sapphire Rapids: Intel family 6, model 0x8f (143).
        if [[ "$vendor" == GenuineIntel && "$family" == 6 && "$model" == 143 &&
              "$sockets" == 2 && "$nodes" == 2 && "$cores" == 28 ]]; then
            pass 'CPU topology: Sapphire Rapids, 2 sockets, 2 NUMA nodes, 28 cores/socket'
            return 0
        fi
        warn "Expected Sapphire Rapids (Intel family 6/model 143), 2 sockets, 2 NUMA nodes, 28 cores/socket; detected vendor=${vendor:-unknown}, family=${family:-unknown}, model=${model:-unknown}, sockets=${sockets:-unknown}, nodes=${nodes:-unknown}, cores/socket=${cores:-unknown}."
    else
        warn 'Unable to check CPU identity and topology with lscpu.'
    fi
    warn 'You may need to adapt src/libtiermem/monitor.c and CPU bindings in experiments/ according to artifact/CPU-REQUIREMENTS.md. Continuing preparation.'
}

check_build_deps() {
    local cmd probe
    for cmd in make gcc g++ nproc; do check_cmd "$cmd"; done
    probe="$(mktemp "${TMPDIR:-/tmp}/spectra-numa-check.XXXXXX")"
    if command -v gcc >/dev/null 2>&1 &&
       printf '#include <numa.h>\nint main(void) { return numa_available(); }\n' |
           gcc -x c - -lnuma -o "$probe" >/dev/null 2>&1; then
        pass 'libnuma headers and linker library'
    else
        fail 'libnuma headers or linker library missing (install libnuma-dev)'
    fi
    rm -f -- "$probe"
}

build_targets() {
    local before="$failures"
    check_build_deps
    if (( failures > before )); then
        warn 'Install build dependencies with ./scripts/prepare.sh --install-deps'
        return 1
    fi
    local jobs="${JOBS:-$(nproc)}"
    export CCACHE_DISABLE="${CCACHE_DISABLE:-1}"
    printf '[build] Spectra library\n'
    make -C "${SPECTRA_ROOT}/src/libtiermem" -j"${jobs}"
    printf '[build] mbench\n'
    make -C "${SPECTRA_ROOT}/benchmarks/mbench" -j"${jobs}"
    pass 'libtiermem.so and mbench built'
}

ask_to_run() {
    local reply
    printf '%s [y/N] ' "$1" >&2
    if ! read -r reply; then return 1; fi
    case "$reply" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

missing_kernels() {
    local kernel
    MISSING_KERNELS=()
    for kernel in "${kernels[@]}"; do
        if [[ -f "/boot/vmlinuz-${kernel}" ]]; then
            pass "kernel image: ${kernel}"
        else
            MISSING_KERNELS+=("$kernel")
            warn "missing kernel image: /boot/vmlinuz-${kernel}"
        fi
    done
}

prepare_kernels() {
    local kernel
    missing_kernels
    if ((${#MISSING_KERNELS[@]} == 0)); then return; fi
    if ask_to_run 'Install missing kernels with third_party/install-kernels.sh?'; then
        for kernel in "${MISSING_KERNELS[@]}"; do
            "${SPECTRA_ROOT}/third_party/install-kernels.sh" "$kernel"
        done
        missing_kernels
    fi
    if ((${#MISSING_KERNELS[@]} > 0)); then
        fail "missing kernels: ${MISSING_KERNELS[*]}"
    fi
}

missing_macro_paths() {
    local name path
    MISSING_MACRO_PATHS=()
    for name in "${macro_paths[@]}"; do
        path="${!name:-}"
        case "$name" in
            LLAMA_BENCH|FAISS_FLAT|FAISS_BENCH_SEARCH|DRAMHIT_BIN|VHT_BIN)
                [[ -n "$path" && -x "$path" ]] ;;
            *) [[ -n "$path" && -f "$path" ]] ;;
        esac && pass "${name}: ${path}" && continue
        MISSING_MACRO_PATHS+=("$name")
        warn "${name} is unset or missing: ${path}"
    done
}

prepare_macro() {
    missing_macro_paths
    if ((${#MISSING_MACRO_PATHS[@]} == 0)); then return; fi
    if ask_to_run 'Download and build macrobenchmarks with third_party/prep-macro.sh?'; then
        ONEAPI_SETVARS="${ONEAPI_SETVARS:-}" "${SPECTRA_ROOT}/third_party/prep-macro.sh"
        missing_macro_paths
    fi
    if ((${#MISSING_MACRO_PATHS[@]} > 0)); then
        fail "missing macrobenchmark inputs: ${MISSING_MACRO_PATHS[*]}"
        warn 'Set missing paths in SPECTRA_CONFIG; prep-macro.sh can download missing FAISS resources.'
    fi
}

check_runtime() {
    local level="$1" cmd path_var path paranoid kernel
    spectra_load_config
    missing_macro_paths
    if ((${#MISSING_MACRO_PATHS[@]} > 0)); then
        fail "missing macrobenchmark inputs: ${MISSING_MACRO_PATHS[*]}"
    fi
    for cmd in bash make gcc g++ python3 numactl taskset pgrep; do check_cmd "$cmd"; done
    [[ -x "${PERF_BIN:-}" ]] && pass "PERF_BIN: ${PERF_BIN}" || fail "PERF_BIN is unset or not executable: ${PERF_BIN:-}"
    paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || printf unknown)"
    [[ "$paranoid" == -1 ]] && pass 'perf_event_paranoid=-1' || warn "perf_event_paranoid=${paranoid}; root runs may still work"
    [[ -x "${SPECTRA_ROOT}/benchmarks/mbench/mbench" ]] && pass 'mbench built' || fail 'mbench is not built; run scripts/prepare.sh --build'
    [[ -f "${SPECTRA_ROOT}/src/libtiermem/libtiermem.so" ]] && pass 'libtiermem.so built' || fail 'libtiermem.so is not built; run scripts/prepare.sh --build'
    for cmd in cgcreate cgdelete cgexec pidstat insmod rmmod numastat free \
               sync swapoff sysctl systemctl update-grub; do check_cmd "$cmd"; done
    [[ -x "${BPFTRACE_BIN:-/usr/bin/bpftrace}" ]] && pass "BPFTRACE_BIN: ${BPFTRACE_BIN:-/usr/bin/bpftrace}" || fail "BPFTRACE_BIN is not executable: ${BPFTRACE_BIN:-/usr/bin/bpftrace}"
    [[ -f "${SPECTRA_ROOT}/scripts/bpftrace/migrate_pages_reason_pid.bt" ]] && pass 'bpftrace script' || fail 'missing bpftrace script'
    [[ -f "${ALTO_SCAN_SCALE:-}" ]] && pass "ALTO_SCAN_SCALE: ${ALTO_SCAN_SCALE}" || fail "ALTO_SCAN_SCALE is unset or missing: ${ALTO_SCAN_SCALE:-}"
    [[ -d "${SPEC_DIR:-}" ]] && pass "SPEC_DIR: ${SPEC_DIR}" || fail "SPEC_DIR is unset or missing: ${SPEC_DIR:-}"
    [[ -f /etc/default/grub ]] && pass 'GRUB configuration' || fail 'missing /etc/default/grub'
    [[ -x "${SPECTRA_ROOT}/experiments/common/kernel_version_switcher.sh" ]] && pass 'kernel switcher' || fail 'kernel switcher is missing or not executable'
    for path_var in MTTM_ROOT MEMTIS_ROOT; do
        path="${!path_var:-}"
        [[ -n "$path" && -e "$path" ]] && pass "${path_var}: ${path}" || fail "${path_var} is unset or missing: ${path}"
    done
    for path_var in TIERINIT_KO MEMEATER_KO COLLOID_MON_KO; do
        path="${!path_var:-}"
        if [[ "$path" != /* || "$path" != *.ko ]]; then
            fail "${path_var} must be an absolute path to a .ko file: ${path}"
        elif [[ -f "$path" ]]; then
            pass "${path_var}: ${path}"
        elif [[ -f "$(dirname -- "$path")/Makefile" ]]; then
            pass "${path_var} can be built: ${path}"
        else
            fail "${path_var} is missing and has no Makefile: ${path}"
        fi
    done
    if [[ "$level" == check ]]; then
        kernel="$(uname -r)"
        case "$kernel" in
            6.3.0-colloid-alto|5.15.19-htmm|5.15.145-mttm) pass "baseline kernel: ${kernel}" ;;
            *) fail "unsupported full-evaluation kernel: ${kernel}" ;;
        esac
    fi
}

check_cpu_topology

mode="${1:-prepare}"
case "$mode" in
    -h|--help) usage; exit 0 ;;
    --print-deps)
        printf 'Ubuntu packages:\n  %s\nPython packages:\n' "${packages[*]}"
        sed 's/^/  /' "${SPECTRA_ROOT}/requirements-analysis.txt"
        exit 0
        ;;
    --install-deps)
        [[ "$EUID" -eq 0 ]] || spectra_die 'Run --install-deps as root'
        apt-get update
        apt-get install -y "${packages[@]}"
        exit 0
        ;;
    --build|--check|prepare) ;;
    *) usage >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    usage >&2
    exit 2
fi

case "$mode" in
    --build) build_targets ;;
    --check) check_runtime check ;;
    prepare)
        build_targets
        prepare_kernels
        spectra_load_config
        prepare_macro
        check_runtime prepare
        ;;
esac
printf '[summary] failures=%d\n' "$failures"
(( failures == 0 ))
