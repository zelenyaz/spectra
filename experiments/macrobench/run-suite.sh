#!/usr/bin/env bash
#
# run-suite.sh — macro-benchmark suite driver for tiering baselines.
#
# Runs pairs of real applications (llama.cpp, SPEC CPU 2017, FAISS) under
# every tiering baseline, reusing the dual-workload runner.
#
# Execution model:
#   All macrobench combos run as two independent processes under
#   ../common/dual-workload/run.sh (W1 + W2). libtiermem-single is excluded because
#   it assumes a single target process.
#
# Apps registered here:
#   llama-qwen              — llama.cpp llama-bench, Qwen3-8B-Q4_K_M
#   spec-607-cactubssn      — SPEC CPU 2017 607.cactuBSSN_s
#   spec-619-lbm            — SPEC CPU 2017 619.lbm_s
#   faiss-flat              — FAISS 1-Flat tutorial binary
#   faiss-bench-polysemous  — FAISS bench_search on SIFT200M polysemous index
#
# Usage:
#   sudo ./run-suite.sh                              # full matrix (all baselines × all combos)
#   sudo ./run-suite.sh <baseline> <combo-id>        # one combo at the combo's default thread counts
#   sudo ./run-suite.sh <baseline> <combo-id> T1 T2  # one combo with explicit thread counts
#
# Baselines : static tpp colloid alto libtiermem memtis mttm
# Combos    : see combo_summary() output, or run with no args to list.
#
# Tunables (env vars override per-combo table defaults):
#   DRAM_FREE_MIB           target free DRAM (MiB) on NUMA_MEM for memeater baselines
#   TIERMEM_DRAM_BUDGET     libtiermem DRAM budget string (e.g. 5000M)
#   MEMTIS_DRAM_BUDGET_MIB  Memtis DRAM budget (MiB) on NUMA_MEM
#   MTTM_DRAM_MIB           MTTM local DRAM size (MiB)
#   MTTM_PAGE_TYPE          MTTM page type: hugepage|basepage (default basepage)
#   WARMUP_SEC              pre-tiering warmup in seconds  (default 30)
#   NUMA_MEM                fast-tier NUMA node            (default 0)
#   RESULTS_ROOT            output root                    (default <script-dir>/results)
#   W1_MEM_NODE / W2_MEM_NODE  static-baseline memory binding (default 1 / 1)
#
#   libtiermem extras (propagated to inject_tiermem_env in run.sh; empty = library default):
#     TIERMEM_MONITOR_CPU TIERMEM_MIGRATE_CPU_START TIERMEM_OBJ_THRESHOLD
#     TIERMEM_EPOCH_SEC TIERMEM_PEBS_PERIOD TIERMEM_STORE_PERIOD
#     TIERMEM_ALLLOAD_PERIOD TIERMEM_OCR_INTERVAL TIERMEM_MIGRATE_THREADS
#     TIERMEM_HYSTERESIS TIERMEM_REG_THETA TIERMEM_LOG_LEVEL
#
# Core layout (AE testbed — 2 sockets, 28 cores each, HT off):
#   node 0 cores 0-27   (workload home node, post-warmup)
#   node 1 cores 28-55  (workload bootstrap node during warmup)
#   libtiermem workers : monitor on cpu 1, migrate workers start at cpu 48
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DUAL_DIR="$(cd "${SCRIPT_DIR}/../common/dual-workload" && pwd)"
RUN_SH="${DUAL_DIR}/run.sh"
SPEC_WRAPPER="${REPO_ROOT}/experiments/common/spec/run_spec_wrapper.sh"

RESULTS_ROOT="${RESULTS_ROOT:-${SCRIPT_DIR}/results}"
TMP_CONFIG_DIR="${TMP_CONFIG_DIR:-${SCRIPT_DIR}/.generated-configs}"
SPECTRA_DRY_RUN="${SPECTRA_DRY_RUN:-0}"

