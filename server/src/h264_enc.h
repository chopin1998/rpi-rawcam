#ifndef RAWCAM_H264_ENC_H
#define RAWCAM_H264_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H264_MAX_OUT_BUFS 2
#define H264_MAX_CAP_BUFS 4

typedef struct {
    void  *start;
    size_t length;
} H264MmapBuf;

typedef struct {
    int         fd;
    bool        streaming;
    uint32_t    enc_w;
    uint32_t    enc_h;
    uint32_t    y_stride;
    uint32_t    y_size;
    H264MmapBuf out_bufs[H264_MAX_OUT_BUFS];
    uint32_t    out_count;
    H264MmapBuf cap_bufs[H264_MAX_CAP_BUFS];
    uint32_t    cap_count;
} H264HwEncoder;

/* Callback invoked to fill the encoder's Y plane directly in MMAP memory. */
typedef void (*H264FillYFn)(uint8_t *dst_y, uint32_t dst_stride,
                            uint32_t enc_w, uint32_t enc_h, void *user_ctx);

void h264_enc_init(H264HwEncoder *e);
bool h264_enc_open(H264HwEncoder *e, uint32_t src_w, uint32_t src_h,
                   uint32_t bitrate, uint32_t gop, const char *dev_path);
void h264_enc_close(H264HwEncoder *e);
void h264_enc_force_keyframe(H264HwEncoder *e);

/* Encodes one frame. Caller provides a growable buffer (*out_buf, *out_cap). */
bool h264_enc_encode(H264HwEncoder *e, H264FillYFn fill_y, void *user_ctx,
                     uint8_t **out_buf, size_t *out_cap, size_t *out_len,
                     bool *is_keyframe);

#endif /* RAWCAM_H264_ENC_H */
