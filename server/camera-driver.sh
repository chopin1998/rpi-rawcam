#!/usr/bin/env bash
# Simple loader/unloader for the VEYE MIPI RAW camera kernel module & DT overlay.
# Usage: sudo ./camera-driver.sh {start|stop|status}
set -euo pipefail

export PATH="/usr/sbin:/usr/bin:/sbin:/bin"

OWNER_HOME=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
VEYE_REPO="${VEYE_REPO:-$OWNER_HOME/build/raspberrypi_v4l2}"
MODULE="${VEYE_MODULE:-veye_mvcam}"
MODULE_KO="${VEYE_KO:-$VEYE_REPO/driver_source/cam_drv_src/rpi-6.12.y/$MODULE.ko}"
OVERLAY_DIR="${VEYE_DTS_DIR:-$VEYE_REPO/driver_source/dts/rpi-6.12.y}"
OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"

die() { printf '%s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 && "$1" =~ ^(start|stop|status)$ ]] || die "Usage: sudo $0 {start|stop|status}"
(( EUID == 0 )) || die "Run with sudo: sudo $0 $1"

shopt -s nullglob
overlay_active() { local o=("$OVERLAY_ROOT"/[0-9]*_"$MODULE"); (( ${#o[@]} > 0 )); }
module_loaded()  { [[ -d "/sys/module/$MODULE" ]]; }
camera_present() { local c=(/sys/bus/i2c/devices/*-003b/veye_mvcam); (( ${#c[@]} > 0 )); }

case "$1" in
    start)
        if ! module_loaded; then
            ver=$(modinfo -F vermagic "$MODULE_KO") || die "Cannot read $MODULE_KO"
            [[ "${ver%% *}" == "$(uname -r)" ]] || die "Rebuild $MODULE_KO for kernel $(uname -r) (found $ver)"
            modprobe v4l2-fwnode
            insmod "$MODULE_KO"
        fi
        if ! overlay_active && ! camera_present; then
            dtoverlay -d "$OVERLAY_DIR" "$MODULE"
        fi
        for ((i = 0; i < 30; i++)); do
            camera_present && break
            sleep 0.2
        done
        camera_present || die "Camera did not enumerate (check CSI cable / dmesg)."
        printf 'Camera driver ready (%s).\n' "$(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null || echo "$MODULE")"
        ;;
    stop)
        overlay_active && dtoverlay -r "$MODULE"
        module_loaded  && rmmod "$MODULE"
        printf 'Camera overlay and module unloaded.\n'
        ;;
    status)
        printf 'Module:  %s\n' "$(module_loaded  && echo loaded || echo absent)"
        printf 'Overlay: %s\n' "$(overlay_active && echo loaded || echo absent)"
        if camera_present; then
            printf 'Model:   %s\n' "$(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null || echo unknown)"
        fi
        ;;
esac
