# rpi-rawcam

rpi-rawcam 是一套 C/S 架构的 MIPI RAW 相机采集软件。服务端 `rawcamd` 跑在单板机上，负责采集图像、编码预览和控制相机。Linux 桌面客户端通过局域网看实时画面、调曝光和增益、设置 ROI，并把原图保存到电脑。

## 亮点

1. **两块板共用一个客户端。** 树莓派 4B 用 Broadcom VPU 硬编 H.264，ROCK Pi 4C 用 Hantro VPU 硬编 MJPEG。客户端连接后自动识别板子和相机，操作方式一样。
2. **看预览时也能抓完整的 RAW10/12。** 点一下 **Snap 12-bit Lossless**，就能从相机取一张无损原图。两块板均用单色 VEYE `RAW-MIPI-IMX462M` 实测，全幅抓拍是 1944 × 1096，树莓派的 H.264 预览为 1920 × 1088。
3. **桌面端也用硬件加速。** H.264 优先走 VA-API 硬解，驱动不支持时切到软件解码。OpenGL 着色器在 GPU 上解包 RAW10/12，做黑白电平调整、Gamma、伪彩和缩放。

## 部署和使用

### 1. 在板上安装服务端

ROCK Pi 4C 的相机排线接 Micro-HDMI 和耳机口旁边的 **J8203**。接好后用板上的普通账号运行：

```bash
git clone --recurse-submodules https://github.com/chopin1998/rpi-rawcam.git ~/rpi-rawcam
cd ~/rpi-rawcam
./deploy.sh
```

脚本会安装驱动、配置开机设备树、编译 `rawcamd` 并启用 systemd 用户服务。安装时会用到 `sudo`，配置好后开机无需登录。如果脚本刚添加了 `video` 或 `i2c` 组权限，重启后生效。

<details>
<summary>更新服务、查日志和回退</summary>

- 驱动和开机配置已装好时，用 `./deploy.sh --service-only` 更新服务。只想手动运行，可以用 `./deploy.sh --no-service` 安装。
- 用 `systemctl --user status rawcam` 查看状态，`systemctl --user restart rawcam` 重启服务，`journalctl --user-unit=rawcam.service -f` 跟踪日志。
- 前台调试时，先用 `systemctl --user stop rawcam` 停掉后台服务，再运行 `./server/start.sh`。
- 回退时，先执行 `systemctl --user disable --now rawcam`，再运行 `sudo ./drivers/rpi/rollback.sh`（树莓派）或 `sudo ./drivers/rk3399/rollback.sh`（ROCK Pi），最后重启。 [RK3399 驱动文档](drivers/rk3399/README.md)

</details>

### 2. 在电脑上启动客户端

在 Linux 电脑上也拉取本仓库，进入 `viewer` 目录，用 `uv` 安装依赖并启动：

```bash
uv sync
uv run rawcam-viewer
```

### 3. 连接相机并采集

输入板子的主机名或 IP，点 **Connect**。看着预览调焦，按住 **Shift** 点击画面可按选中的区域自动调曝光和增益。

点 **Snap 12-bit Lossless** 可保存为 16 位 PNG、TIFF 或 `.raw`。曝光超过 500 ms 时用 **Single Shot (12-bit)**，最长 10 秒。拍完可保存当前画面，或者点 **Resume Live** 回到预览。

默认 TCP 端口是 `5600`（控制）、`5601`（预览和无损流）、`5602`（抓拍和长曝光单拍）。
