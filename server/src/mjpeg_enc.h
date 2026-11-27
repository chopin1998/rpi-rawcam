#ifndef RAWCAM_MJPEG_ENC_H
#define RAWCAM_MJPEG_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MJPEG_MAX_OUT_BUFS 2
#define MJPEG_MAX_CAP_BUFS 2
#define MJPEG_MAX_PLANES   3

typedef struct {
    void  *start[MJPEG_MAX_PLANES];
    size_t length[MJPEG_MAX_PLANES];
} MjpegMmapBuf;

typedef struct {
    int          fd;
    bool         streaming;
    char         dev_path[64];
    uint32_t     src_w;
    uint32_t     src_h;
    uint32_t     enc_w;
    uint32_t     enc_h;
    uint32_t     out_num_planes;
    uint32_t     y_stride;
    uint32_t     plane_size[MJPEG_MAX_PLANES];
    MjpegMmapBuf out_bufs[MJPEG_MAX_OUT_BUFS];
    uint32_t     out_count;
    MjpegMmapBuf cap_bufs[MJPEG_MAX_CAP_BUFS];
    uint32_t     cap_count;
} MjpegHwEncoder;

/* Callback invoked to fill the encoder's Y plane directly in MMAP memory. */
typedef void (*MjpegFillYFn)(uint8_t *dst_y, uint32_t dst_stride,
                             uint32_t copy_w, uint32_t copy_h, void *user_ctx);

void mjpeg_enc_init(MjpegHwEncoder *e);
bool mjpeg_enc_open(MjpegHwEncoder *e, uint32_t src_w, uint32_t src_h,
                    uint32_t quality, const char *dev_path);
void mjpeg_enc_close(MjpegHwEncoder *e);

/* Encodes one monochrome frame into a JFIF JPEG buffer. */
bool mjpeg_enc_encode(MjpegHwEncoder *e, MjpegFillYFn fill_y, void *user_ctx,
                      uint8_t **out_buf, size_t *out_cap, size_t *out_len);

#endif /* RAWCAM_MJPEG_ENC_H */
