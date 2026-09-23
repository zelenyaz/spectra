#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--prepare-sources|--build|--install] [all|6.3.0-colloid-alto|5.15.19-htmm|5.15.145-mttm] [source-dir]\n' "$0"
}

action=all
case "${1:-}" in
    --prepare-source|--prepare-sources) action=prepare; shift ;;
    --build) action=build; shift ;;
    --install) action=install; shift ;;
esac

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    usage
    exit 0
fi

if [[ "${1:-}" == --* ]]; then
    usage >&2
    exit 2
fi

case "${1:-all}" in
    -h|--help) usage; exit 0 ;;
    all) kernels=(6.3.0-colloid-alto 5.15.19-htmm 5.15.145-mttm) ;;
    6.3.0-colloid-alto|5.15.19-htmm|5.15.145-mttm) kernels=("$1") ;;
    *) usage >&2; exit 2 ;;
esac

if (($# > 2)); then
    usage >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_dir="${2:-${script_dir}/sources}"
mkdir -p -- "$source_dir"
source_dir="$(cd -- "$source_dir" && pwd -P)"

kernel_installed() {
    local release="$1"
    [[ "$(uname -r)" == "$release" ]] || \
        { [[ -d "/lib/modules/$release" ]] && [[ -f "/boot/vmlinuz-$release" ]]; }
}

apply_patch_if_needed() {
    local target="$1" patch="$2"
    if git -C "$target" apply --reverse --check "$patch" >/dev/null 2>&1; then
        printf '[kernel] patch already applied: %s\n' "$(basename "$patch")"
    elif git -C "$target" apply --check "$patch"; then
        printf '[kernel] applying %s\n' "$(basename "$patch")"
        git -C "$target" apply "$patch"
    else
        printf '[kernel] patch cannot be applied cleanly: %s\n' "$patch" >&2
        exit 1
    fi
}

configure_kernel() {
    local release="$1" kernel_dir="$2"
    local base_config
    base_config="/boot/config-$(uname -r)"
    local config_tool="${kernel_dir}/scripts/config"

    [[ -f "$base_config" ]] || { printf '[kernel] missing base config: %s\n' "$base_config" >&2; exit 1; }
    [[ -x "$config_tool" ]] || { printf '[kernel] missing config tool: %s\n' "$config_tool" >&2; exit 1; }

    printf '[kernel] configuring %s from %s\n' "$release" "$base_config"
    cp -- "$base_config" "${kernel_dir}/.config"
    "$config_tool" --file "${kernel_dir}/.config" \
        --set-str SYSTEM_TRUSTED_KEYS '' \
        --set-str SYSTEM_REVOCATION_KEYS ''

    case "$release" in
        6.3.0-colloid-alto)
            "$config_tool" --file "${kernel_dir}/.config" --set-str LOCALVERSION '-colloid-alto'
            expected='CONFIG_LOCALVERSION="-colloid-alto"'
            ;;
        5.15.19-htmm)
            "$config_tool" --file "${kernel_dir}/.config" \
                --set-str LOCALVERSION '' --disable LOCALVERSION_AUTO --enable HTMM
            expected='CONFIG_HTMM=y'
            ;;
        5.15.145-mttm)
            "$config_tool" --file "${kernel_dir}/.config" \
                --set-str LOCALVERSION '' --disable LOCALVERSION_AUTO --enable MTTM
            expected='CONFIG_MTTM=y'
            ;;
    esac

    make -C "$kernel_dir" olddefconfig

    for setting in "$expected" 'CONFIG_SYSTEM_TRUSTED_KEYS=""' 'CONFIG_SYSTEM_REVOCATION_KEYS=""'; do
        if ! grep -Fqx -- "$setting" "${kernel_dir}/.config"; then
            printf '[kernel] %s was not retained in %s/.config\n' "$setting" "$kernel_dir" >&2
            exit 1
        fi
    done
}