# Workload paths are supplied by configs/site.conf through scripts/run.sh.
ONEAPI_SETVARS="${ONEAPI_SETVARS:-/opt/intel/oneapi/setvars.sh}"
SPEC_DIR="${SPEC_DIR:-}"
LLAMA_BENCH="${LLAMA_BENCH:-}"
LLAMA_MODEL="${LLAMA_MODEL:-}"
FAISS_FLAT="${FAISS_FLAT:-}"
FAISS_BENCH_SEARCH="${FAISS_BENCH_SEARCH:-}"
FAISS_INDEX="${FAISS_INDEX:-}"
FAISS_QUERIES="${FAISS_QUERIES:-}"
DRAMHIT_BIN="${DRAMHIT_BIN:-}"
VHT_BIN="${VHT_BIN:-}"

# ── Defaults (overridable via env) ──────────────────────────────────
WARMUP_SEC="${WARMUP_SEC:-40}"
NUMA_MEM="${NUMA_MEM:-0}"
MTTM_PAGE_TYPE="${MTTM_PAGE_TYPE:-basepage}"

# Static baseline memory binding (per-workload).
W1_MEM_NODE="${W1_MEM_NODE:-1}"
W2_MEM_NODE="${W2_MEM_NODE:-1}"

# libtiermem worker placement.
TIERMEM_MONITOR_CPU="${TIERMEM_MONITOR_CPU:-1}"
TIERMEM_MIGRATE_CPU_START="${TIERMEM_MIGRATE_CPU_START:-48}"
TIERMEM_OBJ_THRESHOLD="${TIERMEM_OBJ_THRESHOLD:-20M}"
TIERMEM_EPOCH_SEC="${TIERMEM_EPOCH_SEC:-}"
TIERMEM_LOG_LEVEL="${TIERMEM_LOG_LEVEL:-3}"
# libtiermem sampling / migration tunables (empty = library default).
TIERMEM_PEBS_PERIOD="${TIERMEM_PEBS_PERIOD:-1009}"
TIERMEM_STORE_PERIOD="${TIERMEM_STORE_PERIOD:-10007}"
TIERMEM_ALLLOAD_PERIOD="${TIERMEM_ALLLOAD_PERIOD:-100003}"
TIERMEM_OCR_INTERVAL="${TIERMEM_OCR_INTERVAL:-}"
TIERMEM_MIGRATE_THREADS="${TIERMEM_MIGRATE_THREADS:-}"
TIERMEM_HYSTERESIS="${TIERMEM_HYSTERESIS:-}"
TIERMEM_REG_THETA="${TIERMEM_REG_THETA:-}"

# Per-bootstrap-socket starting core (node 1 = 28-55).
BOOTSTRAP_CORE_START=28
# Local-socket starting core post-warmup (leave 0,1 free for monitor workers).
LOCAL_CORE_START=2

BASELINES=(static tpp colloid alto libtiermem memtis mttm)

# ── App registry ────────────────────────────────────────────────────
# Each app_<name> populates the slot-prefixed Wn_* variables in caller
# scope. Args: $1 = slot prefix ("W1" or "W2"), $2 = thread count.
#
# Populated fields:
#   <prefix>_NAME       — short human-readable name
#   <prefix>_CMD        — command array (as a single string literal of a bash array)
#   <prefix>_ENV        — env-var array literal
#   <prefix>_CWD        — working directory (or "")
#   <prefix>_SOURCE     — script to source before launch (or "")
#   <prefix>_TARGET_EXE — process name for per-PID profilers / libtiermem target
#
# Threads are reflected in the command line where the app supports it.

# All app functions store W{slot}_CMD / W{slot}_ENV as *scalar* strings
# containing a bash array literal (e.g. "(arg1 arg2 ...)") so the heredoc in
# write_dual_config can splice them in verbatim.

set_app_field() {
    # $1=variable name (e.g. W1_NAME), $2=value (scalar string)
    printf -v "$1" '%s' "$2"
}

require_executable_path() {
    local label="$1" path="$2"
    [[ -n "${path}" && -x "${path}" ]] || die "${label} is unset or not executable: ${path}"
}

require_file_path() {
    local label="$1" path="$2"
    [[ -n "${path}" && -f "${path}" ]] || die "${label} is unset or not a file: ${path}"
}

app_llama_qwen() {
    local p="$1" t="$2"
    require_executable_path LLAMA_BENCH "${LLAMA_BENCH}"
    require_file_path LLAMA_MODEL "${LLAMA_MODEL}"
    local -a cmd=(
        "${LLAMA_BENCH}"
        -m "${LLAMA_MODEL}"
        -t "${t}" -p 0 -n 64 -r 80 --progress -dio 1 -mmp 0
    )
    local -a env_arr=()
    set_app_field "${p}_NAME"       "llama-qwen"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     "${ONEAPI_SETVARS}"
    set_app_field "${p}_TARGET_EXE" "llama-bench"
}

