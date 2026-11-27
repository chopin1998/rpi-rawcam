"""PySide6 main application window for RAW10/12 remote viewer & controller."""

from __future__ import annotations

import datetime
import math
import os
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
from PySide6.QtCore import QPoint, QSettings, QSize, Qt, QTimer, Slot
from PySide6.QtGui import QCursor, QKeySequence, QShortcut, QSurfaceFormat
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QToolButton,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QStyle,
    QSizePolicy,
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
}
QToolButton#sectionHeader {
    background-color: #20242c;
    border: 1px solid #333b49;
    border-radius: 4px;
    padding: 2px 6px;
    font-weight: 600;
    color: #a8b3cf;
}
QToolButton#sectionHeader:hover, QToolButton#sectionHeader:focus {
    border-color: #558ae3;
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
QSlider:horizontal {
    min-height: 26px;
}
QSlider::groove:horizontal {
    height: 6px;
    background: #465268;
    border-radius: 3px;
}
QSlider::sub-page:horizontal {
    background: #84baff;
    border-radius: 3px;
}
QSlider#gainSlider::sub-page:horizontal { background: #70d6a9; }
QSlider#fpsSlider::sub-page:horizontal { background: #f8be68; }
QSlider::handle:horizontal {
    width: 18px;
    margin: -7px 0;
    border: 2px solid #243247;
    border-radius: 9px;
    background: #f5f8ff;
}
QSlider::handle:horizontal:hover, QSlider::handle:horizontal:focus {
    border-color: #558ae3;
}
QSlider::handle:horizontal:pressed { background: #c6e1ff; }
QStatusBar {
    background-color: #111317;
    color: #98a2b3;
    border-top: 1px solid #262a33;
}
"""


def diagonal_width_change(dx: float, dy: float, ratio: float) -> float:
    """Project pointer motion onto a corner drag with a fixed image aspect."""
    return (dx + dy / ratio) / (1 + 1 / ratio**2)


class ResizeHandle(QWidget):
    def __init__(self, window, horizontal, vertical):
        super().__init__(window)
        self.horizontal = horizontal
        self.vertical = vertical
        self.origin = None
        if horizontal and vertical:
            cursor = Qt.CursorShape.SizeFDiagCursor if horizontal == vertical else Qt.CursorShape.SizeBDiagCursor
        else:
            cursor = Qt.CursorShape.SizeHorCursor if horizontal else Qt.CursorShape.SizeVerCursor
        self.setCursor(cursor)
        self.setAutoFillBackground(False)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.origin = event.globalPosition()
            self.start = self.window().geometry()
            self.window()._restore_geometry = None
            event.accept()

    def mouseMoveEvent(self, event):
        if self.origin is None or not event.buttons() & Qt.MouseButton.LeftButton:
            return
        window = self.window()
        delta = event.globalPosition() - self.origin
        dx, dy = delta.x() * self.horizontal, delta.y() * self.vertical
        ratio = window.frame_ratio
        if self.horizontal and self.vertical:
            width_change = diagonal_width_change(dx, dy, ratio)
        else:
            width_change = dx if self.horizontal else dy * ratio
        size = window.proportional_size(self.start.width() + width_change)
        window.setFixedSize(size)
        # Keep the opposite side stationary for left and top drags.
        if self.horizontal < 0 or self.vertical < 0:
            x = self.start.x() + (self.start.width() - size.width() if self.horizontal < 0 else 0)
            y = self.start.y() + (self.start.height() - size.height() if self.vertical < 0 else 0)
            window.move(x, y)
        event.accept()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.origin = None
            event.accept()


class ShortcutResize(QWidget):
    """Receive pointer movement across the screen until the resize is committed."""
    def __init__(self, viewer):
        super().__init__(viewer, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.viewer = viewer
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.SizeFDiagCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def begin(self) -> None:
        if self.isVisible() or not self.viewer.isActiveWindow():
            return
        self.start_width = self.viewer.width()
        self.previous_restore_geometry = self.viewer._restore_geometry
        corner = self.viewer.corner_handle
        self.origin = corner.mapToGlobal(corner.rect().center())
        self.viewer._restore_geometry = None
        self.setGeometry(self.viewer.screen().geometry())
        self.show()
        self.setFocus()
        QCursor.setPos(self.origin)

    def mouseMoveEvent(self, event):
        delta = event.globalPosition() - self.origin
        ratio = self.viewer.frame_ratio
        dx = diagonal_width_change(delta.x(), delta.y(), ratio)
        self.viewer._size_to_layout(self.start_width + dx)
        event.accept()

    def mousePressEvent(self, event):
        self.hide()
        event.accept()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.viewer._size_to_layout(self.start_width)
            self.viewer._restore_geometry = self.previous_restore_geometry
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Escape):
            self.hide()
        event.accept()


class WindowTitle(QLabel):
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.window().windowHandle().startSystemMove()
            event.accept()

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            window = self.window()
            window._toggle_maximized()
            event.accept()


class PreviewArea(QWidget):
    def __init__(self, canvas, controls, groups):
        super().__init__()
        self.canvas = canvas
        self.controls = controls
        self.groups = groups
        canvas.setParent(self)
        canvas.setMinimumSize(1, 1)
        controls.setParent(self)
        self.headers = []
        self.manually_closed = set()
        self.preferred_group = None
        grid = controls.layout()
        for index, group in enumerate(groups):
            header = QToolButton(controls)
            header.setObjectName("sectionHeader")
            header.setText(group.windowTitle().replace("&", "&&"))
            header.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
            header.setCheckable(True)
            header.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
            header.clicked.connect(lambda checked, i=index: self.toggle_group(i, checked))
            self.headers.append(header)
            grid.addWidget(header, index * 2, 0)
            grid.addWidget(group, index * 2 + 1, 0)
        self.column_width = 400

    def toggle_group(self, index, expanded):
        if expanded:
            self.manually_closed.discard(index)
            self.preferred_group = index
        else:
            self.manually_closed.add(index)
            if self.preferred_group == index:
                self.preferred_group = None
        self.arrange()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self.arrange()

    def arrange(self) -> None:
        w, h = self.width(), self.height()
        grid = self.controls.layout()
        margins = grid.contentsMargins()
        spacing = grid.spacing()
        width = self.column_width - margins.left() - margins.right()
        heights = [max(group.minimumSizeHint().height(), group.heightForWidth(width))
                   if group.hasHeightForWidth() else group.sizeHint().height()
                   for group in self.groups]
        header_height = (sum(header.sizeHint().height() for header in self.headers)
                         + spacing * (len(self.headers) - 1) + margins.top() + margins.bottom())
        costs = [height + spacing for height in heights]
        # Even at minimum height, every title and any one requested group must fit.
        self.setMinimumSize(self.column_width + 8 + 64, header_height + max(costs))
        cw = max(1, w - self.column_width - 8)
        self.canvas.setGeometry(0, 0, cw, h)
        self.controls.setGeometry(cw + 8, 0, self.column_width, h)

        expanded = set(range(len(self.groups))) - self.manually_closed
        required = header_height + sum(costs[i] for i in expanded)
        if required <= h:
            self.preferred_group = None
        for i in reversed(range(len(self.groups))):
            if required <= h:
                break
            if i in expanded and i != self.preferred_group:
                expanded.remove(i)
                required -= costs[i]

        for i, (header, group) in enumerate(zip(self.headers, self.groups)):
            opened = i in expanded
            header.setChecked(opened)
            header.setArrowType(Qt.ArrowType.DownArrow if opened else Qt.ArrowType.RightArrow)
            group.setVisible(opened)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("RAWCam Viewer (GPU Accelerated)")
        self.setWindowFlag(Qt.WindowType.FramelessWindowHint)
        self.frame_ratio = 1944 / 1096
        self._restore_geometry = None
        self.resize_handles = []
        self.resize(1420, 860)

        self._ctrl = ControlClient()
        self._ctrl_worker: ControlWorker | None = None
        self._stream: StreamReceiver | None = None
        self._long_shot_worker: LongShotWorker | None = None
        self._showing_still: bool = False
        self._still_frame: FramePacket | None = None
        self._latest_frame: FramePacket | None = None
        self._layout_frame_size = None
        self._shown_once: bool = False
        self._last_status: dict[str, Any] = {}
        self._updating_ui = False

        # One-shot Spot Auto state machine
        self._spot_job: dict[str, Any] | None = None
        self._spot_overlay_timer = QTimer(self)
        self._spot_overlay_timer.setSingleShot(True)
        self._spot_overlay_timer.setInterval(3000)
        self._spot_overlay_timer.timeout.connect(lambda: self.canvas.clear_spot_overlay())

        self._settings = QSettings("rawcam", "viewer")
        self._build_ui()
        self._set_font_size(self._settings.value("fontPoints", 11, type=int))
        self._fit_default_window_size()
        # KWin consumes its configured resize shortcut before Qt sees the key.
        # Listen to that action without claiming or changing the desktop binding.
        if sys.platform.startswith("linux"):
            from PySide6.QtCore import SLOT
            from PySide6.QtDBus import QDBusConnection
            QDBusConnection.sessionBus().connect(
                "org.kde.kglobalaccel", "/component/kwin", "org.kde.kglobalaccel.Component",
                "globalShortcutPressed", self, SLOT("_desktop_shortcut(QString,QString,qlonglong)")
            )
        self.shortcut_resize = ShortcutResize(self)
        self._resize_shortcut = QShortcut(QKeySequence("Meta+R"), self)
        self._resize_shortcut.activated.connect(self._begin_shortcut_resize)

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

        title_bar = QHBoxLayout()
        title_bar.addWidget(WindowTitle(self.windowTitle()), stretch=1)
        for label, icon, action in (
            ("Minimize", QStyle.StandardPixmap.SP_TitleBarMinButton, self.showMinimized),
            ("Maximize / Restore", QStyle.StandardPixmap.SP_TitleBarMaxButton, self._toggle_maximized),
            ("Close", QStyle.StandardPixmap.SP_TitleBarCloseButton, self.close),
        ):
            button = QToolButton()
            button.setIcon(self.style().standardIcon(icon))
            button.setAccessibleName(label)
            button.setToolTip(label)
            button.clicked.connect(action)
            title_bar.addWidget(button)
        root.addLayout(title_bar)

        # --- Top connection bar ---
        top_bar = QHBoxLayout()
        top_bar.setSpacing(8)

        top_bar.addWidget(QLabel("RPi IP:"))
        self.host_edit = QLineEdit("192.168.110.122")
        top_bar.addWidget(self.host_edit)

        top_bar.addWidget(QLabel("Ctrl:"))
        self.ctrl_port_spin = QSpinBox()
        self.ctrl_port_spin.setRange(1, 65535)
        self.ctrl_port_spin.setValue(5600)
        top_bar.addWidget(self.ctrl_port_spin)

        top_bar.addWidget(QLabel("Stream:"))
        self.stream_port_spin = QSpinBox()
        self.stream_port_spin.setRange(1, 65535)
        self.stream_port_spin.setValue(5601)
        top_bar.addWidget(self.stream_port_spin)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("primaryBtn")
        self.connect_btn.clicked.connect(self._toggle_connect)
        top_bar.addWidget(self.connect_btn)

        top_bar.addStretch()
        top_bar.addWidget(QLabel("Text size"))
        self.font_smaller = QPushButton("−")
        self.font_larger = QPushButton("+")
        for button, step, title, keys in (
            (self.font_smaller, -1, "Decrease text size", ["Ctrl+-"]),
            (self.font_larger, 1, "Increase text size", ["Ctrl++", "Ctrl+="]),
        ):
            button.setAccessibleName(title)
            button.setToolTip(f"{title} ({keys[0]})")
            button.clicked.connect(lambda checked=False, delta=step: self._set_font_size(self._font_points + delta))
            shortcut = QShortcut(self)
            shortcut.setKeys([QKeySequence(key) for key in keys])
            shortcut.activated.connect(button.click)
            top_bar.addWidget(button)
        root.addLayout(top_bar)

        view_bar = QHBoxLayout()
        view_bar.addWidget(QLabel("Stream Mode:"))
        self.stream_mode_combo = QComboBox()
        self._available_stream_modes: list[str] = []
        self._populate_stream_modes(["h264", "mjpeg", "lossless", "preview8"])
        self.stream_mode_combo.currentIndexChanged.connect(self._on_stream_mode_changed)
        view_bar.addWidget(self.stream_mode_combo)

        self.save_btn = QPushButton("Snap 12-bit Lossless")
        self.save_btn.setToolTip("Fetches a genuine full-resolution 10/12-bit lossless RAW frame from RPi (port 5602)")
        self.save_btn.clicked.connect(self._save_current_frame)
        view_bar.addWidget(self.save_btn)

        view_bar.addStretch()
        self.fit_btn = QPushButton("Fit")
        self.fit_btn.clicked.connect(lambda: self.canvas.reset_view())
        view_bar.addWidget(self.fit_btn)

        self.one_to_one_btn = QPushButton("1:1")
        self.one_to_one_btn.setToolTip("Left-click: 1:1 native size (resizes window) | Right-click: 2:1 / 4:1 / 1:2 presets")
        self.one_to_one_btn.clicked.connect(lambda: self._apply_scale_preset(1.0))
        self.one_to_one_btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.one_to_one_btn.customContextMenuRequested.connect(
            lambda pos: self._show_scale_menu(self.one_to_one_btn.mapToGlobal(pos))
        )
        view_bar.addWidget(self.one_to_one_btn)

        root.addLayout(view_bar)

        # --- Main body: GPU Canvas + Right Controls ---

        self.canvas = RawGLCanvas()
        self.canvas.spot_clicked.connect(self._on_spot_clicked)
        self.canvas.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.canvas.customContextMenuRequested.connect(
            lambda pos: self._show_scale_menu(self.canvas.mapToGlobal(pos))
        )

        panel = QWidget()
        pv = QGridLayout(panel)
        pv.setAlignment(Qt.AlignmentFlag.AlignTop)
        pv.setContentsMargins(2, 0, 6, 2)
        pv.setSpacing(5)

        # 0. Spot Auto (One-Shot) Group
        spot_grp = QWidget()
        spot_grp.setWindowTitle("Spot Auto (One-Shot)")
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
        self.spot_info_lbl.setStyleSheet("color: #bdc6d6;")
        self.spot_info_lbl.setWordWrap(True)
        spot_form.addRow(self.spot_info_lbl)

        # 1. Live Video Exposure Group (<= 500 ms for smooth video streaming)
        exp_grp = QWidget()
        exp_grp.setWindowTitle("Live Exposure (≤ 500 ms)")
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
        self.actual_exp_lbl.setStyleSheet("color: #bdc6d6;")
        exp_row.addWidget(self.exp_ms_spin)
        exp_row.addWidget(self.actual_exp_lbl, stretch=1)
        exp_form.addRow(exp_row)

        # Log-scaled slider: 0..1000 maps to 0.05 ms .. 500 ms
        self.exp_slider = QSlider(Qt.Orientation.Horizontal)
        self.exp_slider.setRange(0, 1000)
        self.exp_slider.setValue(self._ms_to_slider(10.0))
        self.exp_slider.valueChanged.connect(self._on_exp_slider_changed)
        self.exp_slider.setObjectName("expSlider")
        self.exp_slider.setAccessibleName("Exposure (ms)")
        exp_form.addRow("Exposure (ms)", self.exp_slider)

        # 1b. Long Exposure Single Shot Group (> 500 ms, lossless 12-bit still)
        long_grp = QWidget()
        long_grp.setWindowTitle("Long Exposure Single Shot (> 500 ms)")
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
        self.long_shot_lbl.setStyleSheet("color: #bdc6d6;")
        self.long_shot_lbl.setWordWrap(True)
        long_form.addRow(self.long_shot_lbl)

        # 2. Adaptive Frame Rate Group
        fps_grp = QWidget()
        fps_grp.setWindowTitle("Frame Rate (Adaptive)")
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
        self.eff_fps_lbl.setStyleSheet("color: #bdc6d6;")
        fps_row.addWidget(self.fps_spin)
        fps_row.addWidget(self.eff_fps_lbl, stretch=1)
        fps_form.addRow(fps_row)

        self.fps_slider = QSlider(Qt.Orientation.Horizontal)
        self.fps_slider.setRange(1, 60)
        self.fps_slider.setValue(30)
        self.fps_slider.valueChanged.connect(self._on_fps_slider_changed)
        self.fps_slider.setObjectName("fpsSlider")
        self.fps_slider.setAccessibleName("Rate (fps)")
        fps_form.addRow("Rate (fps)", self.fps_slider)

        self.fps_clamp_lbl = QLabel("")
        self.fps_clamp_lbl.setStyleSheet("color: #fbbf24;")
        self.fps_clamp_lbl.setWordWrap(True)
        self.fps_clamp_lbl.setVisible(False)
        fps_form.addRow(self.fps_clamp_lbl)

        # 3. Gain Group (Hardware is manual-only on VEYE RAW-MIPI)
        gain_grp = QWidget()
        gain_grp.setWindowTitle("Gain (Manual)")
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
        self.actual_gain_lbl.setStyleSheet("color: #bdc6d6;")
        gain_row.addWidget(self.gain_spin)
        gain_row.addWidget(self.actual_gain_lbl, stretch=1)
        gain_form.addRow(gain_row)

        self.gain_slider = QSlider(Qt.Orientation.Horizontal)
        self.gain_slider.setRange(0, 480)
        self.gain_slider.valueChanged.connect(self._on_gain_slider_changed)
        self.gain_slider.setObjectName("gainSlider")
        self.gain_slider.setAccessibleName("Gain (dB)")
        gain_form.addRow("Gain (dB)", self.gain_slider)


        # 4. Format & ROI Group
        fmt_grp = QWidget()
        fmt_grp.setWindowTitle("Format & ROI")
        fmt_form = QFormLayout(fmt_grp)
        fmt_form.setVerticalSpacing(4)
        fmt_form.setContentsMargins(6, 4, 6, 4)

        fmt_top = QHBoxLayout()
        self._sensor_caps_sig: tuple[int, int, int, int, tuple[int, ...]] | None = None
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

        # 5. GPU Display Stretch Group
        disp_grp = QWidget()
        disp_grp.setWindowTitle("GPU Display Stretch")
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

        self.preview = PreviewArea(self.canvas, panel, (exp_grp, gain_grp, fps_grp, long_grp,
                                                         spot_grp, fmt_grp, disp_grp))
        root.addWidget(self.preview, stretch=1)
        self.preview.arrange()

        # Status bar
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self.status_lbl = QLabel("Disconnected")
        self.stats_lbl = QLabel("")
        self.status_bar.addWidget(self.status_lbl, stretch=1)
        self.status_bar.addPermanentWidget(self.stats_lbl)
        self.status_bar.setSizeGripEnabled(False)
        # Wayland clients cannot position a top-level window. Only expose edges
        # that can keep their opposite side anchored on that backend.
        axes = (0, 1) if QApplication.platformName().startswith("wayland") else (-1, 0, 1)
        for x in axes:
            for y in axes:
                if x or y:
                    handle = ResizeHandle(self, x, y)
                    self.resize_handles.append(handle)
                    if x == y == 1:
                        self.corner_handle = handle
        self._place_resize_handles()

    def _set_font_size(self, points: int) -> None:
        self._font_points = max(8, min(20, points))
        app = QApplication.instance()
        font = app.font()
        font.setPointSize(self._font_points)
        app.setFont(font)
        app.setStyleSheet(DARK_STYLESHEET + f"\nQWidget {{ font-size: {self._font_points}pt; }}")
        self.font_smaller.setEnabled(self._font_points > 8)
        self.font_larger.setEnabled(self._font_points < 20)
        self.host_edit.setFixedWidth(self.host_edit.fontMetrics().horizontalAdvance("255.255.255.255") + 26)
        self.preview.column_width = max(round(400 * self._font_points / 11),
                                        max(g.minimumSizeHint().width() for g in self.preview.groups) + 28,
                                        max(h.sizeHint().width() for h in self.preview.headers) + 8)
        self.preview.arrange()
        if self.isVisible():
            self.centralWidget().layout().activate()
            self._size_to_layout(self.width())
        self._settings.setValue("fontPoints", self._font_points)

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
        self._size_to_layout(desired_cw + self._fixed_chrome().width())
        QTimer.singleShot(0, lambda: self.canvas.set_pixel_scale(scale))

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._shown_once:
            self._shown_once = True
            QTimer.singleShot(0, self._fit_default_window_size)

    def _fit_default_window_size(self) -> None:
        dpr = max(self.canvas.devicePixelRatioF(), 1.0)
        self._size_to_layout(round(1944 / dpr) + self._fixed_chrome().width())

    def _fixed_chrome(self) -> QSize:
        outer = self.size() - self.preview.size() if self.isVisible() else QSize(16, 100)
        return outer + QSize(self.preview.column_width + 8, 0)

    def proportional_size(self, width: float) -> QSize:
        fixed = self._fixed_chrome()
        minimum = self.minimumSizeHint() - fixed
        image_width = width - fixed.width()
        if screen := self.screen():
            area = screen.availableGeometry().size() - fixed
            image_width = min(image_width, area.width(), area.height() * self.frame_ratio)
        # Once either dimension reaches the controls' minimum, stop both together.
        image_width = max(image_width, minimum.width(), minimum.height() * self.frame_ratio)
        width_px = round(image_width)
        return fixed + QSize(width_px, round(width_px / self.frame_ratio))

    def _size_to_layout(self, width: float) -> None:
        # Publish a single allowed size to the compositor. Desktop snap, session
        # restore and external resize requests then use the same bounds as our grips.
        self.setFixedSize(self.proportional_size(width))

    def _toggle_maximized(self) -> None:
        if self._restore_geometry is not None:
            geometry = self._restore_geometry
            self._restore_geometry = None
            self._size_to_layout(geometry.width())
            if not QApplication.platformName().startswith("wayland"):
                self.move(geometry.topLeft())
            return
        self._restore_geometry = self.geometry()
        area = self.screen().availableGeometry()
        self._size_to_layout(area.width())
        if not QApplication.platformName().startswith("wayland"):
            self.move(area.center() - self.rect().center())

    @Slot(str, str, "qlonglong")
    def _desktop_shortcut(self, component, action, timestamp):
        if component == "kwin" and action == "Window Resize" and self.isActiveWindow():
            QTimer.singleShot(0, self._begin_shortcut_resize)

    def _begin_shortcut_resize(self):
        self.shortcut_resize.begin()

    def _place_resize_handles(self) -> None:
        w, h = self.width(), self.height()
        edge, corner = 6, 14
        for handle in self.resize_handles:
            x, y = handle.horizontal, handle.vertical
            thickness = corner if x and y else edge
            left = (0 if x < 0 else w - thickness) if x else corner
            top = (0 if y < 0 else h - thickness) if y else corner
            handle.setGeometry(left, top, thickness if x else w - 2 * corner,
                               thickness if y else h - 2 * corner)
            handle.setVisible(not self.isMaximized() and not self.isFullScreen())
            handle.raise_()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._place_resize_handles()

    def _display_frame(self, frame: FramePacket) -> None:
        dimensions = (frame.header.width, frame.header.height)
        self.canvas.set_frame(frame)
        if dimensions != self._layout_frame_size:
            self._layout_frame_size = dimensions
            self.frame_ratio = dimensions[0] / dimensions[1]
            self._size_to_layout(self.width())

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

        self._layout_frame_size = None
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
        sw = int(st.get("sensor_width", st.get("width", 0)))
        sh = int(st.get("sensor_height", st.get("height", 0)))
        stype = st.get("sensor_type", "mono")
        plat = st.get("platform", "")
        meta = f"{stype}, {sw}×{sh}" + (f", {plat}" if plat else "")
        self.status_lbl.setText(f"Connected to {host} — {st.get('camera_model', 'RAWCam')} ({meta})")

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
        self._display_frame(frame)
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

        self._display_frame(frame)
        exp_ms = frame.header.exposure_us / 1000.0
        gain_db = frame.header.gain_db_x10 / 10.0
        msg = (
            f"Still: {frame.header.width}×{frame.header.height} RAW{frame.header.bit_depth} "
            f"({exp_ms:.1f} ms, {gain_db:.1f} dB) — click 'Snap 12-bit Lossless' to save or 'Resume Live'"
        )
        if frame.restore_warning:
            msg = f"Still captured and available to save. Warning: {frame.restore_warning}"
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
        self.long_shot_lbl.setText("Live view selected.")
        self.status_lbl.setText("Live view selected")

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

    def _sync_sensor_capabilities(self, st: dict[str, Any]) -> None:
        sw = int(st.get("sensor_width", 0))
        sh = int(st.get("sensor_height", 0))
        if sw <= 0 or sh <= 0:
            return
        min_w = max(64, int(st.get("min_roi_width", 368)))
        min_h = max(64, int(st.get("min_roi_height", 304)))
        raw_bds = st.get("available_bit_depths")
        if isinstance(raw_bds, list) and raw_bds:
            bds = tuple(int(b) for b in raw_bds if int(b) in (8, 10, 12, 14, 16))
        else:
            bds = (12, 10)
        if not bds:
            bds = (12, 10)

        sig = (sw, sh, min_w, min_h, bds)
        if sig == self._sensor_caps_sig:
            return
        self._sensor_caps_sig = sig

        prev_updating = self._updating_ui
        self._updating_ui = True
        self.roi_x_spin.setRange(0, max(0, sw - min_w))
        self.roi_y_spin.setRange(0, max(0, sh - min_h))
        self.roi_w_spin.setRange(min_w, sw)
        self.roi_h_spin.setRange(min_h, sh)

        cur_bd = self.bit_depth_combo.currentData()
        self.bit_depth_combo.clear()
        for bd in bds:
            self.bit_depth_combo.addItem(f"RAW{bd}", bd)
        bd_idx = self.bit_depth_combo.findData(st.get("bit_depth", cur_bd))
        if bd_idx >= 0:
            self.bit_depth_combo.setCurrentIndex(bd_idx)

        self.roi_preset_combo.clear()
        self.roi_preset_combo.addItem(f"Full ({sw}×{sh})", (0, 0, sw, sh))
        for label, pw, ph in (
            ("4K", 3840, 2160),
            ("2K", 2560, 1440),
            ("1080p", 1920, 1080),
            ("720p", 1280, 720),
            ("VGA", 640, 480),
        ):
            if min_w <= pw <= sw and min_h <= ph <= sh and (pw, ph) != (sw, sh):
                ox = (((sw - pw) // 2) // 8) * 8
                oy = (((sh - ph) // 2) // 4) * 4
                self.roi_preset_combo.addItem(f"{label} ({pw}×{ph})", (ox, oy, pw, ph))
        self._updating_ui = prev_updating

    def _populate_stream_modes(self, modes: list[str], current_mode: str | None = None) -> None:
        labels = {
            "h264": "Hardware H.264 Preview",
            "mjpeg": "Hardware MJPEG Preview",
            "lossless": "Lossless RAW (10/12-bit)",
            "preview8": "8-bit Uncompressed Preview",
        }
        filtered = [m for m in modes if m in labels]
        if not filtered:
            filtered = ["lossless", "preview8"]
        if filtered != self._available_stream_modes:
            prev_updating = self._updating_ui
            self._updating_ui = True
            self.stream_mode_combo.clear()
            for m in filtered:
                self.stream_mode_combo.addItem(labels[m], m)
            self._available_stream_modes = list(filtered)
            self._updating_ui = prev_updating
        if current_mode:
            idx = self.stream_mode_combo.findData(current_mode)
            if idx >= 0 and self.stream_mode_combo.currentIndex() != idx:
                prev_updating = self._updating_ui
                self._updating_ui = True
                self.stream_mode_combo.setCurrentIndex(idx)
                self._updating_ui = prev_updating

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

        self._sync_sensor_capabilities(st)

        avail = st.get("available_stream_modes")
        cur_mode = st.get("stream_mode", "lossless")
        if isinstance(avail, list) and avail:
            modes = [str(m) for m in avail]
        elif cur_mode == "mjpeg":
            modes = ["mjpeg", "lossless", "preview8"]
        else:
            # Backward compatibility with older RPi servers that omit available_stream_modes
            modes = ["h264", "lossless", "preview8"]
        self._populate_stream_modes(modes, current_mode=cur_mode if sync_inputs else None)

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
        self.shortcut_resize.hide()
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

    # XWayland permits anchoring the opposite corner when dragging left/top.
    if os.environ.get("WAYLAND_DISPLAY") and os.environ.get("DISPLAY"):
        os.environ.setdefault("QT_QPA_PLATFORM", "xcb;wayland")
    QApplication.setApplicationName("rawcam-viewer")
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