check_colloid_build_packages() {
    local package
    local missing_packages=()
    local required_packages=(
        bc bison build-essential cpio debhelper dpkg-dev dwarves fakeroot
        flex kmod libelf-dev libncurses-dev libssl-dev openssl python3 rsync
        xz-utils zstd
    )

    for package in "${required_packages[@]}"; do
        if ! dpkg-query -W -f='${db:Status-Abbrev}' "$package" 2>/dev/null | grep -Fqx 'ii '; then
            missing_packages+=("$package")
        fi
    done
    if ((${#missing_packages[@]} > 0)); then
        printf '[kernel] missing packages required to build the kernel: %s\n' \
            "${missing_packages[*]}" >&2
        printf '[kernel] install them with: sudo apt-get install %s\n' \
            "${missing_packages[*]}" >&2
        exit 1
    fi
}

build_colloid_kernel() {
    local release="$1" kernel_dir="$2"
    check_colloid_build_packages
    printf '[kernel] building Debian packages for %s\n' "$release"
    (cd -- "$kernel_dir" && make bindeb-pkg -j)
}

install_colloid_kernel() {
    local release="$1" kernel_dir="$2"
    local package_dir files_list filename _ package
    local image_package='' headers_package=''
    package_dir="$(dirname -- "$kernel_dir")"
    files_list="${kernel_dir}/debian/files"

    [[ -f "$files_list" ]] || { printf '[kernel] missing package manifest: %s\n' "$files_list" >&2; exit 1; }
    while read -r filename _; do
        case "$filename" in
            "linux-image-${release}_"*.deb) image_package="${package_dir}/${filename}" ;;
            "linux-headers-${release}_"*.deb) headers_package="${package_dir}/${filename}" ;;
        esac
    done < "$files_list"

    for package in "$image_package" "$headers_package"; do
        [[ -n "$package" && -f "$package" ]] || {
            printf '[kernel] expected image and headers packages for %s were not found\n' "$release" >&2
            exit 1
        }
    done

    printf '[kernel] installing %s and %s\n' "$headers_package" "$image_package"
    sudo dpkg -i -- "$headers_package" "$image_package"
}

build_5_15_kernel() {
    local release="$1" kernel_dir="$2"
    local image='localhost/kernel-5.15-builder:latest'
    local package_dir

    if ! command -v podman >/dev/null 2>&1; then
        printf '[kernel] podman is required for %s; please install Podman and try again\n' "$release" >&2
        exit 1
    fi
    if [[ "$(podman info --format '{{.Host.Security.Rootless}}')" != true ]]; then
        printf '[kernel] rootless podman is required for %s\n' "$release" >&2
        exit 1
    fi

    kernel_dir="$(cd -- "$kernel_dir" && pwd -P)"
    package_dir="${source_dir}/packages/${release}"
    mkdir -p -- "$package_dir"

    printf '[kernel] building %s with rootless podman; packages: %s\n' "$release" "$package_dir"
    podman build --file "${script_dir}/kernel-5.15.Dockerfile" --tag "$image" "$script_dir"
    podman run --rm \
        --volume "${kernel_dir}:/source:ro" \
        --volume "${package_dir}:/output" \
        "$image"
}

install_5_15_kernel() {
    local release="$1"
    local package_dir="${source_dir}/packages/${release}"
    local package image_package='' headers_package=''

    for package in "${package_dir}/linux-image-${release}_"*.deb; do
        [[ -f "$package" ]] || continue
        [[ -z "$image_package" ]] || { printf '[kernel] multiple image packages for %s in %s\n' "$release" "$package_dir" >&2; exit 1; }
        image_package="$package"
    done
    for package in "${package_dir}/linux-headers-${release}_"*.deb; do
        [[ -f "$package" ]] || continue
        [[ -z "$headers_package" ]] || { printf '[kernel] multiple headers packages for %s in %s\n' "$release" "$package_dir" >&2; exit 1; }
        headers_package="$package"
    done
    if [[ -z "$image_package" || -z "$headers_package" ]]; then
        printf '[kernel] expected image and headers packages for %s were not found in %s\n' "$release" "$package_dir" >&2
        exit 1
    fi

    printf '[kernel] installing %s and %s\n' "$headers_package" "$image_package"
    sudo dpkg -i -- "$headers_package" "$image_package"
}

for release in "${kernels[@]}"; do
    if [[ "$action" == all ]] && kernel_installed "$release"; then
        printf '[kernel] %s is already installed; skipping\n' "$release"
        continue
    fi

    case "$release" in
        6.3.0-colloid-alto)
            repository=https://github.com/host-architecture/colloid
            directory=colloid
            kernel_subdir=tpp/linux-6.3
            patches=(colloid-spr-support.patch colloid-alto.patch)
            ;;
        5.15.19-htmm)
            repository=https://github.com/cosmoss-jigu/memtis
            directory=memtis
            kernel_subdir=linux
            patches=(memtis-congc.patch)
            ;;
        5.15.145-mttm)
            repository=https://github.com/casys-kaist/MTTM_ae_EuroSys26
            directory=MTTM_ae_EuroSys26
            kernel_subdir=linux-5.15.145
            patches=(mttm-spr-support.patch)
            ;;
    esac

    target="${source_dir}/${directory}"
    if [[ "$action" == prepare || "$action" == all ]]; then
        if [[ -d "${target}/.git" ]]; then
            printf '[kernel] %s source already exists at %s; skipping download\n' "$release" "$target"
        elif [[ -e "$target" ]]; then
            printf '[kernel] refusing to overwrite existing path: %s\n' "$target" >&2
            exit 1
        else
            command -v git >/dev/null || { printf 'git is required\n' >&2; exit 1; }
            mkdir -p -- "$source_dir"
            printf '[kernel] downloading %s to %s\n' "$release" "$target"
            git clone "$repository" "$target"
        fi

        for patch in "${patches[@]}"; do
            apply_patch_if_needed "$target" "${script_dir}/patches/${patch}"
        done
        configure_kernel "$release" "${target}/${kernel_subdir}"
    elif [[ ! -d "${target}/.git" ]]; then
        printf '[kernel] source is not prepared for %s: %s\n' "$release" "$target" >&2
        printf '[kernel] run %s --prepare-sources %s first\n' "$0" "$release" >&2
        exit 1
    fi

    case "$action:$release" in
        prepare:*)
            printf '[kernel] %s source and config prepared\n' "$release"
            ;;
        build:6.3.0-colloid-alto)
            build_colloid_kernel "$release" "${target}/${kernel_subdir}"
            ;;
        build:*)
            build_5_15_kernel "$release" "${target}/${kernel_subdir}"
            ;;
        install:6.3.0-colloid-alto)
            install_colloid_kernel "$release" "${target}/${kernel_subdir}"
            ;;
        install:*)
            install_5_15_kernel "$release"
            ;;
        all:6.3.0-colloid-alto)
            build_colloid_kernel "$release" "${target}/${kernel_subdir}"
            install_colloid_kernel "$release" "${target}/${kernel_subdir}"
            ;;
        all:*)
            build_5_15_kernel "$release" "${target}/${kernel_subdir}"
            install_5_15_kernel "$release"
            ;;
    esac
done
