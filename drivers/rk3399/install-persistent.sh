#!/usr/bin/env bash
# Reversible persistence installer for ROCK Pi 4C (RK3399) + VEYE RAW-MIPI-IMX462M.
# Preserves stock /boot/Image, /boot/uInitrd, and /boot/dtb/rockchip/rk3399-rock-pi-4c.dtb untouched.
set -euo pipefail
export PATH="/usr/sbin:/usr/bin:/sbin:/bin"

(( EUID == 0 )) || { echo "Run with sudo: sudo $0 [artifacts_dir]" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE="$(uname -r)"
ARTIFACTS="${1:-$SCRIPT_DIR/out/$RELEASE}"

[[ -f "$ARTIFACTS/rockchip-isp1.ko" && -f "$ARTIFACTS/veye_mvcam.ko" && -f "$ARTIFACTS/rockpi4c-veye.dtbo" ]] || {
    echo "Missing built artifacts in $ARTIFACTS. Run ./build.sh first or pass artifact directory." >&2
    exit 1
}

echo "=== [1/3] Installing out-of-tree modules into /lib/modules/$RELEASE/updates ==="
install -d -m 0755 "/lib/modules/$RELEASE/updates"
install -m 0644 "$ARTIFACTS/rockchip-isp1.ko" "/lib/modules/$RELEASE/updates/rockchip-isp1.ko"
install -m 0644 "$ARTIFACTS/veye_mvcam.ko" "/lib/modules/$RELEASE/updates/veye_mvcam.ko"
depmod -a "$RELEASE"

cat > /etc/modules-load.d/veye-csi.conf <<'EOF'
phy_rockchip_dphy_rx0
rockchip_isp1
veye_mvcam
EOF

echo "=== [2/3] Installing user DT overlay into /boot/overlay-user ==="
install -d -m 0755 /boot/overlay-user
install -m 0644 "$ARTIFACTS/rockpi4c-veye.dtbo" /boot/overlay-user/rockpi4c-veye.dtbo

if [[ ! -f /boot/armbianEnv.txt.bak-pre-veye ]]; then
    cp -a /boot/armbianEnv.txt /boot/armbianEnv.txt.bak-pre-veye
fi

if grep -q '^user_overlays=' /boot/armbianEnv.txt; then
    if ! grep -Eq '^user_overlays=.*\brockpi4c-veye\b' /boot/armbianEnv.txt; then
        sed -i 's/^user_overlays=\(.*\)/user_overlays=\1 rockpi4c-veye/' /boot/armbianEnv.txt
    fi
else
    echo "user_overlays=rockpi4c-veye" >> /boot/armbianEnv.txt
fi

echo "=== [3/3] Adding udev rule for i2c-4 video-group access ==="
cat > /etc/udev/rules.d/60-veye-i2c4.rules <<'EOF'
SUBSYSTEM=="i2c-dev", KERNEL=="i2c-4", GROUP="video", MODE="0660"
EOF
udevadm control --reload-rules && udevadm trigger --subsystem-match=i2c-dev || true

echo "Installed! Rollback anytime with: sudo $SCRIPT_DIR/rollback.sh"
