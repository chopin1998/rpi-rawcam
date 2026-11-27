# rpi-rawcam

把单板机上的 MIPI RAW10/12 工业相机做成一套局域网采集服务，配一个桌面端 PySide6 + OpenGL 客户端，方便远程对焦、调参和无损抓拍。

目前支持 **Raspberry Pi 4B** 和 **Radxa ROCK Pi 4C（RK3399，Mainline Linux）**，两块板子跑同一份服务端代码，客户端连上去自动识别平台、传感器型号、分辨率、位深和硬件编码能力，不需要手工切配置。

---

## 为什么这么做

- **服务端纯 C11，零第三方库依赖**：直接 `gcc` 编译出几十 KB 的单个二进制 `rawcamd`，板端不用装 OpenCV / FFmpeg / Python 环境。
- **预览走硬件，抓拍拿真 RAW**：
  - 树莓派（Unicam）：预览走片上 VPU 硬编 H.264（`/dev/video11`），带宽低、延迟小；抓拍/无损流直接拿 Unicam DMA 的 `Y10P`/`Y12P`。
  - RK3399（Mainline `rkisp1`）：主线内核没有 `rkvenc` H.264 硬编，预览改走片上 Hantro VPU 硬件 JPEG 编码（`/dev/video3`）做单帧独立 MJPEG，1944×1096 @ 30fps 实测仅 **3.2 MB/s**、服务端总 CPU 占用约 **5%**，调焦无帧间涂抹；无损流与抓拍经 ARM NEON 实时转为标准 `Y10`/`Y12`（千兆网实测 108 MB/s 跑满线速）。
- **预览不断流，随时抓 12-bit 原图**：日常看 H.264 / MJPEG 预览时，点一下 **Snap 12-bit Lossless** 会走独立端口（`5602`）抓当前完整的 10/12-bit 无损帧保存为 16-bit PNG / TIFF / `.raw`。
- **长曝光单次拍摄**：实时流曝光 0.05 ~ 500 ms（超过当前帧周期自动降帧率，调短自动恢复）；需要更长曝光（最高 10 s）时可走 Single Shot 单拍定格 12-bit 原图，拍完后台自动恢复实时流。
- **显卡做解包与拉伸**：PC 端用 PySide6 + OpenGL Shader 直接在 GPU 里解包 `Y10P`/`Y12P`/`Y10`/`Y12`、做黑电平/白电平/Gamma 拉伸、伪彩映射和 1:1 像素缩放，支持点选局部自动曝光/增益（Spot Auto）。
- **可复现的 RK3399 主线驱动方案**：树莓派用官方驱动；RK3399 主线内核的 `veye_mvcam` 加固补丁、`rkisp1` 单色 RAW 旁路补丁和 `rockpi4c-veye.dts` 覆盖层都放在仓库里，不替换原厂内核镜像，支持一键回退。

---

## 硬件与实测情况

目前基于 **VEYE `RAW-MIPI-IMX462M`**（1944×1096 单色 RAW10/12，2-lane 891 Mbps/lane）验证，协议本身不绑死型号，连上后自动读取传感器分辨率、最小 ROI 与位深能力。

| 平台 | 采集后端 | 默认硬件预览 | 实测预览表现（1944×1096） | 无损格式 |
| :--- | :--- | :--- | :--- | :--- |
| **Raspberry Pi 4B** | `unicam-image` | VPU H.264 (`/dev/video11`) | 30–60 fps，低码率，PC 端 VA-API 硬解 | `Y10P` / `Y12P` |
| **ROCK Pi 4C (RK3399)** | `rkisp1_mainpath` | Hantro MJPEG (`/dev/video3`) | 30.4 fps @ 3.17 MB/s（上限 ~46 fps），CPU ~5% | `Y10` / `Y12`（NEON 解包） |

---

## 目录结构

```text
.
├── server/                        # 板端纯 C11 服务 rawcamd（RPi / RK3399 通用）
│   ├── src/                       # V4L2/Media 采集、H.264 & MJPEG 硬编、I2C 控制、TCP 服务
│   ├── camera-driver.sh           # 自动识别平台加载/卸载驱动与设备树覆盖层
│   ├── start.sh                   # 检查驱动 -> 自动编译 -> 启动 rawcamd
│   └── rawcam.service             # systemd 开机自启模板
├── viewer/                        # PC 端 PySide6 + OpenGL 客户端（uv 管理依赖）
└── drivers/
    ├── veye_raspberrypi_v4l2/     # Git Submodule：上游 veyeimaging/raspberrypi_v4l2
    └── rk3399/                    # RK3399 主线内核补丁、DTS、编译/安装/回退脚本
```

网络端口约定：
- `5600`：JSON 控制与状态查询（平台、传感器型号、分辨率、支持位深、支持流模式、曝光/增益/帧率/ROI）
- `5601`：二进制视频流（64 字节 `RAWC` 帧头 + 负载）
- `5602`：按需单帧 10/12-bit 无损快照 & 长曝光单拍

---

## 快速部署

拉取仓库时记得带上驱动 Submodule：

```bash
git clone --recurse-submodules https://github.com/chopin1998/rpi-rawcam.git ~/rpi-rawcam
```

### 1. 板端启动（树莓派 / RK3399 通用）

**RK3399 首次使用**需先编好树外驱动和 DT Overlay（不改原厂内核/DTB，可随时回退）：

```bash
cd ~/rpi-rawcam/drivers/rk3399
./build.sh
sudo ./install-persistent.sh     # 如需恢复原厂状态：sudo ./rollback.sh
```

> RK3399（ROCK Pi 4C）相机接口是 **J8203**（紧邻 Micro-HDMI 和 3.5 mm 耳机口），别插到对称的 J8204 DSI 屏接口上（两者 3.3V/GND 引脚一致，插错也能测到 3.3V 但 I2C4 无响应）。

驱动就绪后，树莓派和 RK3399 启动方式完全一致：

```bash
cd ~/rpi-rawcam/server
make
./start.sh
```

手动查驱动状态：`sudo ./camera-driver.sh status`（支持 `start | stop | status`）。  
需要开机自启的话，按实际用户名和路径改一下 `server/rawcam.service` 装进 systemd：

```bash
sudo cp server/rawcam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rawcam
```

### 2. PC 客户端（Linux）

用 `uv` 安装依赖并启动：

```bash
cd rpi-rawcam/viewer
uv sync
uv run rawcam-viewer
```

顶部输入板子地址（例如 `rp4.local` 或 `rockpi-4c.local`），点 **Connect** 即可：
- 下拉框会自动隐藏当前主机不支持的预览协议（连树莓派显示 H.264，连 RK3399 显示 MJPEG）。
- ROI 预设、范围和可选位深按实机传感器能力自动生成。
- `Shift + 左键` 点击画面任意位置可对该点邻域做 **Spot Auto** 自动调曝光/增益。
- 任意预览模式下点 **Snap 12-bit Lossless** 都会保存真实的 10/12-bit 无损原图。
