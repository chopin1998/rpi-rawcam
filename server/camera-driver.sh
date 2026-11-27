#!/usr/bin/env bash
# Loader/unloader for VEYE MIPI RAW camera kernel modules & DT overlay.
# Auto-detects Raspberry Pi (Unicam) vs Rockchip RK3399 (RKISP1).
# Usage: sudo ./camera-driver.sh {start|stop|status}
set -euo pipefail

export PATH="/usr/sbin:/usr/bin:/sbin:/bin"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OWNER_HOME=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)

MODULE="${VEYE_MODULE:-veye_mvcam}"
OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"

die() { printf '%s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 && "$1" =~ ^(start|stop|status)$ ]] || die "Usage: sudo $0 {start|stop|status}"
(( EUID == 0 )) || die "Run with sudo: sudo $0 $1"

shopt -s nullglob
module_loaded()  { [[ -d "/sys/module/$MODULE" ]]; }
camera_present() { local c=(/sys/bus/i2c/devices/*-003b/veye_mvcam); (( ${#c[@]} > 0 )); }

is_rk3399() {
    grep -q 'rockchip,rk3399' /proc/device-tree/compatible 2>/dev/null
}

if is_rk3399; then
    RELEASE="$(uname -r)"
    RK_OVERLAY_NAME="veye-rockpi4c"
    RK_OVERLAY_DIR="$OVERLAY_ROOT/$RK_OVERLAY_NAME"

    find_rk_dtbo() {
        local candidates=(
            "${VEYE_DTBO:-}"
            "/boot/overlay-user/rockpi4c-veye.dtbo"
            "$REPO_ROOT/drivers/rk3399/out/$RELEASE/rockpi4c-veye.dtbo"
            "$OWNER_HOME/rockpi-csi-stage/rockpi4c-veye-v2/rockpi4c-veye.dtbo"
        )
        for p in "${candidates[@]}"; do
            [[ -n "$p" && -f "$p" ]] && { printf '%s\n' "$p"; return 0; }
        done
        return 1
    }

    rk_dt_active() {
        local n=(/proc/device-tree/i2c@*/camera@3b)
        (( ${#n[@]} > 0 ))
    }

    case "$1" in
        start)
            for dep in phy-rockchip-dphy-rx0 videobuf2-v4l2 videobuf2-dma-contig videobuf2-vmalloc v4l2-fwnode v4l2-async videodev mc; do
                modprobe "$dep" 2>/dev/null || true
            done

            if [[ ! -d /sys/module/rockchip_isp1 ]]; then
                if modinfo rockchip_isp1 >/dev/null 2>&1; then
                    modprobe rockchip_isp1
                else
                    isp_ko="$REPO_ROOT/drivers/rk3399/out/$RELEASE/rockchip-isp1.ko"
                    [[ -f "$isp_ko" ]] || die "rockchip-isp1.ko not installed; run drivers/rk3399/install-persistent.sh"
                    insmod "$isp_ko"
                fi
            fi

            if ! module_loaded; then
                if modinfo "$MODULE" >/dev/null 2>&1; then
                    modprobe "$MODULE"
                else
                    mv_ko="$REPO_ROOT/drivers/rk3399/out/$RELEASE/$MODULE.ko"
                    [[ -f "$mv_ko" ]] || die "$MODULE.ko not installed; run drivers/rk3399/install-persistent.sh"
                    insmod "$mv_ko"
                fi
            fi

            if ! rk_dt_active && ! camera_present; then
                dtbo=$(find_rk_dtbo) || die "rockpi4c-veye.dtbo not found (run drivers/rk3399/build.sh)"
                mkdir -p "$RK_OVERLAY_DIR"
                cat "$dtbo" > "$RK_OVERLAY_DIR/dtbo"
            fi

            for ((i = 0; i < 30; i++)); do
                camera_present && break
                sleep 0.2
            done
            camera_present || die "RK3399 camera did not enumerate on I2C4 0x3b (check J8203 CSI cable / dmesg)."
            printf 'RK3399 camera driver ready (%s).\n' "$(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null || echo "$MODULE")"
            ;;
        stop)
            # Note: Never rmdir configfs DT overlays at runtime on RK3399 (triggers upstream
            # iommu_bus_notifier oops on platform_device_del). Unbind & unload modules safely.
            if [[ -L /sys/bus/i2c/devices/4-003b/driver ]]; then
                echo 4-003b > /sys/bus/i2c/drivers/mvcam/unbind 2>/dev/null || true
            fi
            module_loaded && rmmod "$MODULE" || true
            [[ -d /sys/module/rockchip_isp1 ]] && rmmod rockchip_isp1 || true
            printf 'RK3399 camera modules unloaded.\n'
            ;;
        status)
            printf 'Platform: RK3399\n'
            printf 'ISP:      %s\n' "$([[ -d /sys/module/rockchip_isp1 ]] && echo loaded || echo absent)"
            printf 'Module:   %s\n' "$(module_loaded && echo loaded || echo absent)"
            printf 'DT Node:  %s\n' "$(rk_dt_active && echo active || echo absent)"
            if camera_present; then
                printf 'Model:    %s\n' "$(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null || echo unknown)"
            fi
            ;;
    esac
    exit 0
fi

# ---- Raspberry Pi (Unicam) path ----
DEFAULT_VEYE_REPO="$OWNER_HOME/build/raspberrypi_v4l2"
if [[ ! -d "$DEFAULT_VEYE_REPO" && -d "$REPO_ROOT/drivers/veye_raspberrypi_v4l2" ]]; then
    DEFAULT_VEYE_REPO="$REPO_ROOT/drivers/veye_raspberrypi_v4l2"
fi
VEYE_REPO="${VEYE_REPO:-$DEFAULT_VEYE_REPO}"
MODULE_KO="${VEYE_KO:-$VEYE_REPO/driver_source/cam_drv_src/rpi-6.12.y/$MODULE.ko}"
OVERLAY_DIR="${VEYE_DTS_DIR:-$VEYE_REPO/driver_source/dts/rpi-6.12.y}"

overlay_active() { local o=("$OVERLAY_ROOT"/[0-9]*_"$MODULE"); (( ${#o[@]} > 0 )); }

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
        printf 'Platform: Raspberry Pi\n'
        printf 'Module:   %s\n' "$(module_loaded  && echo loaded || echo absent)"
        printf 'Overlay:  %s\n' "$(overlay_active && echo loaded || echo absent)"
        if camera_present; then
            printf 'Model:    %s\n' "$(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null || echo unknown)"
        fi
        ;;
esac
