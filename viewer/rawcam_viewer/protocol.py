"""Wire protocol definitions and vectorized lossless RAW unpackers."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

import numpy as np
from PIL import Image

FRAME_MAGIC = b"RAWC"
HEADER_STRUCT = struct.Struct("<4sHHIIIIIIQQIIHHHH")
assert HEADER_STRUCT.size == 64


class PixelFormat(IntEnum):
    Y10P = 1        # 10-bit MIPI packed (5 bytes per 4 pixels), lossless
    Y12P = 2        # 12-bit MIPI packed (3 bytes per 2 pixels), lossless
    Y10 = 3         # 10-bit unpacked uint16 LE, lossless
    Y12 = 4         # 12-bit unpacked uint16 LE, lossless
    Y8_PREVIEW = 5  # 8-bit preview (lossy display mode)
    H264 = 6        # Hardware H.264 Annex-B access unit (lossy display mode)


FLAG_LOSSLESS = 1 << 0
FLAG_PREVIEW = 1 << 1
FLAG_KEYFRAME = 1 << 2
FLAG_RESTORE_WARNING = 1 << 3


@dataclass(slots=True)
class FrameHeader:
    magic: bytes
    version: int
    pixel_format: PixelFormat
    flags: int
    width: int
    height: int
    stride: int
    payload_size: int
    exposure_us: int
    frame_seq: int
    timestamp_ns: int
    gain_db_x10: int
    fps_x100: int
    bit_depth: int
    roi_x: int
    roi_y: int
    reserved1: int

    @property
    def is_lossless(self) -> bool:
        return bool(self.flags & FLAG_LOSSLESS)

    @property
    def max_value(self) -> int:
        if self.pixel_format in (PixelFormat.Y8_PREVIEW, PixelFormat.H264):
            return 255
        return (1 << self.bit_depth) - 1

    @classmethod
    def unpack(cls, buf: bytes | memoryview) -> FrameHeader:
        fields = HEADER_STRUCT.unpack(buf)
        if fields[0] != FRAME_MAGIC:
            raise ValueError(f"Invalid frame magic: {fields[0]!r}")
        return cls(
            magic=fields[0],
            version=fields[1],
            pixel_format=PixelFormat(fields[2]),
            flags=fields[3],
            width=fields[4],
            height=fields[5],
            stride=fields[6],
            payload_size=fields[7],
            exposure_us=fields[8],
            frame_seq=fields[9],
            timestamp_ns=fields[10],
            gain_db_x10=fields[11],
            fps_x100=fields[12],
            bit_depth=fields[13],
            roi_x=fields[14],
            roi_y=fields[15],
            reserved1=fields[16],
        )


@dataclass(slots=True)
class FramePacket:
    header: FrameHeader
    payload: bytes
    restore_warning: str = ""


def unpack_to_u16(frame: FramePacket) -> np.ndarray:
    """Vectorized lossless unpack to 2D uint16 array (height, width).

    Used on demand for saving lossless frames, auto-stretch statistics,
    and future real-time analysis plugins. Live display unpacks on the GPU.
    """
    h = frame.header.height
    w = frame.header.width
    stride = frame.header.stride
    fmt = frame.header.pixel_format
    raw = np.frombuffer(frame.payload, dtype=np.uint8)
    rows = raw[: h * stride].reshape(h, stride)

    if fmt == PixelFormat.Y12P:
        # 3 bytes -> 2 pixels: p0 = (b0 << 4) | (b2 & 0x0F), p1 = (b1 << 4) | (b2 >> 4)
        pairs = w // 2
        packed = rows[:, : pairs * 3].reshape(h, pairs, 3).astype(np.uint16)
        out = np.empty((h, pairs * 2), dtype=np.uint16)
        out[:, 0::2] = (packed[:, :, 0] << 4) | (packed[:, :, 2] & 0x0F)
        out[:, 1::2] = (packed[:, :, 1] << 4) | (packed[:, :, 2] >> 4)
        return out

    if fmt == PixelFormat.Y10P:
        # 5 bytes -> 4 pixels: p_k = (b_k << 2) | ((b4 >> (2*k)) & 0x03)
        quads = w // 4
        packed = rows[:, : quads * 5].reshape(h, quads, 5).astype(np.uint16)
        out = np.empty((h, quads * 4), dtype=np.uint16)
        b4 = packed[:, :, 4]
        out[:, 0::4] = (packed[:, :, 0] << 2) | (b4 & 0x03)
        out[:, 1::4] = (packed[:, :, 1] << 2) | ((b4 >> 2) & 0x03)
        out[:, 2::4] = (packed[:, :, 2] << 2) | ((b4 >> 4) & 0x03)
        out[:, 3::4] = (packed[:, :, 3] << 2) | ((b4 >> 6) & 0x03)
        return out

    if fmt in (PixelFormat.Y10, PixelFormat.Y12):
        return rows[:, : w * 2].view(np.uint16).copy()

    if fmt == PixelFormat.Y8_PREVIEW:
        return rows[:, :w].astype(np.uint16)

    raise ValueError(f"Unsupported pixel format: {fmt}")


def save_frame(frame: FramePacket, path: str | Path) -> Path:
    """Save frame losslessly as 16-bit PNG, 16-bit TIFF, or raw binary."""
    out_path = Path(path)
    suffix = out_path.suffix.lower()
    if suffix == ".raw":
        out_path.write_bytes(frame.payload)
        return out_path

    arr = unpack_to_u16(frame)
    if frame.header.pixel_format == PixelFormat.Y8_PREVIEW:
        img = Image.fromarray(arr.astype(np.uint8), mode="L")
    else:
        # Preserve exact 10/12-bit integer values in 16-bit PNG/TIFF (Pillow infers I;16 from uint16)
        img = Image.fromarray(arr)
    img.save(out_path)
    return out_path
