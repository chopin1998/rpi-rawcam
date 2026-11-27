#!/usr/bin/env bash
# Undo what deploy.sh installed for the VEYE camera on Raspberry Pi.
set -euo pipefail
export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"
(( EUID == 0 )) || { echo "Run with sudo: sudo $0" >&2; exit 1; }

KVER="$(uname -r)"
FW=/boot/firmware; [[ -d $FW ]] || FW=/boot

# Keep unrelated firmware settings, including edits made after deployment.
sed -i '/^dtoverlay=veye_mvcam\(,.*\)\?[[:blank:]]*$/d' "$FW/config.txt"
rm -f "$FW/overlays/veye_mvcam.dtbo" "/lib/modules/$KVER/updates/veye_mvcam.ko"
depmod -a "$KVER"
rmmod veye_mvcam 2>/dev/null || true
echo "Rollback done. Reboot to fully drop the overlay."
