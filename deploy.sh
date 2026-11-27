#!/usr/bin/env bash
# One-command setup for Raspberry Pi (Unicam) and RK3399 / ROCK Pi 4C (RKISP1).
# Usage: ./deploy.sh [--no-service] [--service-only] [--force-driver] [--skip-apt]
set -euo pipefail
# modinfo / depmod / dtc live in sbin, which is not on a normal user's PATH
export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_USER="${SUDO_USER:-$USER}"
KVER="$(uname -r)"

INSTALL_SERVICE=1
SERVICE_ONLY=0
FORCE_DRIVER=0
SKIP_APT=0

for arg in "$@"; do
    case "$arg" in
        --no-service)   INSTALL_SERVICE=0 ;;
        --service-only) SERVICE_ONLY=1 ;;
        --force-driver) FORCE_DRIVER=1 ;;
        --skip-apt)     SKIP_APT=1 ;;
        -h|--help)
            cat <<'EOF'
Usage: ./deploy.sh [options]
  --no-service    Install drivers and build rawcamd, but do not set up the systemd user service
  --service-only  (Re)install the systemd user service for an already configured camera
                  May need sudo to enable linger or disable an old system service
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
camera_present() { ls /sys/bus/i2c/devices/*-003b/veye_mvcam >/dev/null 2>&1; }

disable_legacy_service() {
    if ! systemctl is-active --quiet rawcam.service &&
       ! systemctl is-enabled --quiet rawcam.service; then
        return
    fi

    local fragment
    fragment=$(systemctl show rawcam.service -p FragmentPath --value)
    if [[ "$fragment" != /etc/systemd/system/rawcam.service ]] ||
       ! grep -qE '^ExecStartPre=/.*/server/camera-driver\.sh start$' "$fragment"; then
        die "A system-level rawcam.service is running or enabled. Review it before starting the user service."
    fi

    echo "=== Disabling the legacy rawcam system service ==="
    as_root systemctl disable --now rawcam.service
}

