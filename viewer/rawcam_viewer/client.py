"""Network clients for RPi RAWCam control (5600), stream + H.264 HW decode (5601), and 12-bit snapshot (5602)."""

from __future__ import annotations

import json
import os
import socket
import sys
import threading
import time
from typing import Any

import av
from PySide6.QtCore import QThread, Signal

from .protocol import FLAG_RESTORE_WARNING, FrameHeader, FramePacket, HEADER_STRUCT, PixelFormat


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray(n)
    view = memoryview(buf)
    got = 0
    while got < n:
        chunk = sock.recv_into(view[got:], n - got)
        if chunk == 0:
            raise ConnectionError("Connection closed by server")
        got += chunk
    return bytes(buf)


def grab_lossless_snapshot(host: str, port: int = 5602, timeout: float = 5.0) -> FramePacket:
    """Connect to RPi snapshot port (5602) and fetch one genuine 10/12-bit lossless RAW frame."""
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout)
        sock.sendall(b'{"cmd":"snap"}\n')
        hdr_bytes = _recv_exact(sock, HEADER_STRUCT.size)
        hdr = FrameHeader.unpack(hdr_bytes)
        if hdr.payload_size <= 0 or hdr.payload_size > 32 * 1024 * 1024:
            raise ValueError(f"Invalid snapshot payload_size: {hdr.payload_size}")
        payload = _recv_exact(sock, hdr.payload_size)
        return FramePacket(header=hdr, payload=payload)


def grab_long_shot(host: str, exposure_us: int, gain_db: float, port: int = 5602) -> FramePacket:
    """Request a dedicated long-exposure (>500ms) single-shot 10/12-bit lossless frame."""
    timeout = max(6.0, exposure_us / 1_000_000.0 + 6.0)
    req = (json.dumps({
        "cmd": "long_shot",
        "exposure_us": int(exposure_us),
        "gain_db": float(gain_db),
    }, separators=(",", ":")) + "\n").encode("utf-8")
    with socket.create_connection((host, port), timeout=5.0) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout)
        sock.sendall(req)
        prefix = _recv_exact(sock, 4)
        if prefix == b"ERR ":
            raise RuntimeError(_read_shot_message(sock))
        hdr_bytes = prefix + _recv_exact(sock, HEADER_STRUCT.size - len(prefix))
        hdr = FrameHeader.unpack(hdr_bytes)
        if hdr.payload_size <= 0 or hdr.payload_size > 32 * 1024 * 1024:
            raise ValueError(f"Invalid long-shot payload_size: {hdr.payload_size}")
        payload = _recv_exact(sock, hdr.payload_size)
        warning = ""
        if hdr.flags & FLAG_RESTORE_WARNING:
            try:
                warning = _read_shot_message(sock)
            except (OSError, ValueError):
                warning = "Live settings or stream could not be restored"
        return FramePacket(header=hdr, payload=payload, restore_warning=warning)


def _read_shot_message(sock: socket.socket) -> str:
    with sock.makefile("rb") as reader:
        line = reader.readline(512)
    if not line.endswith(b"\n") or not line.strip():
        raise ValueError("Incomplete single-shot status message")
    return line.decode("utf-8", errors="replace").strip()



class LongShotWorker(QThread):
    """Non-blocking worker thread for long-exposure (>500ms) single-shot capture."""

    finished_frame = Signal(object)  # FramePacket
    failed = Signal(str)

    def __init__(self, host: str, port: int, exposure_us: int, gain_db: float, parent=None) -> None:
        super().__init__(parent)
        self._host = host
        self._port = port
        self._exposure_us = exposure_us
        self._gain_db = gain_db

    def run(self) -> None:
        try:
            frame = grab_long_shot(self._host, self._exposure_us, self._gain_db, self._port)
            self.finished_frame.emit(frame)
        except Exception as exc:
            self.failed.emit(str(exc))


