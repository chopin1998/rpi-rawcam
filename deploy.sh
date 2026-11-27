#!/usr/bin/env bash
# One-command setup & deployment for Raspberry Pi (Unicam) and RK3399 / ROCK Pi 4C (RKISP1).
# Usage: ./deploy.sh [--no-service] [--force-driver] [--skip-apt]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_USER="${SUDO_USER:-$USER}"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
KVER="$(uname -r)"

INSTALL_SERVICE=1
FORCE_DRIVER=0
SKIP_APT=0

for arg in "$@"; do
    case "$arg" in
        --no-service)   INSTALL_SERVICE=0 ;;
        --force-driver) FORCE_DRIVER=1 ;;
        --skip-apt)     SKIP_APT=1 ;;
        -h|--help)
            cat <<'EOF'
Usage: ./deploy.sh [options]
  --no-service    Build/install drivers and rawcamd, skip systemd service setup
  --force-driver  Rebuild and reinstall kernel modules even if already installed
  --skip-apt      Skip apt-get dependency installation
EOF
            exit 0
            ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
as_root() { if (( EUID == 0 )); then "$@"; else sudo "$@"; fi; }

# 1. Detect platform
PLATFORM=""
if grep -q 'rockchip,rk3399' /proc/device-tree/compatible 2>/dev/null; then
    PLATFORM="rk3399"
elif grep -q 'raspberrypi,' /proc/device-tree/compatible 2>/dev/null; then
    PLATFORM="rpi"
else
    die "Unsupported board (expected Raspberry Pi or Rockchip RK3399)."
fi
echo "=== Detected platform: $PLATFORM (kernel $KVER) ==="

# 2. Ensure VEYE driver git submodule is present
if [[ ! -f "$REPO_ROOT/drivers/veye_raspberrypi_v4l2/driver_source/cam_drv_src/rpi-6.12.y/veye_mvcam.c" ]]; then
    echo "=== Initializing VEYE driver git submodule ==="
    git -C "$REPO_ROOT" submodule update --init --recursive --depth 1
fi

# 3. Install build & runtime dependencies
if (( SKIP_APT == 0 )) && command -v apt-get >/dev/null 2>&1; then
    PKGS=()
    command -v gcc  >/dev/null 2>&1 || PKGS+=(build-essential)
    command -v dtc  >/dev/null 2>&1 || PKGS+=(device-tree-compiler)
    command -v v4l2-ctl >/dev/null 2>&1 || PKGS+=(v4l-utils)
    command -v patch >/dev/null 2>&1 || PKGS+=(patch)
    command -v curl >/dev/null 2>&1 || PKGS+=(curl)

    if [[ ! -d "/lib/modules/$KVER/build" ]]; then
        if [[ "$PLATFORM" == "rpi" ]]; then
            PKGS+=("linux-headers-$KVER")
        else
            PKGS+=(linux-headers-current-rockchip64)
        fi
    fi

    if (( ${#PKGS[@]} > 0 )); then
        echo "=== Installing dependencies: ${PKGS[*]} ==="
        as_root apt-get update
        as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${PKGS[@]}" || {
            # Fallback package name on older RPi OS
            if [[ "$PLATFORM" == "rpi" && ! -d "/lib/modules/$KVER/build" ]]; then
                as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y raspberrypi-kernel-headers
            else
                die "Failed to install required packages: ${PKGS[*]}"
            fi
        }
    fi
fi

# 4. Build & install kernel driver + DT overlay if needed
if [[ "$PLATFORM" == "rk3399" ]]; then
    NEED_DRV=$FORCE_DRIVER
    if ! modinfo -k "$KVER" veye_mvcam >/dev/null 2>&1 || \
       ! modinfo -k "$KVER" rockchip_isp1 >/dev/null 2>&1 || \
       [[ ! -f /boot/overlay-user/rockpi4c-veye.dtbo ]]; then
        NEED_DRV=1
    fi
    if (( NEED_DRV == 1 )); then
        echo "=== Building and installing RK3399 drivers + DT overlay ==="
        "$REPO_ROOT/drivers/rk3399/build.sh"
        as_root "$REPO_ROOT/drivers/rk3399/install-persistent.sh"
    else
        echo "=== RK3399 drivers and DT overlay already installed for $KVER (use --force-driver to rebuild) ==="
    fi
else
    NEED_DRV=$FORCE_DRIVER
    if ! modinfo -k "$KVER" veye_mvcam >/dev/null 2>&1 || \
       [[ ! -f "$REPO_ROOT/drivers/rpi/out/$KVER/veye_mvcam.dtbo" ]]; then
        NEED_DRV=1
    fi
    if (( NEED_DRV == 1 )); then
        echo "=== Building and installing Raspberry Pi VEYE driver + DT overlay ==="
        "$REPO_ROOT/drivers/rpi/build.sh"
        as_root install -d -m 0755 "/lib/modules/$KVER/updates"
        as_root install -m 0644 "$REPO_ROOT/drivers/rpi/out/$KVER/veye_mvcam.ko" "/lib/modules/$KVER/updates/veye_mvcam.ko"
        as_root depmod -a "$KVER"
    else
        echo "=== Raspberry Pi VEYE driver already installed for $KVER (use --force-driver to rebuild) ==="
    fi
fi

# 5. Permissions for non-root capture & I2C
for grp in video i2c; do
    if getent group "$grp" >/dev/null 2>&1 && ! id -nG "$RUN_USER" | grep -qw "$grp"; then
        echo "=== Adding $RUN_USER to group '$grp' ==="
        as_root usermod -aG "$grp" "$RUN_USER"
    fi
done

# 6. Build pure C11 server (rawcamd)
echo "=== Building rawcamd ==="
make -C "$REPO_ROOT/server"

# 7. Load driver & verify camera presence
echo "=== Loading camera driver ==="
as_root "$REPO_ROOT/server/camera-driver.sh" start

# 8. Install systemd service (optional)
if (( INSTALL_SERVICE == 1 )); then
    echo "=== Installing and enabling rawcam.service ==="
    TMP_SVC="$(mktemp)"
    cat > "$TMP_SVC" <<EOF
[Unit]
Description=RAW Camera Streaming Service (rawcamd)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
Group=video
SupplementaryGroups=i2c
PermissionsStartOnly=true
ExecStartPre=$REPO_ROOT/server/camera-driver.sh start
ExecStart=$REPO_ROOT/server/bin/rawcamd --ctrl-port 5600 --stream-port 5601 --snap-port 5602
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
    # Remove SupplementaryGroups=i2c if the i2c group does not exist on this distro
    if ! getent group i2c >/dev/null 2>&1; then
        sed -i '/^SupplementaryGroups=i2c$/d' "$TMP_SVC"
    fi
    as_root install -m 0644 "$TMP_SVC" /etc/systemd/system/rawcam.service
    rm -f "$TMP_SVC"
    as_root systemctl daemon-reload
    as_root systemctl enable --now rawcam.service
    as_root systemctl --no-pager -l status rawcam.service | head -n 12 || true
fi

echo ""
echo "Deployment complete!"
echo "  Host:     $(hostname).local"
echo "  Ports:    ctrl=5600 stream=5601 snap=5602"
if (( INSTALL_SERVICE == 0 )); then
    echo "  Start:    $REPO_ROOT/server/start.sh"
fi