app_llama_qwen2() {
    local p="$1" t="$2"
    require_executable_path LLAMA_BENCH "${LLAMA_BENCH}"
    require_file_path LLAMA_MODEL "${LLAMA_MODEL}"
    local -a cmd=(
        "${LLAMA_BENCH}"
        -m "${LLAMA_MODEL}"
        -t "${t}" -p 0 -n 64 -r 25 --progress -dio 1 -mmp 0
    )
    local -a env_arr=()
    set_app_field "${p}_NAME"       "llama-qwen"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     "${ONEAPI_SETVARS}"
    set_app_field "${p}_TARGET_EXE" "llama-bench"
}

app_spec_607_cactubssn() {
    local p="$1" t="$2"
    [[ -d "${SPEC_DIR}" ]] || die "SPEC_DIR is unset or not a directory: ${SPEC_DIR}"
    local -a cmd=(
        /usr/bin/bash "${SPEC_WRAPPER}" 607.cactuBSSN_s "${t}"
    )
    local -a env_arr=()
    set_app_field "${p}_NAME"       "spec-607-cactuBSSN-s"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        "${SPEC_DIR}"
    set_app_field "${p}_SOURCE"     ""
    set_app_field "${p}_TARGET_EXE" "cactuBSSN_s_base.gcc13-m64"
}

app_spec_619_lbm() {
    local p="$1" t="$2"
    [[ -d "${SPEC_DIR}" ]] || die "SPEC_DIR is unset or not a directory: ${SPEC_DIR}"
    local -a cmd=(
        /usr/bin/bash "${SPEC_WRAPPER}" 619.lbm_s "${t}"
    )
    local -a env_arr=()
    set_app_field "${p}_NAME"       "spec-619-lbm-s"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        "${SPEC_DIR}"
    set_app_field "${p}_SOURCE"     ""
    set_app_field "${p}_TARGET_EXE" "lbm_s_base.gcc13-m64"
}

app_faiss_flat() {
    local p="$1" t="$2"
    require_executable_path FAISS_FLAT "${FAISS_FLAT}"
    # 1-Flat is hard-coded to its CLI args (dim=64, nb=20M, nq=7760, k=50, nprobe=20).
    # Thread count is controlled via OMP_NUM_THREADS in the env array.
    local -a cmd=(
        "${FAISS_FLAT}"
        64 20000000 7760 50 20
    )
    local -a env_arr=("OMP_NUM_THREADS=${t}")
    set_app_field "${p}_NAME"       "faiss-flat"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     "${ONEAPI_SETVARS}"
    set_app_field "${p}_TARGET_EXE" "1-Flat"
}

app_faiss_bench_polysemous() {
    local p="$1" t="$2"
    require_executable_path FAISS_BENCH_SEARCH "${FAISS_BENCH_SEARCH}"
    require_file_path FAISS_INDEX "${FAISS_INDEX}"
    require_file_path FAISS_QUERIES "${FAISS_QUERIES}"
    local -a cmd=(
        "${FAISS_BENCH_SEARCH}"
        --index "${FAISS_INDEX}"
        --queries "${FAISS_QUERIES}"
        --nthreads "${t}"
        --batch_size 1000 --k 100 --params nprobe=4,ht=102 --max_queries 200000
    )
    local -a env_arr=()
    set_app_field "${p}_NAME"       "faiss-bench-polysemous"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     "${ONEAPI_SETVARS}"
    set_app_field "${p}_TARGET_EXE" "bench_search"
}


app_dramhit() {
    local p="$1" t="$2"
    require_executable_path DRAMHIT_BIN "${DRAMHIT_BIN}"
    local -a cmd=(
	    "${DRAMHIT_BIN}"
	    --ht-type 3 --no-prefetch 0 --mode 14 --num-threads "${t}" --numa-split 0 --insert-factor 1 
	    --ht-fill 90 --hw-pref 1 --drop-caches 0 --ht-size 268435456 
	    --find_queue_sz 64 --read-factor 120 --batch-len 60
    )
    # --ht-size 268435456 --> 5126MiB memory usage
    # --read-factor --> expected execution time ~190 secs

    local -a env_arr=()
    set_app_field "${p}_NAME"       "dramhit"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     ""
    set_app_field "${p}_TARGET_EXE" "dramhit"
}

