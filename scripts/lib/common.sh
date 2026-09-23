#!/usr/bin/env bash

set -o pipefail

SPECTRA_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

spectra_die() {
    printf '[spectra][FATAL] %s\n' "$*" >&2
    exit 1
}

spectra_info() {
    printf '[spectra] %s\n' "$*"
}

spectra_warn() {
    printf '[spectra][WARN] %s\n' "$*" >&2
}

spectra_load_config() {
    local config="${SPECTRA_CONFIG:-${SPECTRA_ROOT}/configs/site.conf}"
    [[ -f "${config}" ]] || spectra_die \
        "Site config not found. Copy configs/site.example.conf to configs/site.conf."
    # shellcheck source=/dev/null
    source "${config}"
    export SPECTRA_CONFIG="${config}"
}

spectra_require_root() {
    [[ "${EUID}" -eq 0 ]] || spectra_die "This command requires root; rerun with sudo --preserve-env=SPECTRA_CONFIG."
}

spectra_require_empty_dir() {
    local dir="$1"
    if [[ -d "${dir}" ]] && find "${dir}" -mindepth 1 -print -quit | grep -q .; then
        spectra_die "Result directory is not empty: ${dir}. Set a new SPECTRA_RUN_ID."
    fi
    mkdir -p "${dir}"
}

