# rpi-rawcam

用电脑通过局域网看 MIPI 相机画面、调曝光和增益，并保存 10/12 位无损原图。板端用硬件编码预览，桌面端用 OpenGL 显示。

两块板都用 VEYE `RAW-MIPI-IMX462M`（1944 × 1096，单色）做过实测。

| 板子 | 预览硬件编码 | 无损格式 |
| --- | --- | --- |
| Raspberry Pi 4B（Unicam） | Broadcom VPU · H.264 | `Y10P` / `Y12P` |
| ROCK Pi 4C（RK3399 主线内核） | Hantro VPU · MJPEG | `Y10` / `Y12` |

## 硬件加速

- 板端的 C11 服务 `rawcamd` 通过 V4L2 调用硬件编码器。树莓派这套相机的 H.264 预览居中裁到 1920 × 1088，无损原图仍是 1944 × 1096。
- Linux 桌面端用 PyAV 解码。H.264 优先尝试 VA-API 硬解，驱动不支持时用软件解码。MJPEG 在板上硬编，在电脑上逐帧解码。
- 客户端的 OpenGL 着色器直接在 GPU 上解包 RAW10/12，并处理黑白电平、Gamma、伪彩和缩放。

## 板端部署

ROCK Pi 4C 的相机排线接 **J8203**，在 Micro-HDMI 和 3.5 mm 耳机口旁边。相邻的 J8204 是屏幕接口。

用板上的普通账号运行：

```bash
git clone --recurse-submodules https://github.com/chopin1998/rpi-rawcam.git ~/rpi-rawcam
cd ~/rpi-rawcam
./deploy.sh
```

脚本会安装依赖，编译驱动和 `rawcamd`，写入开机设备树配置，并启用 systemd 用户服务。安装时会用到 `sudo`，日常运行无需 root，开机也无需登录。如果刚添加了 `video` 或 `i2c` 组权限，重启后生效。

驱动和开机配置已经装好的板子，可以用 `./deploy.sh --service-only` 更新服务。只想手动运行时，用 `./deploy.sh --no-service` 安装。

## 启动客户端

在 Linux 电脑上进入仓库的 `viewer` 目录，用 `uv` 安装依赖并启动：

```bash
uv sync
uv run rawcam-viewer
```

输入板子的主机名或 IP，例如 `marco-rpi4b.local`，点 **Connect**。客户端会自动读取相机支持的分辨率、ROI 和位深，显示该板支持的预览模式。

- 按住 **Shift** 点击画面，用 **Spot Auto** 按选中的区域调曝光和增益。
- 点 **Snap 12-bit Lossless**，把原始帧保存为 16 位 PNG、TIFF 或 `.raw`。
- 实时预览的曝光范围是 0.05–500 ms。更长曝光用 **Single Shot (12-bit)**，最长 10 秒。拍完可以保存当前画面，或点 **Resume Live** 回到预览。

## 管理服务

```bash
systemctl --user status rawcam
systemctl --user restart rawcam
journalctl --user-unit=rawcam.service -f
```

想在终端里直接看服务输出，先用 `systemctl --user stop rawcam` 停掉后台服务，再运行 `./server/start.sh`。

要卸载驱动，先执行 `systemctl --user disable --now rawcam`，然后运行对应的回退脚本（树莓派用 `sudo ./drivers/rpi/rollback.sh`，ROCK Pi 用 `sudo ./drivers/rk3399/rollback.sh`），最后重启。ROCK Pi 的驱动构建说明见 [RK3399 驱动文档](drivers/rk3399/README.md)。

默认 TCP 端口为 `5600`（控制和状态）、`5601`（视频流）和 `5602`（无损抓拍和长曝光单拍）。