app_vht() {
    local p="$1" t="$2"
    require_executable_path VHT_BIN "${VHT_BIN}"
    local -a cmd=(
        "${VHT_BIN}"
        --threads "${t}" --log2-size 28 --queries-per-thread-mb 4900 --lf 90
        --pause-before-lookup
    )
    # --queries-per-thread-mb 4900 --> expected execution time ~190 secs

    local -a env_arr=()
    set_app_field "${p}_NAME"       "vht"
    set_app_field "${p}_CMD"        "$(render_bash_array cmd)"
    set_app_field "${p}_ENV"        "$(render_bash_array env_arr)"
    set_app_field "${p}_CWD"        ""
    set_app_field "${p}_SOURCE"     ""
    set_app_field "${p}_TARGET_EXE" "qp-llc-bench"
}

# Map app id → app_<func>.
declare -A APP_FUNC=(
    [llama-qwen]=app_llama_qwen              # 4988MiB
    [spec-607]=app_spec_607_cactubssn        # 6722MiB
    [spec-619]=app_spec_619_lbm              # 3220MiB
    [faiss-flat]=app_faiss_flat              # 4970MiB
    [faiss-bench-polysemous]=app_faiss_bench_polysemous
    [dramhit]=app_dramhit
    [vht]=app_vht
    [llama-qwen2]=app_llama_qwen2
)

# ── Combo table ─────────────────────────────────────────────────────
# Each entry: "id|app1|t1|app2|t2|dram_free_mib|ltm_budget|memtis_mib|mttm_mib"
# - dram_free_mib : used by tpp/colloid/alto baselines (memeater target).
# - ltm_budget    : libtiermem DRAM budget (e.g., 5000M). Empty = skip libtiermem.
# - memtis_mib    : Memtis DRAM cgroup limit in MiB. Empty = skip memtis.
# - mttm_mib      : MTTM local DRAM size in MiB. Empty = skip mttm.
COMBOS=(
    "faissbench-spec607|spec-607|10|faiss-bench-polysemous|16|3800|2400M|2400|2400"
    "llama-faissflat|llama-qwen|16|faiss-flat|8|6400|5000M|5000|5000"
    "spec619-llama|spec-619|16|llama-qwen2|8|4980|3580M|3580|3580"
    "vht-dramhit|vht|4|dramhit|20|6600|5200M|5200|5200"
    "vht-faissbench|vht|4|faiss-bench-polysemous|20|3800|2400M|2400|2400"
)

# ── Logging ─────────────────────────────────────────────────────────
log()  { printf '[suite] %s\n' "$*"; }
die()  { printf '[suite][FATAL] %s\n' "$*" >&2; exit 1; }
warn() { printf '[suite][WARN] %s\n' "$*" >&2; }

# ── Helpers ─────────────────────────────────────────────────────────
render_bash_array() {
    local -n _arr="$1"
    local rendered="("
    local part first=1
    for part in "${_arr[@]}"; do
        if (( first )); then
            first=0
        else
            rendered+=" "
        fi
        rendered+="$(printf '%q' "${part}")"
    done
    rendered+=")"
    printf '%s' "${rendered}"
}

is_known_app() {
    [[ -n "${APP_FUNC[$1]+x}" ]]
}

is_known_baseline() {
    local target="$1" b
    for b in "${BASELINES[@]}"; do
        [[ "${b}" == "${target}" ]] && return 0
    done
    return 1
}

lookup_combo() {
    # Echoes "app1 t1 app2 t2 dram_free_mib ltm_budget memtis_mib mttm_mib"
    local id="$1" entry bid a1 t1 a2 t2 dram ltm mts mttm
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r bid a1 t1 a2 t2 dram ltm mts mttm <<< "${entry}"
        if [[ "${bid}" == "${id}" ]]; then
            printf '%s %s %s %s %s %s %s %s\n' \
                "${a1}" "${t1}" "${a2}" "${t2}" \
                "${dram}" "${ltm}" "${mts}" "${mttm}"
            return 0
        fi
    done
    die "Unknown combo id: ${id}"
}