class H264Decoder:
    """Low-latency H.264 Annex-B decoder with cross-platform HW accel + safe SW fallback."""

    def __init__(self) -> None:
        self._codec = None
        self.hw_active = False
        self._hw_disabled = False
        self._open(try_hw=True)

    def _open(self, try_hw: bool) -> None:
        self.hw_active = False
        if try_hw and not self._hw_disabled:
            candidates: list[tuple[str, str | None]] = []
            if sys.platform.startswith("linux") and os.path.exists("/dev/dri/renderD128"):
                candidates.append(("vaapi", "/dev/dri/renderD128"))
            elif sys.platform == "darwin":
                candidates.append(("videotoolbox", None))
            elif sys.platform == "win32":
                candidates.append(("d3d11va", None))

            for dev_type, dev_path in candidates:
                try:
                    from av.codec.hwaccel import HWAccel

                    hw = HWAccel(
                        device_type=dev_type,
                        device=dev_path,
                        allow_software_fallback=False,
                    )
                    ctx = av.CodecContext.create("h264", "r", hwaccel=hw)
                    ctx.flags |= av.codec.context.Flags.low_delay
                    self._codec = ctx
                    self.hw_active = True
                    return
                except Exception:
                    continue

        # Fallback: multi-threaded software H.264 decoder
        ctx = av.CodecContext.create("h264", "r")
        ctx.thread_type = "SLICE"
        ctx.flags |= av.codec.context.Flags.low_delay
        self._codec = ctx

    def reset(self, disable_hw: bool = False) -> None:
        if disable_hw:
            self._hw_disabled = True
        self._open(try_hw=not self._hw_disabled)

    def decode_y8(self, nal_bytes: bytes) -> tuple[int, int, int, bytes] | None:
        """Decode one H.264 access unit and return (width, height, y_stride, y_bytes)."""
        if self._codec is None:
            return None
        pkt = av.Packet(nal_bytes)
        try:
            frames = self._codec.decode(pkt)
        except av.error.InvalidDataError:
            return None
        except Exception:
            # If HW decode failed at runtime, permanently fall back to SW decode
            if self.hw_active:
                self.reset(disable_hw=True)
                try:
                    frames = self._codec.decode(pkt)
                except Exception:
                    return None
            else:
                self.reset(disable_hw=False)
                return None

        if not frames:
            return None
        frame = frames[-1]
        fmt_name = frame.format.name
        if fmt_name in ("yuv420p", "yuvj420p", "nv12", "nv21", "gray"):
            plane0 = frame.planes[0]
            return frame.width, frame.height, plane0.line_size, bytes(plane0)

        # Hardware surface (vaapi / videotoolbox / d3d11 / etc.): download Y plane as gray
        gray = frame.to_ndarray(format="gray")
        h, w = gray.shape
        return w, h, w, gray.tobytes()


