#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s -l | -s <kernel-release>\n' "$0" >&2
}

list_kernels() {
    dpkg-query -W -f='${Status}\t${binary:Package}\n' 'linux-image-[0-9]*' 2>/dev/null |
        awk -F '\t' '$1 == "install ok installed" && $2 !~ /-dbg$/ {
            sub(/^linux-image-/, "", $2)
            print $2
        }' |
        sort -V
}

switch_kernel() {
    local release="$1"
    local grub_config=/etc/default/grub
    local cmdline=''
    local tmp

    if [[ ! "$release" =~ ^[A-Za-z0-9][A-Za-z0-9.+_-]*$ ]]; then
        printf 'Invalid kernel release: %s\n' "$release" >&2
        return 1
    fi
    if [[ "$(dpkg-query -W -f='${Status}' "linux-image-$release" 2>/dev/null || true)" != 'install ok installed' ]]; then
        printf 'Kernel image is not installed: linux-image-%s\n' "$release" >&2
        return 1
    fi
    if [[ ! -f "/boot/vmlinuz-$release" ]]; then
        printf 'Kernel image is missing: /boot/vmlinuz-%s\n' "$release" >&2
        return 1
    fi
    if (( EUID != 0 )); then
        printf 'Switching kernels requires root privileges.\n' >&2
        return 1
    fi
    if [[ ! -f "$grub_config" ]]; then
        printf 'GRUB configuration is missing: %s\n' "$grub_config" >&2
        return 1
    fi
    if ! command -v update-grub >/dev/null 2>&1; then
        printf 'update-grub is required.\n' >&2
        return 1
    fi

    if [[ "$release" == 5.15.145-mttm ]]; then
        cmdline='systemd.unified_cgroup_hierarchy=0'
    fi

    tmp="$(mktemp "${grub_config}.XXXXXX")"
    trap 'rm -f -- "$tmp"' EXIT
    cp -p -- "$grub_config" "$tmp"
    awk -v default_line="GRUB_DEFAULT=\"Advanced options for Ubuntu>Ubuntu, with Linux $release\"" \
        -v cmdline_line="GRUB_CMDLINE_LINUX_DEFAULT=\"$cmdline\"" '
        /^GRUB_DEFAULT=/ {
            if (!default_seen++) print default_line
            next
        }
        /^GRUB_CMDLINE_LINUX_DEFAULT=/ {
            if (!cmdline_seen++) print cmdline_line
            next
        }
        { print }
        END {
            if (!default_seen) print default_line
            if (!cmdline_seen) print cmdline_line
        }
    ' "$grub_config" > "$tmp"
    mv -- "$tmp" "$grub_config"
    trap - EXIT

    update-grub
    printf 'GRUB default kernel set to %s. Reboot to use it.\n' "$release"
}

case "${1:-}" in
    -l)
        if (($# != 1)); then
            usage
            exit 2
        fi
        list_kernels
        ;;
    -s)
        if (($# != 2)); then
            usage
            exit 2
        fi
        switch_kernel "$2"
        ;;
    *)
        usage
        exit 2
        ;;
esac
