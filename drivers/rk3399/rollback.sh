#!/usr/bin/env bash
# Complete rollback to stock Armbian state on ROCK Pi 4C (RK3399).
set -euo pipefail
export PATH="/usr/sbin:/usr/bin:/sbin:/bin"

(( EUID == 0 )) || { echo "Run with sudo: sudo $0" >&2; exit 1; }

RELEASE="$(uname -r)"

if [[ -f /boot/armbianEnv.txt.bak-pre-veye ]]; then
    cp -a /boot/armbianEnv.txt.bak-pre-veye /boot/armbianEnv.txt
    echo "Restored /boot/armbianEnv.txt from backup."
else
    sed -i '/^user_overlays=rockpi4c-veye$/d' /boot/armbianEnv.txt 2>/dev/null || true
    sed -i 's/\<rockpi4c-veye\>//g; s/^user_overlays=[[:space:]]*$//g' /boot/armbianEnv.txt 2>/dev/null || true
fi

rm -f /boot/overlay-user/rockpi4c-veye.dtbo
rm -f "/lib/modules/$RELEASE/updates/rockchip-isp1.ko" "/lib/modules/$RELEASE/updates/veye_mvcam.ko"
rm -f /etc/modules-load.d/veye-csi.conf
rm -f /etc/udev/rules.d/60-veye-i2c4.rules
depmod -a "$RELEASE"

# Note: Do not rmdir configfs DT overlays at runtime on RK3399 (triggers upstream
# iommu_bus_notifier oops on platform_device_del).
if [[ -L /sys/bus/i2c/devices/4-003b/driver ]]; then
    echo 4-003b > /sys/bus/i2c/drivers/mvcam/unbind 2>/dev/null || true
fi
rmmod veye_mvcam 2>/dev/null || true
rmmod rockchip_isp1 2>/dev/null || true

echo "Rollback completed. Stock boot files and module tree are clean."
