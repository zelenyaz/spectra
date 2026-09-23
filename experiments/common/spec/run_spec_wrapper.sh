#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <benchmark> <threads>" >&2
    exit 1
fi

spec_bench="$1"
threads="$2"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../../.." && pwd)"
spec_dir="${SPEC_DIR:-}"
if [[ -z "${spec_dir}" || ! -d "${spec_dir}" ]]; then
    echo "SPEC_DIR is unset or not a directory: ${spec_dir}" >&2
    exit 3
fi

case "${spec_bench}" in
    619.lbm_s)
        program_stdout_name="lbm.out"
        program_stderr_name="lbm.err"
        ;;
    649.fotonik3d_s)
        program_stdout_name="fotonik3d_s.log"
        program_stderr_name="fotonik3d_s.err"
        ;;
    607.cactuBSSN_s)
        program_stdout_name="spec_ref.out"
        program_stderr_name="spec_ref.err"
        ;;
    *)
        echo "unsupported SPEC benchmark: ${spec_bench}" >&2
        exit 2
        ;;
esac

cd "${spec_dir}"
# shellcheck disable=SC1091
source ./shrc

export OMP_NUM_THREADS="${threads}"
export HOOK_ALLOC_BINARY_HINT_ROOT="${repo_root}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

runcpu_stdout="${tmpdir}/runcpu.stdout"
runcpu_stderr="${tmpdir}/runcpu.stderr"

set +e
runcpu \
    --config=gcc-linux-x86-local.cfg \
    --copies=1 \
    --threads="${threads}" \
    --noreportable \
    "${spec_bench}" \
    >"${runcpu_stdout}" \
    2>"${runcpu_stderr}"
runcpu_status=$?
set -e

run_subdir="$(grep -oP "Setting up ${spec_bench} .*?:\\s+\\Krun_[^[:space:]]+" "${runcpu_stdout}" | tail -1 || true)"
if [ -z "${run_subdir}" ]; then
    run_subdir="$(grep -oP "/output/benchspec/CPU/${spec_bench}/run/\\K\\S+" "${runcpu_stdout}" | tail -1 || true)"
fi

run_dir=""
if [ -n "${run_subdir}" ]; then
    run_dir="${spec_dir}/output/benchspec/CPU/${spec_bench}/run/${run_subdir}"
fi

program_stdout_path=""
program_stderr_path=""
if [ -n "${run_dir}" ] && [ -d "${run_dir}" ]; then
    program_stdout_path="${run_dir}/${program_stdout_name}"
    program_stderr_path="${run_dir}/${program_stderr_name}"
fi

if [ -n "${program_stdout_path}" ] && [ -f "${program_stdout_path}" ]; then
    cat "${program_stdout_path}"
else
    cat "${runcpu_stdout}"
fi

if [ -n "${program_stderr_path}" ] && [ -f "${program_stderr_path}" ]; then
    cat "${program_stderr_path}" >&2
else
    cat "${runcpu_stderr}" >&2
fi

exit "${runcpu_status}"