combo_summary() {
    local entry bid a1 t1 a2 t2 _rest
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r bid a1 t1 a2 t2 _rest <<< "${entry}"
        printf '  %-22s %s(t=%s) + %s(t=%s)\n' "${bid}" "${a1}" "${t1}" "${a2}" "${t2}"
    done
}

# Build "start-end" core range for `nthreads` cores starting at `start`.
core_range() {
    local start="$1" nthreads="$2"
    local end=$(( start + nthreads - 1 ))
    printf '%d-%d\n' "${start}" "${end}"
}

# ── Config writer ───────────────────────────────────────────────────
write_dual_config() {
    # $1=out-file $2=baseline $3=outdir $4=app1 $5=t1 $6=app2 $7=t2
    # $8=dram_free $9=ltm_budget $10=memtis_mib $11=mttm_mib $12=combo-id
    local out="$1" baseline="$2" outdir="$3"
    local a1="$4" t1="$5" a2="$6" t2="$7"
    local dram_default="$8" ltm_default="$9" memtis_default="${10}" mttm_default="${11}"
    local combo_id="${12}"

    # Allow env overrides (uniform across all combos when set).
    local dram_free="${DRAM_FREE_MIB-${dram_default}}"
    local ltm_budget="${TIERMEM_DRAM_BUDGET-${ltm_default}}"
    local memtis_budget="${MEMTIS_DRAM_BUDGET_MIB-${memtis_default}}"
    local mttm_budget="${MTTM_DRAM_MIB-${mttm_default}}"

    # Resolve app slots → W1_*, W2_*.
    is_known_app "${a1}" || die "Unknown app: ${a1}"
    is_known_app "${a2}" || die "Unknown app: ${a2}"
    "${APP_FUNC[$a1]}" W1 "${t1}"
    "${APP_FUNC[$a2]}" W2 "${t2}"

    # Core ranges: pack t1 then t2 starting at BOOTSTRAP_CORE_START / LOCAL_CORE_START.
    local w1_cores w1_cores_after w2_cores w2_cores_after
    w1_cores="$(core_range "${BOOTSTRAP_CORE_START}" "${t1}")"
    w2_cores="$(core_range $(( BOOTSTRAP_CORE_START + t1 )) "${t2}")"
    w1_cores_after="$(core_range "${LOCAL_CORE_START}" "${t1}")"
    w2_cores_after="$(core_range $(( LOCAL_CORE_START + t1 )) "${t2}")"

    # Static baseline: no warmup migration, so share the local socket from the
    # start and leave the *_AFTER fields empty.
    if [[ "${baseline}" == "static" ]]; then
        w1_cores="0-27"
        w2_cores="0-27"
        w1_cores_after=""
        w2_cores_after=""
    fi

    local dram_line="" libtm_block="" mttm_block="" static_mem_binding=""
    case "${baseline}" in
        libtiermem)
            [[ -n "${ltm_budget}" ]] \
                || die "libtiermem requires TIERMEM_DRAM_BUDGET (combo default empty for this combo)"
            dram_line='DRAM_FREE_MIB=""'
            libtm_block=$(cat <<EOF
LIBTIERMEM_PATH="$(realpath "${REPO_ROOT}/src/libtiermem/libtiermem.so")"
TIERMEM_DRAM_BUDGET="${ltm_budget}"
TIERMEM_MONITOR_CPU=${TIERMEM_MONITOR_CPU}
TIERMEM_MIGRATE_CPU_START=${TIERMEM_MIGRATE_CPU_START}
TIERMEM_OBJ_THRESHOLD="${TIERMEM_OBJ_THRESHOLD}"
TIERMEM_EPOCH_SEC="${TIERMEM_EPOCH_SEC}"
TIERMEM_PEBS_PERIOD="${TIERMEM_PEBS_PERIOD}"
TIERMEM_STORE_PERIOD="${TIERMEM_STORE_PERIOD}"
TIERMEM_ALLLOAD_PERIOD="${TIERMEM_ALLLOAD_PERIOD}"
TIERMEM_OCR_INTERVAL="${TIERMEM_OCR_INTERVAL}"
TIERMEM_MIGRATE_THREADS="${TIERMEM_MIGRATE_THREADS}"
TIERMEM_HYSTERESIS="${TIERMEM_HYSTERESIS}"
TIERMEM_REG_THETA="${TIERMEM_REG_THETA}"
TIERMEM_LOG_LEVEL="${TIERMEM_LOG_LEVEL}"
EOF
)
            ;;
        static)
            dram_line='DRAM_FREE_MIB=""'
            static_mem_binding=$(cat <<EOF
W1_MEM_NODE=${W1_MEM_NODE}
W2_MEM_NODE=${W2_MEM_NODE}
EOF
)
            ;;
        tpp|colloid|alto)
            [[ -n "${dram_free}" ]] || die "${baseline} requires DRAM_FREE_MIB"
            dram_line="DRAM_FREE_MIB=\"${dram_free}\""
            ;;
        memtis)
            [[ -n "${memtis_budget}" ]] \
                || die "memtis requires MEMTIS_DRAM_BUDGET_MIB (combo default empty)"
            dram_line="DRAM_FREE_MIB=\"${memtis_budget}\""
            ;;
        mttm)
            [[ -n "${mttm_budget}" ]] \
                || die "mttm requires MTTM_DRAM_MIB (combo default empty)"
            dram_line="DRAM_FREE_MIB=\"${mttm_budget}\""
            mttm_block="MTTM_PAGE_TYPE=\"${MTTM_PAGE_TYPE}\""
            ;;
        *)
            die "write_dual_config: unexpected baseline ${baseline}"
            ;;
    esac

    local enable_vvmstat=1
    case "${baseline}" in
        static|mttm|libtiermem) enable_vvmstat=0 ;;
    esac

    # plots/macro-converge.py reads migration logs only for these TPP pairs.
    local enable_bpftrace=0
    case "${baseline}:${combo_id}" in
        tpp:llama-faissflat|tpp:spec619-llama) enable_bpftrace=1 ;;
    esac

    mkdir -p "$(dirname "${out}")"
    cat > "${out}" <<EOF
