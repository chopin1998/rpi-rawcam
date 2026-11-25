#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAM_HELPER="${CAM_HELPER:-$SCRIPT_DIR/camera-driver.sh}"

# Ensure camera driver & device tree overlay are loaded
if ! ls /sys/bus/i2c/devices/*-003b/veye_mvcam >/dev/null 2>&1; then
    if [[ -x "$CAM_HELPER" ]]; then
        echo "[start.sh] Camera not detected; loading driver via $CAM_HELPER..."
        if (( EUID == 0 )); then
            "$CAM_HELPER" start
        else
            sudo "$CAM_HELPER" start
        fi
    else
        echo "[start.sh] Camera not detected and CAM_HELPER ($CAM_HELPER) not found." >&2
        exit 1
    fi
fi

# Build if binary is missing
if [[ ! -x "$SCRIPT_DIR/bin/rawcamd" ]]; then
    make -C "$SCRIPT_DIR"
fi

exec "$SCRIPT_DIR/bin/rawcamd" "$@"
