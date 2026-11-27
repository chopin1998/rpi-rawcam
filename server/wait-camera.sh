#!/usr/bin/env bash
# Block until the VEYE camera has enumerated on I2C (0x3b), or time out.
# Used as ExecStartPre by the rawcam user service (no root required).
set -u
timeout_s="${1:-60}"
for ((i = 0; i < timeout_s; i++)); do
    if ls /sys/bus/i2c/devices/*-003b/veye_mvcam >/dev/null 2>&1; then
        exit 0
    fi
    sleep 1
done
echo "wait-camera: VEYE camera did not enumerate within ${timeout_s}s (is the driver/overlay installed? run ./deploy.sh)" >&2
exit 1
