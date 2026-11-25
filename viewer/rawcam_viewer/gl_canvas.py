"""OpenGL 3.3 Core GPU-accelerated RAW10/RAW12 unpacker & viewer widget."""

from __future__ import annotations

import ctypes
import math

import numpy as np
from OpenGL import GL
from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QMouseEvent, QSurfaceFormat, QWheelEvent
from PySide6.QtOpenGLWidgets import QOpenGLWidget

from .protocol import FramePacket, PixelFormat

VERTEX_SHADER_BODY = """
out vec2 v_uv;
void main() {
    // Full-screen triangle (no VBO needed)
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    v_uv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
"""

FRAGMENT_SHADER_BODY = """
in vec2 v_uv;
out vec4 frag_color;

uniform usampler2D u_tex;
uniform int u_fmt;         // 1=Y10P, 2=Y12P, 3=Y10, 4=Y12, 5=Y8_PREVIEW
uniform int u_width;
uniform int u_height;
uniform float u_black;
uniform float u_white;
uniform float u_gamma;
uniform int u_colormap;    // 0=Gray, 1=Inferno, 2=Viridis, 3=Jet
uniform vec2 u_view_size;
uniform vec2 u_pan;        // in normalized image coords
uniform float u_zoom;
uniform int u_has_frame;
uniform vec2 u_spot_pos;   // image pixel coords, <0 if hidden
uniform float u_spot_rad;  // patch half-size in image pixels

vec3 colormap_inferno(float t) {
    vec3 c0 = vec3(0.0002, 0.0016, 0.0139);
    vec3 c1 = vec3(0.2582, 0.0386, 0.4064);
    vec3 c2 = vec3(0.5783, 0.1480, 0.4044);
    vec3 c3 = vec3(0.8650, 0.3168, 0.2261);
    vec3 c4 = vec3(0.9876, 0.6453, 0.0399);
    vec3 c5 = vec3(0.9884, 0.9984, 0.6449);
    float s = t * 5.0;
    if (s < 1.0) return mix(c0, c1, s);
    if (s < 2.0) return mix(c1, c2, s - 1.0);
    if (s < 3.0) return mix(c2, c3, s - 2.0);
    if (s < 4.0) return mix(c3, c4, s - 3.0);
    return mix(c4, c5, s - 4.0);
}

vec3 colormap_viridis(float t) {
    vec3 c0 = vec3(0.2670, 0.0049, 0.3294);
    vec3 c1 = vec3(0.2297, 0.3224, 0.5457);
    vec3 c2 = vec3(0.1276, 0.5669, 0.5506);
    vec3 c3 = vec3(0.3692, 0.7889, 0.3829);
    vec3 c4 = vec3(0.9932, 0.9062, 0.1439);
    float s = t * 4.0;
    if (s < 1.0) return mix(c0, c1, s);
    if (s < 2.0) return mix(c1, c2, s - 1.0);
    if (s < 3.0) return mix(c2, c3, s - 2.0);
    return mix(c3, c4, s - 3.0);
}

vec3 colormap_jet(float t) {
    float r = clamp(1.5 - abs(4.0 * t - 3.0), 0.0, 1.0);
    float g = clamp(1.5 - abs(4.0 * t - 2.0), 0.0, 1.0);
    float b = clamp(1.5 - abs(4.0 * t - 1.0), 0.0, 1.0);
    return vec3(r, g, b);
}

void main() {
    if (u_has_frame == 0 || u_width <= 0 || u_height <= 0) {
        frag_color = vec4(0.08, 0.09, 0.11, 1.0);
        return;
    }

    // Aspect-preserving fit + zoom + pan
    float view_aspect = u_view_size.x / max(u_view_size.y, 1.0);
    float img_aspect = float(u_width) / float(u_height);

    vec2 centered = v_uv - 0.5;
    if (view_aspect > img_aspect) {
        centered.x *= view_aspect / img_aspect;
    } else {
        centered.y *= img_aspect / view_aspect;
    }

    vec2 img_uv = centered / u_zoom + 0.5 - u_pan;
    // Flip Y so row 0 is at top
    img_uv.y = 1.0 - img_uv.y;

    if (img_uv.x < 0.0 || img_uv.x >= 1.0 || img_uv.y < 0.0 || img_uv.y >= 1.0) {
        frag_color = vec4(0.06, 0.07, 0.09, 1.0);
        return;
    }

    int x = clamp(int(floor(img_uv.x * float(u_width))), 0, u_width - 1);
    int y = clamp(int(floor(img_uv.y * float(u_height))), 0, u_height - 1);

    uint raw_val = 0u;
    if (u_fmt == 2) {
        // Y12P: 3 bytes per 2 pixels
        int pair = x >> 1;
        int base = pair * 3;
        int lane = x & 1;
        uint hi = texelFetch(u_tex, ivec2(base + lane, y), 0).r;
        uint lsb = texelFetch(u_tex, ivec2(base + 2, y), 0).r;
        uint lo = (lane == 0) ? (lsb & 0x0Fu) : (lsb >> 4u);
        raw_val = (hi << 4u) | lo;
    } else if (u_fmt == 1) {
        // Y10P: 5 bytes per 4 pixels
        int quad = x >> 2;
        int base = quad * 5;
        int lane = x & 3;
        uint hi = texelFetch(u_tex, ivec2(base + lane, y), 0).r;
        uint lsb = texelFetch(u_tex, ivec2(base + 4, y), 0).r;
        uint lo = (lsb >> uint(lane * 2)) & 0x03u;
        raw_val = (hi << 2u) | lo;
    } else if (u_fmt == 3 || u_fmt == 4) {
        // Y10 / Y12 unpacked uint16 LE
        uint lo = texelFetch(u_tex, ivec2(x * 2, y), 0).r;
        uint hi = texelFetch(u_tex, ivec2(x * 2 + 1, y), 0).r;
        raw_val = (hi << 8u) | lo;
    } else {
        // Y8_PREVIEW
        raw_val = texelFetch(u_tex, ivec2(x, y), 0).r;
    }

    float norm = clamp((float(raw_val) - u_black) / max(u_white - u_black, 1.0), 0.0, 1.0);
    norm = pow(norm, 1.0 / max(u_gamma, 0.05));

    vec3 rgb;
    if (u_colormap == 1) {
        rgb = colormap_inferno(norm);
    } else if (u_colormap == 2) {
        rgb = colormap_viridis(norm);
    } else if (u_colormap == 3) {
        rgb = colormap_jet(norm);
    } else {
        rgb = vec3(norm);
    }

    // Optional spot-auto neighborhood box overlay (pixel-accurate under zoom/pan)
    if (u_spot_pos.x >= 0.0 && u_spot_rad > 0.0) {
        vec2 d = abs(vec2(float(x), float(y)) - u_spot_pos);
        float border = max(1.5 / max(u_zoom, 0.25), 1.0);
        bool on_box = (d.x <= u_spot_rad + border && d.y <= u_spot_rad + border) &&
                      (d.x >= u_spot_rad - border || d.y >= u_spot_rad - border);
        bool on_cross = (d.x <= border && d.y <= 6.0) || (d.y <= border && d.x <= 6.0);
        if (on_box || on_cross) {
            rgb = vec3(0.20, 0.90, 0.45);
        }
    }
    frag_color = vec4(rgb, 1.0);
}
"""