class ControlClient:
    """Thread-safe newline-delimited JSON control client (port 5600)."""

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._file = None
        self._lock = threading.Lock()

    def connect(self, host: str, port: int = 5600, timeout: float = 3.0) -> dict[str, Any]:
        with self._lock:
            self._close_locked()
            sock = socket.create_connection((host, port), timeout=timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(5.0)
            self._sock = sock
            self._file = sock.makefile("rwb", buffering=0)
        return self.send_cmd({"cmd": "get_status"})

    def close(self) -> None:
        with self._lock:
            self._close_locked()

    def _close_locked(self) -> None:
        if self._file is not None:
            try:
                self._file.close()
            except OSError:
                pass
            self._file = None
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def send_cmd(self, payload: dict[str, Any]) -> dict[str, Any]:
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
        with self._lock:
            if self._sock is None or self._file is None:
                raise ConnectionError("Not connected to camera control server")
            self._sock.sendall(data)
            line = self._file.readline()
            if not line:
                self._close_locked()
                raise ConnectionError("Control server closed connection")
            resp = json.loads(line.decode("utf-8"))
            if not resp.get("ok", False):
                raise RuntimeError(resp.get("error", "Unknown camera error"))
            return resp["status"]


class ControlWorker(QThread):
    """Non-blocking background worker for camera control RPCs with command coalescing."""

    status_received = Signal(dict, bool)  # status, sync_inputs
    error_occurred = Signal(str)

    def __init__(self, ctrl: ControlClient, parent=None) -> None:
        super().__init__(parent)
        self._ctrl = ctrl
        self._stop_event = threading.Event()
        self._cv = threading.Condition()
        self._pending: dict[str, tuple[dict[str, Any], bool]] = {}
        self._order: list[str] = []

    def enqueue(self, payload: dict[str, Any], sync_inputs: bool = False) -> None:
        cmd = str(payload.get("cmd", ""))
        with self._cv:
            if cmd not in self._pending:
                self._order.append(cmd)
            self._pending[cmd] = (payload, sync_inputs)
            self._cv.notify()

    def stop(self) -> None:
        self._stop_event.set()
        with self._cv:
            self._cv.notify_all()

    def run(self) -> None:
        while not self._stop_event.is_set():
            with self._cv:
                while not self._order and not self._stop_event.is_set():
                    self._cv.wait(timeout=0.25)
                if self._stop_event.is_set():
                    break
                cmd = self._order.pop(0)
                payload, sync_inputs = self._pending.pop(cmd)

            if not self._ctrl.connected:
                continue
            try:
                st = self._ctrl.send_cmd(payload)
                self.status_received.emit(st, sync_inputs)
            except Exception as exc:
                if not self._stop_event.is_set():
                    self.error_occurred.emit(f"{payload.get('cmd')}: {exc}")


class StreamReceiver(QThread):
    """High-throughput binary frame receiver + hardware H.264 decoder thread (port 5601)."""

    frame_available = Signal()
    stats_updated = Signal(float, float, int, str)  # rx_fps, mb_per_s, dropped_seq, decode_tag
    connection_lost = Signal(str)

    def __init__(self, host: str, port: int = 5601, parent=None) -> None:
        super().__init__(parent)
        self._host = host
        self._port = port
        self._stop_event = threading.Event()
        self._sock: socket.socket | None = None
        self._frame_lock = threading.Lock()
        self._latest_frame: FramePacket | None = None

    def take_latest_frame(self) -> FramePacket | None:
        with self._frame_lock:
            frame = self._latest_frame
            self._latest_frame = None
            return frame

    def stop(self) -> None:
        self._stop_event.set()
        sock = self._sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    def run(self) -> None:
        try:
            sock = socket.create_connection((self._host, self._port), timeout=3.0)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
            except OSError:
                pass
            sock.settimeout(2.0)
            self._sock = sock
        except OSError as exc:
            if not self._stop_event.is_set():
                self.connection_lost.emit(f"Stream connect failed: {exc}")
            return

        decoder: H264Decoder | None = None
        last_h264_dims = (0, 0)

        t0 = time.monotonic()
        frames = 0
        total_bytes = 0
        last_seq = 0
        dropped = 0
        decode_tag = ""

        try:
            while not self._stop_event.is_set():
                try:
                    hdr_bytes = _recv_exact(sock, HEADER_STRUCT.size)
                except socket.timeout:
                    continue
                hdr = FrameHeader.unpack(hdr_bytes)
                if hdr.payload_size > 32 * 1024 * 1024:
                    raise ValueError(f"Unreasonable payload_size: {hdr.payload_size}")
                payload = _recv_exact(sock, hdr.payload_size)

                if last_seq > 0 and hdr.frame_seq > last_seq + 1:
                    dropped += hdr.frame_seq - last_seq - 1
                last_seq = hdr.frame_seq
                total_bytes += HEADER_STRUCT.size + hdr.payload_size

                if hdr.pixel_format == PixelFormat.H264:
                    if decoder is None or last_h264_dims != (hdr.width, hdr.height):
                        decoder = H264Decoder()
                        last_h264_dims = (hdr.width, hdr.height)
                    dec = decoder.decode_y8(payload)
                    if dec is None:
                        continue
                    dw, dh, dstride, y_bytes = dec
                    hdr.pixel_format = PixelFormat.Y8_PREVIEW
                    hdr.width = dw
                    hdr.height = dh
                    hdr.stride = dstride
                    hdr.payload_size = len(y_bytes)
                    payload = y_bytes
                    decode_tag = "H264 VA-API" if decoder.hw_active else "H264 SW"
                else:
                    decode_tag = "LOSSLESS" if hdr.is_lossless else "PREVIEW-8"

                pkt = FramePacket(header=hdr, payload=payload)
                with self._frame_lock:
                    was_empty = self._latest_frame is None
                    self._latest_frame = pkt
                if was_empty:
                    self.frame_available.emit()

                frames += 1
                now = time.monotonic()
                dt = now - t0
                if dt >= 0.5:
                    rx_fps = frames / dt
                    mb_s = (total_bytes / (1024.0 * 1024.0)) / dt
                    self.stats_updated.emit(rx_fps, mb_s, dropped, decode_tag)
                    frames = 0
                    total_bytes = 0
                    t0 = now
        except Exception as exc:
            if not self._stop_event.is_set():
                self.connection_lost.emit(str(exc))
        finally:
            try:
                sock.close()
            except OSError:
                pass
            self._sock = None
