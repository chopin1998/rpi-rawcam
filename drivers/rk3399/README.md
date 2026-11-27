# RK3399 (Radxa ROCK Pi 4C) Mainline Linux Driver Support

This directory provides reproducible, reversible out-of-tree kernel modules and a Device Tree Overlay for connecting **VEYE `RAW-MIPI-IMX462M`** (and compatible VEYE RAW-MIPI cameras) to the `J8203` 15-pin 2-lane CSI connector on Radxa ROCK Pi 4C running mainline Linux (tested on Armbian `6.18.44-current-rockchip64`).

## Contents

- `../veye_raspberrypi_v4l2`: Git submodule referencing upstream [veyeimaging/raspberrypi_v4l2](https://github.com/veyeimaging/raspberrypi_v4l2).
- `mvcam-v2.patch`: Adapts upstream `veye_mvcam.c` for mainline 6.18 + RK3399 (startup power-up delay, I²C retry/backoff, `LaneNum_Cap` fallback, optional `xvclk` clock handling).
- `rkisp1.patch`: Adds monochrome `MEDIA_BUS_FMT_Y10_1X10` / `Y12_1X12` (`V4L2_PIX_FMT_Y10` / `V4L2_PIX_FMT_Y12`) bypass support and `V4L2_CID_LINK_FREQ`-based D-PHY configuration to mainline `rockchip-isp1`.
- `rockpi4c-veye.dts`: DT overlay enabling `i2c4` (`camera@3b` on `GPIO1_B5` reset + 24 MHz `CIF_CLKOUTA`), `mipi_dphy_rx0`, `isp0_mmu`, and `isp0` (2-lane CSI-2).
- `build.sh`, `install-persistent.sh`, `rollback.sh`: Build, reversible boot persistence installer, and one-command rollback.

## Usage

```bash
git submodule update --init --recursive

# On-board build (requires matching linux-headers and device-tree-compiler):
./build.sh

# Install reversibly into /lib/modules/$(uname -r)/updates and /boot/overlay-user:
sudo ./install-persistent.sh

# Roll back completely to stock Armbian files:
sudo ./rollback.sh
```
