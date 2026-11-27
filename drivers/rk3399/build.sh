#!/usr/bin/env bash
# Build out-of-tree veye_mvcam.ko, rockchip-isp1.ko, and rockpi4c-veye.dtbo
# for Radxa ROCK Pi 4C (RK3399) running mainline Linux (e.g. Armbian 6.18.x).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VEYE_SUBMODULE="${VEYE_SUBMODULE:-$SCRIPT_DIR/../veye_raspberrypi_v4l2}"
KVER="${KVER:-$(uname -r)}"
KDIR="${KDIR:-/lib/modules/$KVER/build}"
OUTDIR="${OUTDIR:-$SCRIPT_DIR/out/$KVER}"
WORK="${WORK:-$SCRIPT_DIR/.work-$KVER}"

MAKE_ARGS=(-C "$KDIR")
[[ -n "${ARCH:-}" ]] && MAKE_ARGS+=(ARCH="$ARCH")
[[ -n "${CROSS_COMPILE:-}" ]] && MAKE_ARGS+=(CROSS_COMPILE="$CROSS_COMPILE")

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ -d "$KDIR" ]] || die "Kernel headers directory not found: $KDIR (set KDIR=/path/to/headers)"
[[ -d "$VEYE_SUBMODULE/driver_source/cam_drv_src/rpi-6.12.y" ]] || \
    die "VEYE driver submodule missing. Run: git submodule update --init --recursive"

mkdir -p "$OUTDIR" "$WORK"

echo "=== [1/3] Compiling rockpi4c-veye.dtbo ==="
dtc -@ -I dts -O dtb -o "$OUTDIR/rockpi4c-veye.dtbo" "$SCRIPT_DIR/rockpi4c-veye.dts"

echo "=== [2/3] Building veye_mvcam.ko (with mvcam-v2.patch) ==="
rm -rf "$WORK/mvcam"
mkdir -p "$WORK/mvcam"
cp -a "$VEYE_SUBMODULE/driver_source/cam_drv_src/rpi-6.12.y/"* "$WORK/mvcam/"
patch -d "$WORK/mvcam" -p1 < "$SCRIPT_DIR/mvcam-v2.patch"
cat > "$WORK/mvcam/Makefile" <<'EOF'
obj-m += veye_mvcam.o
EOF
make "${MAKE_ARGS[@]}" M="$WORK/mvcam" modules
cp -a "$WORK/mvcam/veye_mvcam.ko" "$OUTDIR/veye_mvcam.ko"

echo "=== [3/3] Building rockchip-isp1.ko (with rkisp1.patch) ==="
rm -rf "$WORK/rkisp1"
mkdir -p "$WORK/rkisp1"
if [[ -n "${RKISP1_SRC:-}" && -d "$RKISP1_SRC" ]]; then
    cp -a "$RKISP1_SRC/"* "$WORK/rkisp1/"
else
    # Derive upstream tag from KVER (e.g. 6.18.44-current-rockchip64 -> v6.18.44)
    TAG="${LINUX_TAG:-v${KVER%%-*}}"
    BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/media/platform/rockchip/rkisp1"
    for f in Makefile rkisp1-capture.c rkisp1-common.c rkisp1-common.h rkisp1-csi.c \
             rkisp1-debug.c rkisp1-dev.c rkisp1-isp.c rkisp1-params.c rkisp1-regs.h \
             rkisp1-resizer.c rkisp1-stats.c; do
        curl -fsSL "$BASE_URL/$f?h=$TAG" -o "$WORK/rkisp1/$f" || \
            die "Failed to fetch $f for $TAG (set RKISP1_SRC=/path/to/rkisp1 or LINUX_TAG=vX.Y.Z)"
    done
fi
patch -d "$WORK/rkisp1" -p1 < "$SCRIPT_DIR/rkisp1.patch"
make "${MAKE_ARGS[@]}" M="$WORK/rkisp1" modules
cp -a "$WORK/rkisp1/rockchip-isp1.ko" "$OUTDIR/rockchip-isp1.ko"

echo "Build complete:"
ls -lh "$OUTDIR/veye_mvcam.ko" "$OUTDIR/rockchip-isp1.ko" "$OUTDIR/rockpi4c-veye.dtbo"