#!/usr/bin/env bash
# Auto-generated config for macro-bench suite.
# baseline=${baseline}  app1=${a1}(t=${t1})  app2=${a2}(t=${t2})
# Regenerated each run — do not hand-edit.

BASELINE="${baseline}"
WARMUP_SEC=${WARMUP_SEC}
OUTDIR="${outdir}"
TURBO="on"

NUMA_MEM=${NUMA_MEM}
${dram_line}

${static_mem_binding}
${libtm_block}
${mttm_block}

W1_NAME="${W1_NAME}"
W1_CMD=${W1_CMD}
W1_ENV=${W1_ENV}
W1_CWD="${W1_CWD}"
W1_SOURCE="${W1_SOURCE}"
W1_CORES="${w1_cores}"
W1_CORES_AFTER="${w1_cores_after}"
W1_TARGET_EXE="${W1_TARGET_EXE}"

W2_NAME="${W2_NAME}"
W2_CMD=${W2_CMD}
W2_ENV=${W2_ENV}
W2_CWD="${W2_CWD}"
W2_SOURCE="${W2_SOURCE}"
W2_CORES="${w2_cores}"
W2_CORES_AFTER="${w2_cores_after}"
W2_TARGET_EXE="${W2_TARGET_EXE}"

VVMSTAT_INTERVAL=1
NUMA_MAPS_INTERVAL=2
ENABLE_VVMSTAT=${enable_vvmstat}
ENABLE_NUMA_MAPS=1
ENABLE_BPFTRACE=${enable_bpftrace}

TARGET_PID_TIMEOUT_SEC=60
EOF
}