install_user_service() {
    [[ -x "$REPO_ROOT/server/bin/rawcamd" ]] || make -C "$REPO_ROOT/server"
    local unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    local unit_root="$REPO_ROOT"
    # Escape the checkout path for a quoted systemd argument and a sed replacement.
    unit_root=${unit_root//%/%%}
    unit_root=${unit_root//\\/\\\\}
    unit_root=${unit_root//\"/\\\"}
    unit_root=${unit_root//\\/\\\\}
    unit_root=${unit_root//&/\\&}
    unit_root=${unit_root//|/\\|}
    mkdir -p "$unit_dir"
    sed "s|@REPO_ROOT@|$unit_root|g" "$REPO_ROOT/server/rawcam.service" > "$unit_dir/rawcam.service"

    # Let the user unit start at boot without an interactive login
    if [[ "$(loginctl show-user "$RUN_USER" -p Linger --value 2>/dev/null)" != "yes" ]]; then
        loginctl enable-linger "$RUN_USER" 2>/dev/null || as_root loginctl enable-linger "$RUN_USER"
    fi

    systemctl --user daemon-reload
    disable_legacy_service
    systemctl --user enable rawcam.service
    systemctl --user restart rawcam.service
    echo "Manage with:  systemctl --user {status|restart|stop} rawcam"
    echo "Logs:         journalctl --user-unit=rawcam.service -f"
}

if (( INSTALL_SERVICE == 1 || SERVICE_ONLY == 1 )); then
    (( EUID != 0 )) || die "Run deploy.sh from the account that will run rawcam; the script invokes sudo when needed."
fi

if (( SERVICE_ONLY == 1 )); then
    install_user_service
    exit 0
fi

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
            if [[ "$PLATFORM" == "rpi" && ! -d "/lib/modules/$KVER/build" ]]; then
                as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y raspberrypi-kernel-headers
            else
                die "Failed to install required packages: ${PKGS[*]}"
            fi
        }
    fi
fi

# 4. Build & install kernel driver + DT overlay (persistent across reboots)
if [[ "$PLATFORM" == "rk3399" ]]; then
    NEED_DRV=$FORCE_DRIVER
    if ! modinfo -k "$KVER" veye_mvcam >/dev/null 2>&1 || \
       [[ ! -f "/lib/modules/$KVER/updates/rockchip-isp1.ko" ||
          ! -f /etc/modules-load.d/veye-csi.conf ||
          ! -f /boot/overlay-user/rockpi4c-veye.dtbo ]] || \
       ! grep -qE '^user_overlays=([[:blank:]]*|.*[[:blank:]])rockpi4c-veye([[:blank:]]|$)' /boot/armbianEnv.txt; then
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
    FW=/boot/firmware; [[ -d $FW ]] || FW=/boot
    NEED_DRV=$FORCE_DRIVER
    if ! modinfo -k "$KVER" veye_mvcam >/dev/null 2>&1 || \
       [[ ! -f "$FW/overlays/veye_mvcam.dtbo" ]] || \
       ! grep -qE '^dtoverlay=veye_mvcam(,|[[:blank:]]*$)' "$FW/config.txt"; then
        NEED_DRV=1
    fi
    if (( NEED_DRV == 1 )); then
        echo "=== Building and installing Raspberry Pi VEYE driver + DT overlay ==="
        "$REPO_ROOT/drivers/rpi/build.sh"
        OUT="$REPO_ROOT/drivers/rpi/out/$KVER"
        as_root install -d -m 0755 "/lib/modules/$KVER/updates"
        as_root install -m 0644 "$OUT/veye_mvcam.ko" "/lib/modules/$KVER/updates/veye_mvcam.ko"
        as_root depmod -a "$KVER"
        as_root install -m 0644 "$OUT/veye_mvcam.dtbo" "$FW/overlays/veye_mvcam.dtbo"
        if ! grep -qE '^dtoverlay=veye_mvcam(,|[[:blank:]]*$)' "$FW/config.txt"; then
            [[ -f "$FW/config.txt.bak-pre-veye" ]] || as_root cp -a "$FW/config.txt" "$FW/config.txt.bak-pre-veye"
            printf '\n[all]\ndtoverlay=veye_mvcam\n' | as_root tee -a "$FW/config.txt" >/dev/null
            echo "=== Added 'dtoverlay=veye_mvcam' to $FW/config.txt (backup: config.txt.bak-pre-veye) ==="
        fi
    else
        echo "=== Raspberry Pi VEYE driver already installed for $KVER (use --force-driver to rebuild) ==="
    fi
fi

# 5. Permissions for non-root capture & I2C
for grp in video i2c; do
    if getent group "$grp" >/dev/null 2>&1 && ! id -nG "$RUN_USER" | grep -qw "$grp"; then
        echo "=== Adding $RUN_USER to group '$grp' (takes effect on next login) ==="
        as_root usermod -aG "$grp" "$RUN_USER"
    fi
done

# 6. Build pure C11 server (rawcamd)
echo "=== Building rawcamd ==="
make -C "$REPO_ROOT/server"

# 7. Make sure the camera is up right now (no reboot needed)
if camera_present; then
    echo "=== Camera already enumerated: $(cat /sys/bus/i2c/devices/*-003b/veye_mvcam/camera_model 2>/dev/null) ==="
else
    echo "=== Loading camera driver ==="
    as_root "$REPO_ROOT/server/camera-driver.sh" start
fi

# 8. systemd user service
if (( INSTALL_SERVICE == 1 )); then
    echo "=== Installing rawcam user service ==="
    install_user_service
fi

echo ""
echo "Deployment complete!  Host: $(hostname).local   Ports: ctrl=5600 stream=5601 snap=5602"
(( INSTALL_SERVICE == 1 )) || echo "Run in foreground: $REPO_ROOT/server/start.sh"