class RawGLCanvas(QOpenGLWidget):
    """GPU-accelerated canvas that uploads raw packed bytes directly to OpenGL."""

    zoom_changed = Signal(float)
    spot_clicked = Signal(int, int)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setMinimumSize(480, 320)
        self.setMouseTracking(True)

        self._prog: int = 0
        self._vao: int = 0
        self._tex: int = 0
        self._tex_w: int = 0
        self._tex_h: int = 0

        self._pending_frame: FramePacket | None = None
        self._has_frame: bool = False
        self._img_w: int = 0
        self._img_h: int = 0
        self._img_fmt: int = int(PixelFormat.Y12P)

        self._black: float = 0.0
        self._white: float = 4095.0
        self._gamma: float = 1.0
        self._colormap: int = 0

        self._zoom: float = 1.0
        self._pan = QPointF(0.0, 0.0)
        self._dragging: bool = False
        self._drag_last = QPointF()

        self._pick_mode: bool = False
        self._spot_pos: tuple[float, float] = (-1.0, -1.0)
        self._spot_rad: float = 32.0

        self._uniforms: dict[str, int] = {}

    def set_pick_mode(self, enabled: bool) -> None:
        self._pick_mode = bool(enabled)
        self.setCursor(Qt.CursorShape.CrossCursor if enabled else Qt.CursorShape.ArrowCursor)

    def set_spot_overlay(self, x: float, y: float, radius: float) -> None:
        self._spot_pos = (float(x), float(y))
        self._spot_rad = float(radius)
        self.update()

    def clear_spot_overlay(self) -> None:
        self._spot_pos = (-1.0, -1.0)
        self.update()

    def widget_to_image(self, pos: QPointF) -> tuple[int, int] | None:
        if not self._has_frame or self._img_w <= 0 or self._img_h <= 0:
            return None
        vw = max(float(self.width()), 1.0)
        vh = max(float(self.height()), 1.0)
        # Match fragment shader v_uv (origin bottom-left)
        vx = pos.x() / vw
        vy = 1.0 - (pos.y() / vh)

        view_aspect = vw / vh
        img_aspect = float(self._img_w) / float(self._img_h)
        cx = vx - 0.5
        cy = vy - 0.5
        if view_aspect > img_aspect:
            cx *= view_aspect / img_aspect
        else:
            cy *= img_aspect / view_aspect

        iu = cx / self._zoom + 0.5 - self._pan.x()
        iv = 1.0 - (cy / self._zoom + 0.5 - self._pan.y())
        if iu < 0.0 or iu >= 1.0 or iv < 0.0 or iv >= 1.0:
            return None
        ix = max(0, min(self._img_w - 1, int(math.floor(iu * self._img_w))))
        iy = max(0, min(self._img_h - 1, int(math.floor(iv * self._img_h))))
        return ix, iy

    def set_frame(self, frame: FramePacket) -> None:
        self._pending_frame = frame
        self.update()

    def set_display_params(self, black: float, white: float, gamma: float, colormap: int) -> None:
        self._black = float(black)
        self._white = float(white)
        self._gamma = float(gamma)
        self._colormap = int(colormap)
        self.update()

    def reset_view(self) -> None:
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
        self.zoom_changed.emit(self._zoom)
        self.update()

    def set_zoom_1to1(self) -> None:
        if self._img_w <= 0 or self._img_h <= 0:
            return
        vw = max(self.width(), 1)
        vh = max(self.height(), 1)
        fit_scale = min(vw / self._img_w, vh / self._img_h)
        self._zoom = max(0.1, 1.0 / max(fit_scale, 1e-6))
        self._pan = QPointF(0.0, 0.0)
        self.zoom_changed.emit(self._zoom)
        self.update()

    def _compile_shader(self, src: str, shader_type: int) -> int:
        sh = GL.glCreateShader(shader_type)
        GL.glShaderSource(sh, src)
        GL.glCompileShader(sh)
        if not GL.glGetShaderiv(sh, GL.GL_COMPILE_STATUS):
            log = GL.glGetShaderInfoLog(sh).decode("utf-8", errors="replace")
            raise RuntimeError(f"Shader compile error: {log}")
        return sh

    def initializeGL(self) -> None:
        try:
            ctx = self.context()
            is_gles = ctx.isOpenGLES() if ctx is not None else False
            if is_gles:
                vs_src = "#version 300 es\nprecision highp float;\n" + VERTEX_SHADER_BODY
                fs_src = (
                    "#version 300 es\n"
                    "precision highp float;\n"
                    "precision highp int;\n"
                    "precision highp usampler2D;\n"
                    + FRAGMENT_SHADER_BODY
                )
            else:
                vs_src = "#version 330 core\n" + VERTEX_SHADER_BODY
                fs_src = "#version 330 core\n" + FRAGMENT_SHADER_BODY

            vs = self._compile_shader(vs_src, GL.GL_VERTEX_SHADER)
            fs = self._compile_shader(fs_src, GL.GL_FRAGMENT_SHADER)
            prog = GL.glCreateProgram()
            GL.glAttachShader(prog, vs)
            GL.glAttachShader(prog, fs)
            GL.glLinkProgram(prog)
            if not GL.glGetProgramiv(prog, GL.GL_LINK_STATUS):
                log = GL.glGetProgramInfoLog(prog).decode("utf-8", errors="replace")
                raise RuntimeError(f"Program link error: {log}")
            GL.glDeleteShader(vs)
            GL.glDeleteShader(fs)
            self._prog = prog

            self._vao = GL.glGenVertexArrays(1)
            self._tex = GL.glGenTextures(1)
            GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_NEAREST)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_NEAREST)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_CLAMP_TO_EDGE)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_CLAMP_TO_EDGE)

            names = [
                "u_tex", "u_fmt", "u_width", "u_height",
                "u_black", "u_white", "u_gamma", "u_colormap",
                "u_view_size", "u_pan", "u_zoom", "u_has_frame",
                "u_spot_pos", "u_spot_rad",
            ]
            self._uniforms = {n: GL.glGetUniformLocation(self._prog, n) for n in names}
        except Exception as exc:
            import sys
            print(f"[RawGLCanvas] OpenGL init failed: {exc}", file=sys.stderr)
            self._prog = 0

    def _upload_pending_frame(self) -> None:
        frame = self._pending_frame
        if frame is None:
            return
        self._pending_frame = None
        hdr = frame.header
        tex_w = hdr.stride
        tex_h = hdr.height
        if tex_w <= 0 or tex_h <= 0 or len(frame.payload) < tex_w * tex_h:
            return

        GL.glActiveTexture(GL.GL_TEXTURE0)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex)
        GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)

        if tex_w != self._tex_w or tex_h != self._tex_h:
            GL.glTexImage2D(
                GL.GL_TEXTURE_2D, 0, GL.GL_R8UI, tex_w, tex_h, 0,
                GL.GL_RED_INTEGER, GL.GL_UNSIGNED_BYTE, frame.payload
            )
            self._tex_w = tex_w
            self._tex_h = tex_h
        else:
            GL.glTexSubImage2D(
                GL.GL_TEXTURE_2D, 0, 0, 0, tex_w, tex_h,
                GL.GL_RED_INTEGER, GL.GL_UNSIGNED_BYTE, frame.payload
            )

        self._img_w = hdr.width
        self._img_h = hdr.height
        self._img_fmt = int(hdr.pixel_format)
        self._has_frame = True

    def paintGL(self) -> None:
        self._upload_pending_frame()

        dpr = self.devicePixelRatioF()
        vw = max(int(self.width() * dpr), 1)
        vh = max(int(self.height() * dpr), 1)
        GL.glViewport(0, 0, vw, vh)
        GL.glClearColor(0.06, 0.07, 0.09, 1.0)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT)

        if not self._prog:
            return

        GL.glUseProgram(self._prog)
        GL.glActiveTexture(GL.GL_TEXTURE0)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex)
        GL.glUniform1i(self._uniforms["u_tex"], 0)
        GL.glUniform1i(self._uniforms["u_fmt"], self._img_fmt)
        GL.glUniform1i(self._uniforms["u_width"], self._img_w)
        GL.glUniform1i(self._uniforms["u_height"], self._img_h)
        GL.glUniform1f(self._uniforms["u_black"], self._black)
        GL.glUniform1f(self._uniforms["u_white"], self._white)
        GL.glUniform1f(self._uniforms["u_gamma"], self._gamma)
        GL.glUniform1i(self._uniforms["u_colormap"], self._colormap)
        GL.glUniform2f(self._uniforms["u_view_size"], float(vw), float(vh))
        GL.glUniform2f(self._uniforms["u_pan"], float(self._pan.x()), float(self._pan.y()))
        GL.glUniform1f(self._uniforms["u_zoom"], float(self._zoom))
        GL.glUniform1i(self._uniforms["u_has_frame"], 1 if self._has_frame else 0)
        GL.glUniform2f(self._uniforms["u_spot_pos"], self._spot_pos[0], self._spot_pos[1])
        GL.glUniform1f(self._uniforms["u_spot_rad"], self._spot_rad)

        GL.glBindVertexArray(self._vao)
        GL.glDrawArrays(GL.GL_TRIANGLES, 0, 3)
        GL.glBindVertexArray(0)

    def wheelEvent(self, event: QWheelEvent) -> None:
        delta = event.angleDelta().y()
        if delta == 0:
            return
        factor = math.pow(1.15, delta / 120.0)
        self._zoom = max(0.2, min(32.0, self._zoom * factor))
        self.zoom_changed.emit(self._zoom)
        self.update()

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            if self._pick_mode or (event.modifiers() & Qt.KeyboardModifier.ShiftModifier):
                pt = self.widget_to_image(event.position())
                if pt is not None:
                    self.spot_clicked.emit(pt[0], pt[1])
                return
            self._dragging = True
            self._drag_last = event.position()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._dragging and self._img_w > 0 and self._img_h > 0:
            cur = event.position()
            dx = (cur.x() - self._drag_last.x()) / max(self.width(), 1)
            dy = (cur.y() - self._drag_last.y()) / max(self.height(), 1)
            self._drag_last = cur
            self._pan = QPointF(
                self._pan.x() + dx / self._zoom,
                self._pan.y() - dy / self._zoom,
            )
            self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = False
            self.setCursor(Qt.CursorShape.CrossCursor if self._pick_mode else Qt.CursorShape.ArrowCursor)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        self.reset_view()
