# rpi-rawcam

把树莓派上的 MIPI RAW10/12 摄像头做成一个轻量的网络发布服务，配合 PC 端 PySide6 软件做远程预览、参数调节和无损抓拍。

核心思路是**尽量把活交给硬件**：树莓派这边用 Unicam DMA 直接拿 MIPI 打包数据，预览时走片上 VPU 硬件 H.264 编码（省带宽、低 CPU），需要保存或做图像分析时随时拿完整的 10/12-bit 无损 RAW；PC 端用 VA-API 硬解 + OpenGL Shader 直接在显卡里解包、拉伸和显示。

> 目前协议和寄存器适配基于 VEYE RAW-MIPI 驱动（已在 IMX462M 模组上验证），结构上不绑定具体型号，后续方便扩展其它传感器。

## 目录结构

- `server/`：树莓派端 C++17 服务（`rawcamd`），无第三方库依赖。
  - `5600` 端口：JSON 参数控制与状态回报（曝光、增益、帧率、ROI、位深、流模式）。
  - `5601` 端口：视频流（硬件 H.264 / 无损 `Y12P`/`Y10P` / 8-bit 预览）。
  - `5602` 端口：按需单帧 10/12-bit 无损抓拍 & 长曝光（>500 ms）单次拍摄。
- `viewer/`：PC 端 PySide6 + OpenGL 客户端（`rawcam-viewer`）。

## 部署

### 1. 树莓派端

确保摄像头驱动已加载（`start.sh` 默认会检查并调用 `~/build/imx462-camera.sh start`，也可以通过环境变量 `CAM_HELPER` 指定自己的加载脚本）：

```bash
git clone https://github.com/chopin1998/rpi-rawcam.git ~/rpi-rawcam
cd ~/rpi-rawcam/server
make
./start.sh
```

如果需要开机自启，可以把 `server/rawcam.service` 改好路径后装进 systemd：

```bash
sudo cp server/rawcam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rawcam
```

### 2. 远程电脑端（Linux / Python 3.11+）

使用 `uv` 安装依赖并运行：

```bash
git clone https://github.com/chopin1998/rpi-rawcam.git
cd rpi-rawcam/viewer
uv sync
uv run rawcam-viewer
```

## 使用方法

1. 启动 `rawcam-viewer`，顶部填入树莓派 IP，点击 **Connect**。
2. **流模式切换**：
   - `Hardware H.264 Preview`（默认）：RPi VPU 硬编 + 本机 VA-API 硬解，带宽低、帧率高，适合日常观察和调焦。
   - `Lossless RAW (MIPI Packed)`：连续推送完整 10/12-bit 无损数据，本机 GPU Shader 实时解包。
3. **曝光 / 增益 / 自适应帧率**：
   - 实时流曝光范围 `0.05 ~ 500 ms`，曝光变长超过当前帧周期时会自动降低帧率，调短后自动恢复到目标帧率。
   - **Spot Auto**：按住 `Shift + 左键` 点击画面任意位置（或点按钮后点击），程序会按该点附近邻域亮度自动迭代调整曝光或增益。
4. **长曝光单次拍摄（> 500 ms）**：
   - 在 **Long Exposure Single Shot** 面板设置曝光（最高 `10000 ms`）和增益，点 **Single Shot (12-bit)**。拍完会定格显示 12-bit 无损原图，树莓派后台自动恢复实时流；点 **Resume Live** 切回实时画面。
5. **无损抓拍保存**：
   - 无论当前处于 H.264 预览还是长曝光定格，点击顶部 **Snap 12-bit Lossless** 都会保存真实的 10/12-bit 无损图像（支持 16-bit PNG、16-bit TIFF、原始 `.raw`）。
