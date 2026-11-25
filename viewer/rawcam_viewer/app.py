"""PySide6 main application window for RAW10/12 remote viewer & controller."""

from __future__ import annotations

import datetime
import math
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtGui import QSurfaceFormat
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSlider,
    QSpinBox,
    QStatusBar,
    QVBoxLayout,
    QWidget,
)

from .client import (
    ControlClient,
    ControlWorker,
    LongShotWorker,
    StreamReceiver,
    grab_lossless_snapshot,
)
from .gl_canvas import RawGLCanvas
from .protocol import FramePacket, PixelFormat, save_frame, unpack_to_u16

DARK_STYLESHEET = """
QMainWindow, QWidget {
    background-color: #16181d;
    color: #e4e7ec;
    font-size: 11pt;
}
QGroupBox {
    border: 1px solid #2b303b;
    border-radius: 6px;
    margin-top: 8px;
    padding: 8px 8px 6px 8px;
    font-weight: 600;
    color: #a8b3cf;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background-color: #1f232b;
    border: 1px solid #323846;
    border-radius: 4px;
    padding: 3px 6px;
    color: #f0f3f8;
}
QPushButton {
    background-color: #283040;
    border: 1px solid #3a4559;
    border-radius: 4px;
    padding: 4px 10px;
    color: #eef2f7;
}
QPushButton:hover {
    background-color: #354157;
}
QPushButton:pressed {
    background-color: #1f2735;
}
QPushButton#primaryBtn {
    background-color: #2563eb;
    border-color: #3b82f6;
    font-weight: 600;
}
QPushButton#primaryBtn:hover {
    background-color: #1d4ed8;
}
QStatusBar {
    background-color: #111317;
    color: #98a2b3;
    border-top: 1px solid #262a33;
}
"""


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("RAWCam Viewer (GPU Accelerated)")
        self.resize(1420, 860)

        self._ctrl = ControlClient()
        self._ctrl_worker: ControlWorker | None = None
        self._stream: StreamReceiver | None = None
        self._long_shot_worker: LongShotWorker | None = None
        self._showing_still: bool = False
        self._still_frame: FramePacket | None = None
        self._latest_frame: FramePacket | None = None
        self._last_status: dict[str, Any] = {}
        self._updating_ui = False

        # One-shot Spot Auto state machine
        self._spot_job: dict[str, Any] | None = None
        self._spot_overlay_timer = QTimer(self)
        self._spot_overlay_timer.setSingleShot(True)
        self._spot_overlay_timer.setInterval(3000)
        self._spot_overlay_timer.timeout.connect(lambda: self.canvas.clear_spot_overlay())

        self._build_ui()
        self._fit_default_window_size()

        # Debounced timers so dragging sliders doesn't flood I2C
        self._exp_timer = QTimer(self)
        self._exp_timer.setSingleShot(True)
        self._exp_timer.setInterval(80)
        self._exp_timer.timeout.connect(self._send_exposure)

        self._gain_timer = QTimer(self)
        self._gain_timer.setSingleShot(True)
        self._gain_timer.setInterval(80)
        self._gain_timer.timeout.connect(self._send_gain)

        self._fps_timer = QTimer(self)
        self._fps_timer.setSingleShot(True)
        self._fps_timer.setInterval(80)
        self._fps_timer.timeout.connect(self._send_fps)

        # Periodic telemetry refresh (1 Hz)
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(1000)
        self._poll_timer.timeout.connect(self._poll_status)

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 8, 8, 6)
        root.setSpacing(6)

        # --- Top connection bar ---
        top_bar = QHBoxLayout()
        top_bar.setSpacing(8)

        top_bar.addWidget(QLabel("RPi IP:"))
        self.host_edit = QLineEdit("192.168.110.122")
        self.host_edit.setFixedWidth(140)
        top_bar.addWidget(self.host_edit)

        top_bar.addWidget(QLabel("Ctrl:"))
        self.ctrl_port_spin = QSpinBox()
        self.ctrl_port_spin.setRange(1, 65535)
        self.ctrl_port_spin.setValue(5600)
        self.ctrl_port_spin.setFixedWidth(72)
        top_bar.addWidget(self.ctrl_port_spin)

        top_bar.addWidget(QLabel("Stream:"))
        self.stream_port_spin = QSpinBox()
        self.stream_port_spin.setRange(1, 65535)
        self.stream_port_spin.setValue(5601)
        self.stream_port_spin.setFixedWidth(72)
        top_bar.addWidget(self.stream_port_spin)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("primaryBtn")
        self.connect_btn.clicked.connect(self._toggle_connect)
        top_bar.addWidget(self.connect_btn)

        top_bar.addSpacing(12)
        top_bar.addWidget(QLabel("Stream Mode:"))
        self.stream_mode_combo = QComboBox()
        self.stream_mode_combo.addItem("Hardware H.264 Preview", "h264")
        self.stream_mode_combo.addItem("Lossless RAW (MIPI Packed)", "lossless")
        self.stream_mode_combo.addItem("8-bit Uncompressed Preview", "preview8")
        self.stream_mode_combo.currentIndexChanged.connect(self._on_stream_mode_changed)
        top_bar.addWidget(self.stream_mode_combo)

        self.save_btn = QPushButton("Snap 12-bit Lossless")
        self.save_btn.setToolTip("Fetches a genuine full-resolution 10/12-bit lossless RAW frame from RPi (port 5602)")
        self.save_btn.clicked.connect(self._save_current_frame)
        top_bar.addWidget(self.save_btn)

        top_bar.addStretch()
        self.fit_btn = QPushButton("Fit")
        self.fit_btn.clicked.connect(lambda: self.canvas.reset_view())
        top_bar.addWidget(self.fit_btn)

        self.one_to_one_btn = QPushButton("1:1")
        self.one_to_one_btn.setToolTip("Left-click: 1:1 native size (resizes window) | Right-click: 2:1 / 4:1 / 1:2 presets")
        self.one_to_one_btn.clicked.connect(lambda: self._apply_scale_preset(1.0))
        self.one_to_one_btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.one_to_one_btn.customContextMenuRequested.connect(
            lambda pos: self._show_scale_menu(self.one_to_one_btn.mapToGlobal(pos))
        )
        top_bar.addWidget(self.one_to_one_btn)

        root.addLayout(top_bar)

        # --- Main body: GPU Canvas + Right Controls ---
        body = QHBoxLayout()
        body.setSpacing(8)

        self.canvas = RawGLCanvas()
        self.canvas.spot_clicked.connect(self._on_spot_clicked)
        self.canvas.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.canvas.customContextMenuRequested.connect(
            lambda pos: self._show_scale_menu(self.canvas.mapToGlobal(pos))
        )
        body.addWidget(self.canvas, stretch=1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFixedWidth(400)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        self._ctrl_scroll = scroll

        panel = QWidget()
        self._ctrl_panel = panel
        pv = QVBoxLayout(panel)
        pv.setContentsMargins(2, 0, 6, 2)
        pv.setSpacing(5)

        # 0. Spot Auto (One-Shot) Group
        spot_grp = QGroupBox("Spot Auto (One-Shot)")
        spot_form = QFormLayout(spot_grp)
        spot_form.setVerticalSpacing(4)
        spot_form.setContentsMargins(6, 4, 6, 4)

        self.spot_pick_btn = QPushButton("Click Image to Auto-Tune (Shift+Click)")
        self.spot_pick_btn.setCheckable(True)
        self.spot_pick_btn.toggled.connect(self._on_spot_pick_toggled)
        spot_form.addRow(self.spot_pick_btn)

        spot_row = QHBoxLayout()
        self.spot_target_combo = QComboBox()
        self.spot_target_combo.addItem("Exposure", "exposure")
        self.spot_target_combo.addItem("Gain", "gain")
        self.spot_target_combo.addItem("Exp → Gain", "exp_then_gain")
        self.spot_pct_spin = QSpinBox()
        self.spot_pct_spin.setRange(5, 95)
        self.spot_pct_spin.setValue(50)
        self.spot_pct_spin.setSuffix("%")
        self.spot_rad_spin = QSpinBox()
        self.spot_rad_spin.setRange(4, 128)
        self.spot_rad_spin.setValue(32)
        self.spot_rad_spin.setSuffix("px")
        spot_row.addWidget(self.spot_target_combo, stretch=1)
        spot_row.addWidget(self.spot_pct_spin)
        spot_row.addWidget(QLabel("R:"))
        spot_row.addWidget(self.spot_rad_spin)
        spot_form.addRow(spot_row)

        self.spot_info_lbl = QLabel("Idle — Shift+Click any point")
        self.spot_info_lbl.setStyleSheet("color: #98a2b3; font-size: 9.5pt;")
        self.spot_info_lbl.setWordWrap(True)
        spot_form.addRow(self.spot_info_lbl)
        pv.addWidget(spot_grp)

        # 1. Live Video Exposure Group (<= 500 ms for smooth video streaming)
        exp_grp = QGroupBox("Live Exposure (≤ 500 ms)")
        exp_form = QFormLayout(exp_grp)
        exp_form.setVerticalSpacing(4)
        exp_form.setContentsMargins(6, 4, 6, 4)

        exp_row = QHBoxLayout()
        self.exp_ms_spin = QDoubleSpinBox()
        self.exp_ms_spin.setRange(0.05, 500.0)
        self.exp_ms_spin.setDecimals(2)
        self.exp_ms_spin.setSingleStep(1.0)
        self.exp_ms_spin.setSuffix(" ms")
        self.exp_ms_spin.setValue(10.0)
        self.exp_ms_spin.valueChanged.connect(self._on_exp_spin_changed)
        self.actual_exp_lbl = QLabel("HW: —")
        self.actual_exp_lbl.setStyleSheet("color: #98a2b3; font-size: 9.5pt;")
        exp_row.addWidget(self.exp_ms_spin)
        exp_row.addWidget(self.actual_exp_lbl, stretch=1)
        exp_form.addRow(exp_row)

        # Log-scaled slider: 0..1000 maps to 0.05 ms .. 500 ms
        self.exp_slider = QSlider(Qt.Orientation.Horizontal)
        self.exp_slider.setRange(0, 1000)
        self.exp_slider.setValue(self._ms_to_slider(10.0))
        self.exp_slider.valueChanged.connect(self._on_exp_slider_changed)
        exp_form.addRow(self.exp_slider)
        pv.addWidget(exp_grp)

        # 1b. Long Exposure Single Shot Group (> 500 ms, lossless 12-bit still)
        long_grp = QGroupBox("Long Exposure Single Shot (> 500 ms)")
        long_form = QFormLayout(long_grp)
        long_form.setVerticalSpacing(4)
        long_form.setContentsMargins(6, 4, 6, 4)

        long_row = QHBoxLayout()
        self.long_exp_spin = QDoubleSpinBox()
        self.long_exp_spin.setRange(500.0, 10000.0)
        self.long_exp_spin.setDecimals(0)
        self.long_exp_spin.setSingleStep(100.0)
        self.long_exp_spin.setSuffix(" ms")
        self.long_exp_spin.setValue(1000.0)
        self.long_gain_spin = QDoubleSpinBox()
        self.long_gain_spin.setRange(0.0, 48.0)
        self.long_gain_spin.setDecimals(1)
        self.long_gain_spin.setSingleStep(1.0)
        self.long_gain_spin.setSuffix(" dB")
        self.long_gain_spin.setValue(0.0)
        long_row.addWidget(self.long_exp_spin)
        long_row.addWidget(self.long_gain_spin)
        long_form.addRow(long_row)

        long_btns = QHBoxLayout()
        self.long_shot_btn = QPushButton("Single Shot (12-bit)")
        self.long_shot_btn.setObjectName("primaryBtn")
        self.long_shot_btn.clicked.connect(self._trigger_long_shot)
        self.resume_live_btn = QPushButton("Resume Live")
        self.resume_live_btn.setEnabled(False)
        self.resume_live_btn.clicked.connect(self._resume_live_view)
        long_btns.addWidget(self.long_shot_btn)
        long_btns.addWidget(self.resume_live_btn)
        long_form.addRow(long_btns)

        self.long_shot_lbl = QLabel("12-bit lossless still without slowing live stream")
        self.long_shot_lbl.setStyleSheet("color: #98a2b3; font-size: 9.5pt;")
        self.long_shot_lbl.setWordWrap(True)
        long_form.addRow(self.long_shot_lbl)
        pv.addWidget(long_grp)

        # 2. Adaptive Frame Rate Group
        fps_grp = QGroupBox("Frame Rate (Adaptive)")
        fps_form = QFormLayout(fps_grp)
        fps_form.setVerticalSpacing(4)
        fps_form.setContentsMargins(6, 4, 6, 4)

        fps_row = QHBoxLayout()
        self.fps_spin = QDoubleSpinBox()
        self.fps_spin.setRange(0.5, 120.0)
        self.fps_spin.setDecimals(1)
        self.fps_spin.setSingleStep(1.0)
        self.fps_spin.setSuffix(" fps")
        self.fps_spin.setValue(30.0)
        self.fps_spin.valueChanged.connect(self._on_fps_spin_changed)
        self.eff_fps_lbl = QLabel("Eff: —")
        self.eff_fps_lbl.setStyleSheet("color: #98a2b3; font-size: 9.5pt;")
        fps_row.addWidget(self.fps_spin)
        fps_row.addWidget(self.eff_fps_lbl, stretch=1)
        fps_form.addRow(fps_row)

        self.fps_slider = QSlider(Qt.Orientation.Horizontal)
        self.fps_slider.setRange(1, 60)
        self.fps_slider.setValue(30)
        self.fps_slider.valueChanged.connect(self._on_fps_slider_changed)
        fps_form.addRow(self.fps_slider)

        self.fps_clamp_lbl = QLabel("")
        self.fps_clamp_lbl.setStyleSheet("color: #f59e0b; font-size: 9.5pt;")
        self.fps_clamp_lbl.setWordWrap(True)
        self.fps_clamp_lbl.setVisible(False)
        fps_form.addRow(self.fps_clamp_lbl)
        pv.addWidget(fps_grp)

        # 3. Gain Group (Hardware is manual-only on VEYE RAW-MIPI)
        gain_grp = QGroupBox("Gain (Manual)")
        gain_form = QFormLayout(gain_grp)
        gain_form.setVerticalSpacing(4)
        gain_form.setContentsMargins(6, 4, 6, 4)

        gain_row = QHBoxLayout()
        self.gain_spin = QDoubleSpinBox()
        self.gain_spin.setRange(0.0, 48.0)
        self.gain_spin.setDecimals(1)
        self.gain_spin.setSingleStep(0.5)
        self.gain_spin.setSuffix(" dB")
        self.gain_spin.valueChanged.connect(self._on_gain_spin_changed)
        self.actual_gain_lbl = QLabel("HW: —")
        self.actual_gain_lbl.setStyleSheet("color: #98a2b3; font-size: 9.5pt;")
        gain_row.addWidget(self.gain_spin)
        gain_row.addWidget(self.actual_gain_lbl, stretch=1)
        gain_form.addRow(gain_row)

        self.gain_slider = QSlider(Qt.Orientation.Horizontal)
        self.gain_slider.setRange(0, 480)
        self.gain_slider.valueChanged.connect(self._on_gain_slider_changed)
        gain_form.addRow(self.gain_slider)
        pv.addWidget(gain_grp)

        # 4. Format & ROI Group
        fmt_grp = QGroupBox("Format & ROI")
        fmt_form = QFormLayout(fmt_grp)
        fmt_form.setVerticalSpacing(4)
        fmt_form.setContentsMargins(6, 4, 6, 4)

        fmt_top = QHBoxLayout()
        self.bit_depth_combo = QComboBox()
        self.bit_depth_combo.addItem("RAW12", 12)
        self.bit_depth_combo.addItem("RAW10", 10)
        self.roi_preset_combo = QComboBox()
        self.roi_preset_combo.addItem("Full (1944×1096)", (0, 0, 1944, 1096))
        self.roi_preset_combo.addItem("1080p (1920×1080)", (8, 8, 1920, 1080))
        self.roi_preset_combo.addItem("720p (1280×720)", (328, 188, 1280, 720))
        self.roi_preset_combo.addItem("VGA (640×480)", (648, 308, 640, 480))
        self.roi_preset_combo.currentIndexChanged.connect(self._apply_roi_preset)
        fmt_top.addWidget(self.bit_depth_combo)
        fmt_top.addWidget(self.roi_preset_combo, stretch=1)
        fmt_form.addRow(fmt_top)

        roi_xy = QHBoxLayout()
        self.roi_x_spin = QSpinBox()
        self.roi_x_spin.setRange(0, 1576)
        self.roi_x_spin.setSingleStep(8)
        self.roi_y_spin = QSpinBox()
        self.roi_y_spin.setRange(0, 792)
        self.roi_y_spin.setSingleStep(4)
        self.roi_w_spin = QSpinBox()
        self.roi_w_spin.setRange(368, 1944)
        self.roi_w_spin.setSingleStep(8)
        self.roi_w_spin.setValue(1944)
        self.roi_h_spin = QSpinBox()
        self.roi_h_spin.setRange(304, 1096)
        self.roi_h_spin.setSingleStep(4)
        self.roi_h_spin.setValue(1096)
        roi_xy.addWidget(QLabel("X:"))
        roi_xy.addWidget(self.roi_x_spin)
        roi_xy.addWidget(QLabel("Y:"))
        roi_xy.addWidget(self.roi_y_spin)
        fmt_form.addRow(roi_xy)

        roi_wh = QHBoxLayout()
        roi_wh.addWidget(QLabel("W:"))
        roi_wh.addWidget(self.roi_w_spin)
        roi_wh.addWidget(QLabel("H:"))
        roi_wh.addWidget(self.roi_h_spin)
        self.apply_fmt_btn = QPushButton("Apply")
        self.apply_fmt_btn.clicked.connect(self._send_format)
        roi_wh.addWidget(self.apply_fmt_btn)
        fmt_form.addRow(roi_wh)
        pv.addWidget(fmt_grp)

        # 5. GPU Display Stretch Group
        disp_grp = QGroupBox("GPU Display Stretch")
        disp_form = QFormLayout(disp_grp)
        disp_form.setVerticalSpacing(4)
        disp_form.setContentsMargins(6, 4, 6, 4)

        bw_row = QHBoxLayout()
        self.black_spin = QSpinBox()
        self.black_spin.setRange(0, 4094)
        self.black_spin.setValue(0)
        self.black_spin.valueChanged.connect(self._update_display_params)
        self.white_spin = QSpinBox()
        self.white_spin.setRange(1, 4095)
        self.white_spin.setValue(4095)
        self.white_spin.valueChanged.connect(self._update_display_params)
        bw_row.addWidget(QLabel("Black:"))
        bw_row.addWidget(self.black_spin, stretch=1)
        bw_row.addWidget(QLabel("White:"))
        bw_row.addWidget(self.white_spin, stretch=1)
        disp_form.addRow(bw_row)

        gc_row = QHBoxLayout()
        self.gamma_spin = QDoubleSpinBox()
        self.gamma_spin.setRange(0.2, 4.0)
        self.gamma_spin.setSingleStep(0.1)
        self.gamma_spin.setValue(1.0)
        self.gamma_spin.valueChanged.connect(self._update_display_params)
        self.cmap_combo = QComboBox()
        self.cmap_combo.addItem("Grayscale", 0)
        self.cmap_combo.addItem("Inferno", 1)
        self.cmap_combo.addItem("Viridis", 2)
        self.cmap_combo.addItem("Jet", 3)
        self.cmap_combo.currentIndexChanged.connect(self._update_display_params)
        gc_row.addWidget(QLabel("γ:"))
        gc_row.addWidget(self.gamma_spin)
        gc_row.addWidget(self.cmap_combo, stretch=1)
        disp_form.addRow(gc_row)

        stretch_btns = QHBoxLayout()
        self.auto_stretch_btn = QPushButton("Auto Stretch")
        self.auto_stretch_btn.clicked.connect(self._auto_stretch)
        self.reset_stretch_btn = QPushButton("Reset")
        self.reset_stretch_btn.clicked.connect(self._reset_stretch)
        stretch_btns.addWidget(self.auto_stretch_btn)
        stretch_btns.addWidget(self.reset_stretch_btn)
        disp_form.addRow(stretch_btns)

        pv.addWidget(disp_grp)
        pv.addStretch()

        scroll.setWidget(panel)
        body.addWidget(scroll)
        root.addLayout(body, stretch=1)

        # Status bar
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self.status_lbl = QLabel("Disconnected")
        self.stats_lbl = QLabel("")
        self.status_bar.addWidget(self.status_lbl, stretch=1)
        self.status_bar.addPermanentWidget(self.stats_lbl)

    def _show_scale_menu(self, global_pos: QPoint) -> None:
        menu = QMenu(self)
        menu.addAction("Fit Window", lambda: self.canvas.reset_view())
        menu.addSeparator()
        for label, scale in [
            ("0.5× (50%)", 0.5),
            ("0.75× (75%)", 0.75),
            ("1:1 (100% Native)", 1.0),
            ("1.25× (125%)", 1.25),
            ("1.5× (150%)", 1.5),
            ("2× (200%)", 2.0),
            ("3× (300%)", 3.0),
            ("4× (400%)", 4.0),
        ]:
            menu.addAction(label, lambda _checked=False, s=scale: self._apply_scale_preset(s))
        menu.addSeparator()
        menu.addAction("Custom Scale...", self._prompt_custom_scale)
        menu.exec(global_pos)

    def _prompt_custom_scale(self) -> None:
        val, ok = QInputDialog.getDouble(
            self, "Custom Scale", "Pixel scale (e.g. 1.25, 1.5, 2.5):", 1.5, 0.25, 16.0, 2
        )
        if ok:
            self._apply_scale_preset(val)

    def _apply_scale_preset(self, scale: float) -> None:
        """Resize the window to match the requested pixel scale (clamped to screen),
        then set exact GPU pixel scale so 1:1 fills the canvas at native resolution."""
        img_w, img_h = self.canvas.image_size
        if img_w <= 0 or img_h <= 0:
            return
        dpr = max(self.canvas.devicePixelRatioF(), 1.0)
        desired_cw = int(round((img_w * scale) / dpr))
        desired_ch = int(round((img_h * scale) / dpr))

        chrome_w = max(self.width() - self.canvas.width(), self._ctrl_scroll.width() + 24)
        chrome_h = max(self.height() - self.canvas.height(), 80)
        min_panel_h = self._ctrl_panel.sizeHint().height() + chrome_h

        target_w = desired_cw + chrome_w
        target_h = desired_ch + chrome_h

        screen = self.screen() or QApplication.primaryScreen()
        if screen is not None:
            avail = screen.availableGeometry()
            max_w = int(avail.width() * 0.96)
            max_h = int(avail.height() * 0.96)
            target_w = min(target_w, max_w)
            target_h = min(max(target_h, min(min_panel_h, max_h)), max_h)

        self.resize(max(640, target_w), max(480, target_h))
        # Apply exact pixel scale after layout updates canvas geometry
        QTimer.singleShot(0, lambda s=scale: self.canvas.set_pixel_scale(s))

    def _fit_default_window_size(self) -> None:
        """Size the window so the right control panel fits without a vertical scrollbar
        and the canvas area starts close to the sensor's 16:9 aspect ratio."""
        self._ctrl_panel.adjustSize()
        panel_h = self._ctrl_panel.sizeHint().height()
        # Top toolbar + status bar + layout margins ≈ 92px
        target_h = panel_h + 96
        canvas_w = int(round((target_h - 96) * (1944.0 / 1096.0)))
        target_w = canvas_w + self._ctrl_scroll.width() + 32

        screen = QApplication.primaryScreen()
        if screen is not None:
            avail = screen.availableGeometry()
            target_h = min(target_h, int(avail.height() * 0.94))
            target_w = min(target_w, int(avail.width() * 0.94))
        self.resize(max(1280, target_w), max(780, target_h))

    # --- Log slider mapping for exposure (0.05 ms .. 500 ms) ---
    @staticmethod
    def _ms_to_slider(ms: float) -> int:
        ms = max(0.05, min(500.0, ms))
        return int(round(1000.0 * np.log(ms / 0.05) / np.log(500.0 / 0.05)))

    @staticmethod
    def _slider_to_ms(val: int) -> float:
        return float(0.05 * ((500.0 / 0.05) ** (val / 1000.0)))

    def _on_exp_spin_changed(self, ms: float) -> None:
        if self._updating_ui:
            return
        self._updating_ui = True
        self.exp_slider.setValue(self._ms_to_slider(ms))
        self._updating_ui = False
        self._exp_timer.start()

    def _on_exp_slider_changed(self, val: int) -> None:
        if self._updating_ui:
            return
        ms = self._slider_to_ms(val)
        self._updating_ui = True
        self.exp_ms_spin.setValue(round(ms, 2))
        self._updating_ui = False
        self._exp_timer.start()

    def _on_fps_spin_changed(self, fps: float) -> None:
        if self._updating_ui:
            return
        self._updating_ui = True
        self.fps_slider.setValue(int(round(fps)))
        self._updating_ui = False
        self._fps_timer.start()

    def _on_fps_slider_changed(self, val: int) -> None:
        if self._updating_ui:
            return
        self._updating_ui = True
        self.fps_spin.setValue(float(val))
        self._updating_ui = False
        self._fps_timer.start()

    def _on_gain_spin_changed(self, db: float) -> None:
        if self._updating_ui:
            return
        self._updating_ui = True
        self.gain_slider.setValue(int(round(db * 10.0)))
        self._updating_ui = False
        self._gain_timer.start()

    def _on_gain_slider_changed(self, val: int) -> None:
        if self._updating_ui:
            return
        self._updating_ui = True
        self.gain_spin.setValue(val / 10.0)
        self._updating_ui = False
        self._gain_timer.start()

    def _apply_roi_preset(self) -> None:
        data = self.roi_preset_combo.currentData()
        if not data:
            return
        rx, ry, w, h = data
        self.roi_x_spin.setValue(rx)
        self.roi_y_spin.setValue(ry)
        self.roi_w_spin.setValue(w)
        self.roi_h_spin.setValue(h)

    # --- Connection lifecycle ---
    def _toggle_connect(self) -> None:
        if self._ctrl.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self) -> None:
        host = self.host_edit.text().strip()
        cport = self.ctrl_port_spin.value()
        sport = self.stream_port_spin.value()
        try:
            st = self._ctrl.connect(host, cport)
        except Exception as exc:
            QMessageBox.critical(self, "Connection Error", f"Failed to connect to {host}:{cport}\n\n{exc}")
            return

        self._apply_status_to_ui(st, sync_inputs=True)
        self._reset_stretch()

        self._ctrl_worker = ControlWorker(self._ctrl, self)
        self._ctrl_worker.status_received.connect(self._apply_status_to_ui)
        self._ctrl_worker.error_occurred.connect(lambda msg: self.status_lbl.setText(f"Control error: {msg}"))
        self._ctrl_worker.start()

        self._stream = StreamReceiver(host, sport, self)
        self._stream.frame_available.connect(self._on_frame_available)
        self._stream.stats_updated.connect(self._on_stats_updated)
        self._stream.connection_lost.connect(self._on_stream_lost)
        self._stream.start()

        self._poll_timer.start()
        self.connect_btn.setText("Disconnect")
        self.status_lbl.setText(f"Connected to {host} ({st.get('camera_model', 'RAWCam')})")

    def _disconnect(self) -> None:
        self._spot_job = None
        self._showing_still = False
        self._still_frame = None
        self.resume_live_btn.setEnabled(False)
        self.canvas.clear_spot_overlay()
        self._poll_timer.stop()
        if self._ctrl_worker is not None:
            self._ctrl_worker.stop()
            self._ctrl_worker.wait(1500)
            self._ctrl_worker = None
        if self._stream is not None:
            self._stream.stop()
            self._stream.wait(1500)
            self._stream = None
        self._ctrl.close()
        self.connect_btn.setText("Connect")
        self.status_lbl.setText("Disconnected")
        self.stats_lbl.setText("")

    def _on_stream_lost(self, msg: str) -> None:
        self.status_lbl.setText(f"Stream lost: {msg}")
        self._disconnect()

    def _on_frame_available(self) -> None:
        if self._stream is None:
            return
        frame = self._stream.take_latest_frame()
        if frame is None:
            return
        if self._showing_still:
            # Keep displaying the captured long-exposure still until user clicks Resume Live
            return
        prev_fmt = self._latest_frame.header.pixel_format if self._latest_frame else None
        self._latest_frame = frame
        if prev_fmt != frame.header.pixel_format:
            # Adjust default white ceiling when switching between RAW12 / RAW10 / 8-bit preview
            max_v = frame.header.max_value
            self._updating_ui = True
            self.white_spin.setMaximum(max_v)
            self.black_spin.setMaximum(max(max_v - 1, 1))
            self.white_spin.setValue(max_v)
            self.black_spin.setValue(0)
            self._updating_ui = False
            self._update_display_params()
        self.canvas.set_frame(frame)
        if self._spot_job is not None:
            self._step_spot_auto(frame)

    # --- Long Exposure Single Shot (> 500 ms) ---
    def _trigger_long_shot(self) -> None:
        if not self._ctrl.connected:
            QMessageBox.information(self, "Single Shot", "Connect to the camera first.")
            return
        if self._long_shot_worker is not None and self._long_shot_worker.isRunning():
            return

        host = self.host_edit.text().strip()
        snap_port = self.stream_port_spin.value() + 1
        exp_ms = self.long_exp_spin.value()
        exp_us = int(round(exp_ms * 1000.0))
        gain_db = self.long_gain_spin.value()

        self.long_shot_btn.setEnabled(False)
        self.long_shot_lbl.setText(f"Integrating {exp_ms:.0f} ms single shot (gain {gain_db:.1f} dB)...")
        self.status_lbl.setText(f"Long-exposure single shot in progress ({exp_ms:.0f} ms)...")

        worker = LongShotWorker(host, snap_port, exp_us, gain_db, self)
        worker.finished_frame.connect(self._on_long_shot_done)
        worker.failed.connect(self._on_long_shot_failed)
        self._long_shot_worker = worker
        worker.start()

    def _on_long_shot_done(self, frame: FramePacket) -> None:
        self.long_shot_btn.setEnabled(True)
        self._showing_still = True
        self._still_frame = frame
        self._latest_frame = frame
        self.resume_live_btn.setEnabled(True)

        max_v = frame.header.max_value
        self._updating_ui = True
        self.white_spin.setMaximum(max_v)
        self.black_spin.setMaximum(max(max_v - 1, 1))
        self.white_spin.setValue(max_v)
        self.black_spin.setValue(0)
        self._updating_ui = False
        self._update_display_params()

        self.canvas.set_frame(frame)
        exp_ms = frame.header.exposure_us / 1000.0
        gain_db = frame.header.gain_db_x10 / 10.0
        msg = (
            f"Still: {frame.header.width}×{frame.header.height} RAW{frame.header.bit_depth} "
            f"({exp_ms:.1f} ms, {gain_db:.1f} dB) — click 'Snap 12-bit Lossless' to save or 'Resume Live'"
        )
        self.long_shot_lbl.setText(msg)
        self.status_lbl.setText(msg)

    def _on_long_shot_failed(self, err: str) -> None:
        self.long_shot_btn.setEnabled(True)
        self.long_shot_lbl.setText(f"Single shot failed: {err}")
        self.status_lbl.setText(f"Single shot failed: {err}")

    def _resume_live_view(self) -> None:
        self._showing_still = False
        self._still_frame = None
        self.resume_live_btn.setEnabled(False)
        self.long_shot_lbl.setText("Live stream resumed.")
        self.status_lbl.setText("Live stream resumed")

    def _on_stats_updated(self, rx_fps: float, mb_s: float, dropped: int, decode_tag: str) -> None:
        if self._latest_frame is None:
            return
        h = self._latest_frame.header
        self.stats_lbl.setText(
            f"{h.width}×{h.height} [{decode_tag}]  |  "
            f"RX: {rx_fps:.1f} fps  |  {mb_s:.2f} MB/s  |  Drop: {dropped}"
        )

    # --- Spot Auto (One-Shot) ---
    def _on_spot_pick_toggled(self, checked: bool) -> None:
        self.canvas.set_pick_mode(checked)
        if checked:
            self.spot_pick_btn.setText("Picking... Click Image Now")
            self.spot_pick_btn.setStyleSheet("background-color: #15803d; border-color: #22c55e; font-weight: 600;")
        else:
            self.spot_pick_btn.setText("Click Image to Auto-Tune (or Shift+Click)")
            self.spot_pick_btn.setStyleSheet("")

    def _on_spot_clicked(self, x: int, y: int) -> None:
        if self.spot_pick_btn.isChecked():
            self.spot_pick_btn.setChecked(False)
        if not self._ctrl.connected or self._ctrl_worker is None or self._latest_frame is None:
            self.spot_info_lbl.setText("Connect and wait for a frame first.")
            return

        radius = self.spot_rad_spin.value()
        target_pct = self.spot_pct_spin.value() / 100.0
        control_mode = self.spot_target_combo.currentData()

        self._spot_overlay_timer.stop()
        self.canvas.set_spot_overlay(x, y, radius)

        self._spot_job = {
            "x": x,
            "y": y,
            "radius": radius,
            "target_pct": target_pct,
            "control": control_mode,
            "step": 0,
            "max_steps": 5,
            "wait_until_seq": 0,
            "wait_until_time": 0.0,
        }
        self._step_spot_auto(self._latest_frame)

    def _step_spot_auto(self, frame: FramePacket) -> None:
        job = self._spot_job
        if job is None or self._ctrl_worker is None:
            return
        if frame.header.frame_seq < job["wait_until_seq"] or time.monotonic() < job["wait_until_time"]:
            return

        arr = unpack_to_u16(frame)
        h, w = arr.shape
        x, y, r = job["x"], job["y"], job["radius"]
        x0, x1 = max(0, x - r), min(w, x + r + 1)
        y0, y1 = max(0, y - r), min(h, y + r + 1)
        patch = arr[y0:y1, x0:x1]
        if patch.size == 0:
            self._spot_job = None
            return

        max_val = float(frame.header.max_value)
        mean_val = float(np.mean(patch))
        cur_pct = mean_val / max_val
        target_val = job["target_pct"] * max_val
        rel_err = abs(mean_val - target_val) / max(target_val, 1.0)

        job["step"] += 1
        step = job["step"]

        if rel_err <= 0.06 or step > job["max_steps"]:
            self._spot_job = None
            self._spot_overlay_timer.start()
            tag = "Converged" if rel_err <= 0.06 else "Done"
            self.spot_info_lbl.setText(
                f"{tag} ({step - 1} steps) at ({x},{y}): {mean_val:.0f}/{max_val:.0f} ({cur_pct*100:.1f}%)"
            )
            return

        # Compute safe multiplicative ratio (handle near-black or saturated patches)
        if mean_val >= 0.97 * max_val:
            ratio = 0.45
        elif mean_val <= max(2.0, 0.015 * max_val):
            ratio = 3.0
        else:
            ratio = max(0.25, min(4.0, target_val / mean_val))

        cur_exp_us = float(frame.header.exposure_us or round(self.exp_ms_spin.value() * 1000.0))
        cur_gain_db = float(frame.header.gain_db_x10 / 10.0)
        ctrl_mode = job["control"]

        changed = False
        if ctrl_mode == "exposure":
            new_exp_us = int(round(max(50.0, min(500000.0, cur_exp_us * ratio))))
            if abs(new_exp_us - cur_exp_us) >= max(10.0, 0.02 * cur_exp_us):
                self._set_exp_ui_and_send(new_exp_us)
                changed = True
        elif ctrl_mode == "gain":
            delta_db = 20.0 * math.log10(ratio)
            new_gain_db = round(max(0.0, min(48.0, cur_gain_db + delta_db)), 1)
            if abs(new_gain_db - cur_gain_db) >= 0.2:
                self._set_gain_ui_and_send(new_gain_db)
                changed = True
        else:
            # Exposure first (keep within current frame period if possible, up to 100ms), then gain
            exp_cap_us = 100000.0
            if ratio > 1.0 and cur_exp_us < exp_cap_us:
                new_exp_us = int(round(max(50.0, min(exp_cap_us, cur_exp_us * ratio))))
                if abs(new_exp_us - cur_exp_us) >= max(10.0, 0.02 * cur_exp_us):
                    self._set_exp_ui_and_send(new_exp_us)
                    changed = True
            elif ratio < 1.0 and cur_gain_db > 0.2:
                delta_db = 20.0 * math.log10(ratio)
                new_gain_db = round(max(0.0, min(48.0, cur_gain_db + delta_db)), 1)
                self._set_gain_ui_and_send(new_gain_db)
                changed = True
            elif ratio < 1.0:
                new_exp_us = int(round(max(50.0, min(500000.0, cur_exp_us * ratio))))
                if abs(new_exp_us - cur_exp_us) >= max(10.0, 0.02 * cur_exp_us):
                    self._set_exp_ui_and_send(new_exp_us)
                    changed = True
            else:
                delta_db = 20.0 * math.log10(ratio)
                new_gain_db = round(max(0.0, min(48.0, cur_gain_db + delta_db)), 1)
                if abs(new_gain_db - cur_gain_db) >= 0.2:
                    self._set_gain_ui_and_send(new_gain_db)
                    changed = True

        if not changed:
            self._spot_job = None
            self._spot_overlay_timer.start()
            self.spot_info_lbl.setText(
                f"Limit reached at ({x},{y}): {mean_val:.0f}/{max_val:.0f} ({cur_pct*100:.1f}%)"
            )
            return

        # Wait for sensor to apply new register + at least 2 frames
        job["wait_until_seq"] = frame.header.frame_seq + 2
        job["wait_until_time"] = time.monotonic() + 0.22
        self.spot_info_lbl.setText(
            f"Tuning step {step}/{job['max_steps']} at ({x},{y}): {cur_pct*100:.1f}% → {job['target_pct']*100:.0f}%..."
        )

    def _set_exp_ui_and_send(self, exp_us: int) -> None:
        exp_ms = exp_us / 1000.0
        self._updating_ui = True
        self.exp_ms_spin.setValue(round(exp_ms, 2))
        self.exp_slider.setValue(self._ms_to_slider(exp_ms))
        self._updating_ui = False
        if self._ctrl_worker is not None:
            self._ctrl_worker.enqueue({"cmd": "set_exposure", "mode": "manual", "exposure_us": exp_us}, sync_inputs=False)

    def _set_gain_ui_and_send(self, gain_db: float) -> None:
        self._updating_ui = True
        self.gain_spin.setValue(gain_db)
        self.gain_slider.setValue(int(round(gain_db * 10.0)))
        self._updating_ui = False
        if self._ctrl_worker is not None:
            self._ctrl_worker.enqueue({"cmd": "set_gain", "mode": "manual", "gain_db": gain_db}, sync_inputs=False)

    # --- Control RPCs (asynchronous, non-blocking) ---
    def _send_exposure(self) -> None:
        if self._ctrl_worker is None or not self._ctrl.connected:
            return
        exp_us = int(round(self.exp_ms_spin.value() * 1000.0))
        self._ctrl_worker.enqueue({"cmd": "set_exposure", "mode": "manual", "exposure_us": exp_us}, sync_inputs=False)

    def _send_gain(self) -> None:
        if self._ctrl_worker is None or not self._ctrl.connected:
            return
        gain_db = self.gain_spin.value()
        self._ctrl_worker.enqueue({"cmd": "set_gain", "mode": "manual", "gain_db": gain_db}, sync_inputs=False)

    def _send_fps(self) -> None:
        if self._ctrl_worker is None or not self._ctrl.connected:
            return
        self._ctrl_worker.enqueue({"cmd": "set_fps", "target_fps": self.fps_spin.value()}, sync_inputs=False)

    def _send_format(self) -> None:
        if self._ctrl_worker is None or not self._ctrl.connected:
            return
        self._ctrl_worker.enqueue({
            "cmd": "set_format",
            "bit_depth": self.bit_depth_combo.currentData(),
            "roi_x": self.roi_x_spin.value(),
            "roi_y": self.roi_y_spin.value(),
            "width": self.roi_w_spin.value(),
            "height": self.roi_h_spin.value(),
        }, sync_inputs=True)

    def _on_stream_mode_changed(self) -> None:
        if self._updating_ui or self._ctrl_worker is None or not self._ctrl.connected:
            return
        mode = self.stream_mode_combo.currentData()
        self._ctrl_worker.enqueue({"cmd": "set_stream_mode", "mode": mode}, sync_inputs=False)

    def _poll_status(self) -> None:
        if self._ctrl_worker is None or not self._ctrl.connected:
            return
        self._ctrl_worker.enqueue({"cmd": "get_status"}, sync_inputs=False)

    def _apply_status_to_ui(self, st: dict[str, Any], sync_inputs: bool) -> None:
        self._last_status = st
        act_exp_us = st.get("actual_exposure_us", 0)
        self.actual_exp_lbl.setText(f"HW: {act_exp_us / 1000.0:.2f} ms")
        self.actual_gain_lbl.setText(f"HW: {st.get('actual_gain_db', 0.0):.1f} dB")

        eff_fps = st.get("effective_fps", 0.0)
        max_fps = st.get("sensor_max_fps", 60.0)
        meas_fps = st.get("measured_fps", 0.0)
        self.eff_fps_lbl.setText(f"Eff {eff_fps:.1f}/{max_fps:.0f} (cam {meas_fps:.1f})")

        if st.get("fps_clamped_by_exposure", False):
            self.fps_clamp_lbl.setText(
                f"Clamped by exposure ({st.get('target_exposure_us', 0)/1000.0:.1f} ms). "
                f"Will recover to {st.get('target_fps', 30.0):.1f} fps when exposure decreases."
            )
            self.fps_clamp_lbl.setVisible(True)
        else:
            self.fps_clamp_lbl.setText("")
            self.fps_clamp_lbl.setVisible(False)

        self.fps_slider.setMaximum(max(1, int(round(max_fps))))

        if sync_inputs:
            self._updating_ui = True
            exp_ms = st.get("target_exposure_us", 10000) / 1000.0
            self.exp_ms_spin.setValue(exp_ms)
            self.exp_slider.setValue(self._ms_to_slider(exp_ms))

            gain_db = float(st.get("target_gain_db", 0.0))
            self.gain_spin.setValue(gain_db)
            self.gain_slider.setValue(int(round(gain_db * 10.0)))

            tfps = float(st.get("target_fps", 30.0))
            self.fps_spin.setValue(tfps)
            self.fps_slider.setValue(int(round(tfps)))

            idx = self.bit_depth_combo.findData(st.get("bit_depth", 12))
            if idx >= 0:
                self.bit_depth_combo.setCurrentIndex(idx)
            self.roi_x_spin.setValue(int(st.get("roi_x", 0)))
            self.roi_y_spin.setValue(int(st.get("roi_y", 0)))
            self.roi_w_spin.setValue(int(st.get("width", 1944)))
            self.roi_h_spin.setValue(int(st.get("height", 1096)))

            idx = self.stream_mode_combo.findData(st.get("stream_mode", "lossless"))
            if idx >= 0:
                self.stream_mode_combo.setCurrentIndex(idx)
            self._updating_ui = False

    # --- Display controls & saving ---
    def _update_display_params(self) -> None:
        if self._updating_ui:
            return
        self.canvas.set_display_params(
            self.black_spin.value(),
            max(self.white_spin.value(), self.black_spin.value() + 1),
            self.gamma_spin.value(),
            self.cmap_combo.currentData(),
        )

    def _auto_stretch(self) -> None:
        if self._latest_frame is None:
            return
        arr = unpack_to_u16(self._latest_frame)
        # Subsample for instant percentile computation
        sub = arr[::4, ::4]
        lo, hi = np.percentile(sub, [1.0, 99.5])
        if hi <= lo:
            hi = lo + 1
        self._updating_ui = True
        self.black_spin.setValue(int(lo))
        self.white_spin.setValue(int(hi))
        self._updating_ui = False
        self._update_display_params()

    def _reset_stretch(self) -> None:
        max_v = self._latest_frame.header.max_value if self._latest_frame else 4095
        self._updating_ui = True
        self.black_spin.setMaximum(max(max_v - 1, 1))
        self.white_spin.setMaximum(max_v)
        self.black_spin.setValue(0)
        self.white_spin.setValue(max_v)
        self.gamma_spin.setValue(1.0)
        self._updating_ui = False
        self._update_display_params()

    def _save_current_frame(self) -> None:
        if self._showing_still and self._still_frame is not None:
            frame = self._still_frame
        else:
            if not self._ctrl.connected:
                QMessageBox.information(self, "Save 12-bit Snapshot", "Connect to the camera first.")
                return
            host = self.host_edit.text().strip()
            snap_port = self.stream_port_spin.value() + 1  # default 5602
            try:
                self.status_lbl.setText("Grabbing genuine 10/12-bit lossless RAW snapshot from RPi...")
                QApplication.processEvents()
                frame = grab_lossless_snapshot(host, snap_port, timeout=5.0)
            except Exception as exc:
                QMessageBox.critical(self, "Snapshot Error", f"Failed to grab lossless RAW frame:\n{exc}")
                return

        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        default_name = (
            f"rawcam_{frame.header.width}x{frame.header.height}_"
            f"RAW{frame.header.bit_depth}_{frame.header.pixel_format.name}_{ts}.png"
        )
        path, _ = QFileDialog.getSaveFileName(
            self,
            f"Save Lossless {frame.header.bit_depth}-bit Frame",
            str(Path.home() / default_name),
            "16-bit PNG (*.png);;16-bit TIFF (*.tiff *.tif);;Raw Packed Binary (*.raw)",
        )
        if not path:
            return
        try:
            saved = save_frame(frame, path)
            self.status_lbl.setText(
                f"Saved lossless {frame.header.bit_depth}-bit ({frame.header.width}×{frame.header.height}) to {saved}"
            )
        except Exception as exc:
            QMessageBox.critical(self, "Save Error", str(exc))

    def closeEvent(self, event) -> None:
        self._disconnect()
        super().closeEvent(event)


def main() -> None:
    # Only macOS requires requesting 3.3 Core explicitly; on Linux Wayland/EGL
    # forcing 3.3 Core causes EGL_BAD_MATCH (3009). Let Qt negotiate native GL/GLES.
    fmt = QSurfaceFormat()
    if sys.platform == "darwin":
        fmt.setVersion(3, 3)
        fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
    fmt.setSwapInterval(0)
    QSurfaceFormat.setDefaultFormat(fmt)

    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_STYLESHEET)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
