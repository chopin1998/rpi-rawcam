#ifndef RAWCAM_PROTOCOL_H
#define RAWCAM_PROTOCOL_H

#include <stdint.h>

#define RAWCAM_FRAME_MAGIC_0 'R'
#define RAWCAM_FRAME_MAGIC_1 'A'
#define RAWCAM_FRAME_MAGIC_2 'W'
#define RAWCAM_FRAME_MAGIC_3 'C'
#define RAWCAM_PROTOCOL_VERSION 1

typedef enum {
    PIXFMT_Y10P       = 1, /* 10-bit MIPI packed (5 bytes per 4 pixels), lossless */
    PIXFMT_Y12P       = 2, /* 12-bit MIPI packed (3 bytes per 2 pixels), lossless */
    PIXFMT_Y10        = 3, /* 10-bit unpacked uint16 LE, lossless */
    PIXFMT_Y12        = 4, /* 12-bit unpacked uint16 LE, lossless */
    PIXFMT_Y8_PREVIEW = 5, /* 8-bit top-bit preview (lossy display mode) */
    PIXFMT_H264       = 6, /* Hardware H.264 Annex-B access unit (lossy display mode) */
    PIXFMT_MJPEG      = 7, /* Hardware JPEG / MJPEG frame (lossy display mode) */
} PixelFormat;

#define FLAG_LOSSLESS (1u << 0)
#define FLAG_PREVIEW  (1u << 1)
#define FLAG_KEYFRAME (1u << 2)
/* Single shot: payload followed by a UTF-8 warning line. Without a frame,
 * send "ERR " and the error line instead of a RAWC header. */
#define FLAG_RESTORE_WARNING (1u << 3)

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint16_t pixel_format;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t payload_size;
    uint32_t exposure_us;
    uint64_t frame_seq;
    uint64_t timestamp_ns;
    uint32_t gain_db_x10;
    uint32_t fps_x100;
    uint16_t bit_depth;
    uint16_t roi_x;
    uint16_t roi_y;
    uint16_t reserved1;
} FrameHeader;

_Static_assert(sizeof(FrameHeader) == 64, "FrameHeader must be exactly 64 bytes");

#endif /* RAWCAM_PROTOCOL_H */
