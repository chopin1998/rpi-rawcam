#!/usr/bin/env bash
# Build out-of-tree veye_mvcam.ko and veye_mvcam.dtbo for Raspberry Pi
# without dirtying the drivers/veye_raspberrypi_v4l2 git submodule.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VEYE_SUBMODULE="${VEYE_SUBMODULE:-$SCRIPT_DIR/../veye_raspberrypi_v4l2}"
KVER="${KVER:-$(uname -r)}"
KDIR="${KDIR:-/lib/modules/$KVER/build}"
OUTDIR="${OUTDIR:-$SCRIPT_DIR/out/$KVER}"
WORK="${WORK:-$SCRIPT_DIR/.work-$KVER}"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ -d "$KDIR" ]] || die "Kernel headers not found: $KDIR (install linux-headers-$(uname -r) or raspberrypi-kernel-headers)"
[[ -d "$VEYE_SUBMODULE/driver_source/cam_drv_src" ]] || \
    die "VEYE submodule missing. Run: git submodule update --init --recursive"

# Pick matching VEYE source branch for running kernel
MAJOR_MINOR="$(printf '%s' "$KVER" | sed -nE 's/^([0-9]+\.[0-9]+).*/\1/p')"
case "$MAJOR_MINOR" in
    6.1)  DRV_SERIES="rpi-6.1.y";  DTS_SERIES="rpi-6.1.y-bookworm" ;;
    6.6)  DRV_SERIES="rpi-6.6.y";  DTS_SERIES="rpi-6.6.y" ;;
    *)    DRV_SERIES="rpi-6.12.y"; DTS_SERIES="rpi-6.12.y" ;;
esac

SRC_DIR="$VEYE_SUBMODULE/driver_source/cam_drv_src/$DRV_SERIES"
DTS_DIR="$VEYE_SUBMODULE/driver_source/dts/$DTS_SERIES"
[[ -d "$SRC_DIR" ]] || die "Driver source directory not found: $SRC_DIR"
[[ -d "$DTS_DIR" ]] || DTS_DIR="$VEYE_SUBMODULE/driver_source/dts/rpi-6.12.y"

mkdir -p "$OUTDIR" "$WORK"

echo "=== [1/2] Building veye_mvcam.ko ($DRV_SERIES for $KVER) ==="
rm -rf "$WORK/mvcam"
mkdir -p "$WORK/mvcam"
cp -a "$SRC_DIR/veye_mvcam.c" "$SRC_DIR/veye_mvcam.h" "$WORK/mvcam/"
cat > "$WORK/mvcam/Makefile" <<'EOF'
obj-m += veye_mvcam.o
EOF
make -C "$KDIR" M="$WORK/mvcam" modules
cp -a "$WORK/mvcam/veye_mvcam.ko" "$OUTDIR/veye_mvcam.ko"

echo "=== [2/2] Compiling veye_mvcam.dtbo ($DTS_SERIES) ==="
if [[ -f "$DTS_DIR/veye_mvcam-overlay.dts" ]]; then
    dtc -@ -I dts -O dtb -o "$OUTDIR/veye_mvcam.dtbo" "$DTS_DIR/veye_mvcam-overlay.dts"
elif [[ -f "$DTS_DIR/veye_mvcam.dtbo" ]]; then
    cp -a "$DTS_DIR/veye_mvcam.dtbo" "$OUTDIR/veye_mvcam.dtbo"
else
    die "veye_mvcam overlay DTS/DTBO not found in $DTS_DIR"
fi

echo "Build complete:"
ls -lh "$OUTDIR/veye_mvcam.ko" "$OUTDIR/veye_mvcam.dtbo"
