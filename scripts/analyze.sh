#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

extract_data() {
    local target="$1" data="${ROOT}/results/generated/data"
    case "${target}" in
        microbench|congestion|macrobench|all) ;;
        *) printf 'Usage: %s extract [microbench|congestion|macrobench|all]\n' "$0" >&2; return 2 ;;
    esac

    mkdir -p "${data}"
    export SPECTRA_DATA_DIR="${data}"
    export SPECTRA_MICRO_RESULTS="${ROOT}/results/generated/raw/microbench/results"
    export SPECTRA_CONGC_RESULTS="${ROOT}/results/generated/raw/congestion"
    export SPECTRA_MACRO_RESULTS="${ROOT}/results/generated/raw/macrobench/results"

    if [[ "${target}" == microbench || "${target}" == all ]]; then
        python3 "${ROOT}/experiments/microbench/scripts/collect_summary.py" \
            "${ROOT}/results/generated/raw/microbench" \
            -o "${data}/eval-micro.md"
        python3 "${ROOT}/experiments/microbench/scripts/extract_eval_data.py"
    fi

    if [[ "${target}" == congestion || "${target}" == all ]]; then
        python3 "${ROOT}/experiments/microbench/scripts/extract_congc.py"
    fi

    if [[ "${target}" == macrobench || "${target}" == all ]]; then
        python3 "${ROOT}/experiments/macrobench/scripts/collect_duration_summary.py" \
            "${ROOT}/results/generated/raw/macrobench" \
            -o "${data}/macrobench-duration-summary.md"
        python3 "${ROOT}/experiments/macrobench/scripts/extract_placement.py"
        python3 "${ROOT}/experiments/macrobench/scripts/extract_cpu_overhead.py"
    fi

    printf '[analyze] processed data in %s\n' "${data}"
}

mode="${1:-generated}"
if [[ "${mode}" != extract && $# -gt 1 ]]; then
    printf 'Usage: %s [generated|extract [microbench|congestion|macrobench|all]]\n' "$0" >&2
    exit 2
fi
case "${mode}" in
    extract)
        [[ $# -le 2 ]] || { printf 'Usage: %s extract [microbench|congestion|macrobench|all]\n' "$0" >&2; exit 2; }
        extract_data "${2:-all}"
        exit 0
        ;;
    generated)
        macro_results="${ROOT}/results/generated/raw/macrobench/results"
        macro_run_complete() {
            local run_dir="$1" pair base workload
            for pair in llama-faissflat spec619-llama; do
                for base in tpp colloid alto memtis mttm libtiermem; do
                    [[ -s "${run_dir}/${pair}/${base}/w1/numa_maps.log" ]] || return 1
                done
                for workload in w1 w2; do
                    [[ -s "${run_dir}/${pair}/tpp/${workload}/migrate_pages.log" ]] || return 1
                done
            done
        }
        migrate_run="${SPECTRA_MIGRATE_RUN_ID:-${SPECTRA_MACRO_RUN_ID:-}}"
        if [[ -z "${migrate_run}" ]]; then
            latest_num=-1
            for candidate in "${macro_results}"/run*; do
                candidate_id="${candidate##*/}"
                [[ "${candidate_id}" =~ ^run[0-9]+$ ]] || continue
                candidate_num=$((10#${candidate_id#run}))
                if (( candidate_num > latest_num )) && macro_run_complete "${candidate}"; then
                    migrate_run="${candidate_id}"
                    latest_num="${candidate_num}"
                fi
            done
        fi
        if [[ -z "${migrate_run}" ]] || ! macro_run_complete "${macro_results}/${migrate_run}"; then
            printf 'No complete macro convergence run under %s\n' "${macro_results}" >&2
            exit 1
        fi
        export SPECTRA_MACRO_RUN_ID="${migrate_run}"
        export SPECTRA_MIGRATE_ROOT="${macro_results}/${migrate_run}"
        extract_data all
        data_dir="${ROOT}/results/generated/data"
        ;;
    *) printf 'Usage: %s [generated|extract [microbench|congestion|macrobench|all]]\n' "$0" >&2; exit 2 ;;
esac

[[ -d "${data_dir}" ]] || { printf 'data directory missing: %s\n' "${data_dir}" >&2; exit 1; }
out_dir="${ROOT}/results/generated/figures"
mkdir -p "${out_dir}"
export MPLCONFIGDIR="${ROOT}/results/generated/.mplconfig"
mkdir -p "${MPLCONFIGDIR}"

export SPECTRA_DATA_DIR="${data_dir}"
export SPECTRA_FIG_DIR="${out_dir}"
for plot in \
    microbench.py microbench-bw.py congc.py macrobench-per-workload.py \
    macro-converge.py cpu-overhead.py; do
    printf '[analyze] %s\n' "${plot}"
    python3 "${ROOT}/plots/${plot}"
done

printf '[analyze] generated figures in %s\n' "${out_dir}"