# ── Single-run driver ───────────────────────────────────────────────
run_one() {
    # $1=baseline $2=combo-id [$3=t1 $4=t2 — override combo defaults]
    local baseline="$1" combo_id="$2"
    local t1_override="${3:-}" t2_override="${4:-}"

    is_known_baseline "${baseline}" \
        || die "Unknown baseline: ${baseline} (valid: ${BASELINES[*]})"

    local app1 t1_default app2 t2_default dram ltm mts mttm
    read -r app1 t1_default app2 t2_default dram ltm mts mttm \
        < <(lookup_combo "${combo_id}")

    local t1="${t1_override:-${t1_default}}"
    local t2="${t2_override:-${t2_default}}"

    # Sanity check: t1+t2 must fit on a single 28-core socket.
    if (( t1 + t2 > 28 )); then
        die "Thread budget exceeded: t1=${t1} + t2=${t2} = $((t1+t2)) > 28 cores/socket on the AE testbed"
    fi

    local outdir="${RESULTS_ROOT}/${combo_id}/${baseline}"
    if (( ! SPECTRA_DRY_RUN )); then
        if [[ -d "${outdir}" ]] && find "${outdir}" -mindepth 1 -print -quit | grep -q .; then
            die "Refusing to overwrite nonempty result directory: ${outdir}"
        fi
        mkdir -p "${outdir}"
    fi

    local config="${TMP_CONFIG_DIR}/${combo_id}-${baseline}.sh"
    mkdir -p "${TMP_CONFIG_DIR}"

    log "=== baseline=${baseline}  combo=${combo_id}  ${app1}(t=${t1}) + ${app2}(t=${t2}) ==="
    log "    outdir=${outdir}"
    log "    config=${config}"

    write_dual_config "${config}" "${baseline}" "${outdir}" \
        "${app1}" "${t1}" "${app2}" "${t2}" \
        "${dram}" "${ltm}" "${mts}" "${mttm}" "${combo_id}"

    if (( SPECTRA_DRY_RUN )); then
        cat "${config}"
        return 0
    fi

    [[ -x "${RUN_SH}" ]] || die "run.sh not executable: ${RUN_SH}"

    # Track kswapd0 for kernel-tiering baselines.
    local pidstat_pid=""
    case "${baseline}" in
        tpp|colloid|alto)
            local kswapd_pid
            kswapd_pid="$(pgrep -x kswapd0 || true)"
            if [[ -n "${kswapd_pid}" ]]; then
                local pidstat_log="${outdir}/kswapd0-pidstat.log"
                log "    tracking kswapd0 (pid=${kswapd_pid}) → ${pidstat_log}"
                pidstat -h -u -p "${kswapd_pid}" 1 > "${pidstat_log}" 2>&1 &
                pidstat_pid=$!
                # shellcheck disable=SC2064
                trap "kill ${pidstat_pid} 2>/dev/null; wait ${pidstat_pid} 2>/dev/null" RETURN
            else
                warn "kswapd0 not found; skipping pidstat for ${baseline}"
            fi
            ;;
    esac

    "${RUN_SH}" "${config}"
}

run_all() {
    local entry bid _rest b
    for entry in "${COMBOS[@]}"; do
        IFS='|' read -r bid _rest <<< "${entry}"
        for b in "${BASELINES[@]}"; do
            run_one "${b}" "${bid}"
            log "Sleeping 15s between runs..."
            sleep 15
        done
    done
}

# ── Pre-flight ──────────────────────────────────────────────────────
if (( ! SPECTRA_DRY_RUN )); then
    [[ "${EUID}" -eq 0 ]] || die "Must run as root (sudo ${BASH_SOURCE[0]} ...)"
fi
[[ -f "${RUN_SH}" ]]       || die "run.sh missing: ${RUN_SH}"
[[ -f "${SPEC_WRAPPER}" ]] || warn "SPEC wrapper missing: ${SPEC_WRAPPER} (SPEC combos will fail)"

# ── CLI ─────────────────────────────────────────────────────────────
case $# in
    0)
        log "Running full suite: ${#BASELINES[@]} baselines × ${#COMBOS[@]} combos = $(( ${#BASELINES[@]} * ${#COMBOS[@]} )) runs"
        log "Baselines: ${BASELINES[*]}"
        log "Combos (with default thread counts):"
        combo_summary | while read -r line; do log "${line}"; done
        run_all
        ;;
    2)
        run_one "$1" "$2"
        ;;
    4)
        run_one "$1" "$2" "$3" "$4"
        ;;
    *)
        cat >&2 <<EOF
Usage:
  sudo $0                                 # run the full baseline × combo matrix
  sudo $0 <baseline> <combo-id>           # one combo with that combo's default thread counts
  sudo $0 <baseline> <combo-id> T1 T2     # one combo with explicit thread counts

Baselines : ${BASELINES[*]}
Combos (id — app1(t1) + app2(t2)):
$(combo_summary)
EOF
        exit 1
        ;;
esac

log "Done."
