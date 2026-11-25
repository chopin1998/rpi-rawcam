#include "h264_enc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void set_ctrl(int fd, uint32_t id, int32_t val) {
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.value = val;
    xioctl(fd, VIDIOC_S_CTRL, &c);
}

static bool qbuf_mplane(int fd, enum v4l2_buf_type type, uint32_t idx, uint32_t bytesused) {
    struct v4l2_buffer b;
    struct v4l2_plane pl;
    memset(&b, 0, sizeof(b));
    memset(&pl, 0, sizeof(pl));
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    b.length = 1;
    b.m.planes = &pl;
    if (bytesused > 0) {
        pl.bytesused = bytesused;
    }
    return xioctl(fd, VIDIOC_QBUF, &b) == 0;
}

static bool alloc_mplane_bufs(int fd, enum v4l2_buf_type type, uint32_t req_count,
                              H264MmapBuf *bufs, uint32_t *out_count, bool fill_neutral_uv) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = req_count;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        return false;
    }
    if (req.count > req_count) req.count = req_count;

    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer b;
        struct v4l2_plane pl;
        memset(&b, 0, sizeof(b));
        memset(&pl, 0, sizeof(pl));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &pl;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return false;

        void *ptr = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, pl.m.mem_offset);
        if (ptr == MAP_FAILED) return false;

        bufs[i].start = ptr;
        bufs[i].length = pl.length;
        *out_count = i + 1;

        if (fill_neutral_uv) {
            /* Pre-fill U/V planes with 0x80 for monochrome video */
            memset(ptr, 0x80, pl.length);
        }
    }
    return true;
}

void h264_enc_init(H264HwEncoder *e) {
    memset(e, 0, sizeof(*e));
    e->fd = -1;
}

bool h264_enc_open(H264HwEncoder *e, uint32_t src_w, uint32_t src_h,
                   uint32_t bitrate, uint32_t gop, const char *dev_path) {
    h264_enc_close(e);

    /* bcm2835-codec-encode supports up to 1920x1920, step 2 */
    uint32_t ew = (src_w > 1920 ? 1920 : src_w) & ~1u;
    uint32_t eh = (src_h > 1920 ? 1920 : src_h) & ~1u;
    if (ew < 32 || eh < 32) return false;

    e->fd = open(dev_path ? dev_path : "/dev/video11", O_RDWR | O_NONBLOCK);
    if (e->fd < 0) return false;
    e->enc_w = ew;
    e->enc_h = eh;

    /* Configure codec controls (Level 4.2 supports 1920x1096 macroblock count) */
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t)gop);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)gop);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 16);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 42);

    /* Set OUTPUT (raw YU12 input) format first */
    struct v4l2_format ofmt;
    memset(&ofmt, 0, sizeof(ofmt));
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ofmt.fmt.pix_mp.width = ew;
    ofmt.fmt.pix_mp.height = eh;
    ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    ofmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(e->fd, VIDIOC_S_FMT, &ofmt) < 0) {
        fprintf(stderr, "[h264] OUTPUT S_FMT failed: %s\n", strerror(errno));
        h264_enc_close(e);
        return false;
    }
    e->y_stride = ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    e->y_size   = ofmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    /* Set CAPTURE (encoded H264 output) format */
    struct v4l2_format cfmt;
    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cfmt.fmt.pix_mp.width = ew;
    cfmt.fmt.pix_mp.height = eh;
    cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    cfmt.fmt.pix_mp.num_planes = 1;
    cfmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
    if (xioctl(e->fd, VIDIOC_S_FMT, &cfmt) < 0) {
        fprintf(stderr, "[h264] CAPTURE S_FMT failed: %s\n", strerror(errno));
        h264_enc_close(e);
        return false;
    }

    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.width = ew;
    sel.r.height = eh;
    xioctl(e->fd, VIDIOC_S_SELECTION, &sel);

    if (!alloc_mplane_bufs(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                           H264_MAX_OUT_BUFS, e->out_bufs, &e->out_count, true) ||
        !alloc_mplane_bufs(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                           H264_MAX_CAP_BUFS, e->cap_bufs, &e->cap_count, false)) {
        h264_enc_close(e);
        return false;
    }

    for (uint32_t i = 0; i < e->cap_count; ++i) {
        if (!qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 0)) {
            h264_enc_close(e);
            return false;
        }
    }

    enum v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t_out) < 0 ||
        xioctl(e->fd, VIDIOC_STREAMON, &t_cap) < 0) {
        fprintf(stderr, "[h264] STREAMON failed: %s\n", strerror(errno));
        h264_enc_close(e);
        return false;
    }

    e->streaming = true;
    return true;
}

void h264_enc_close(H264HwEncoder *e) {
    if (e->fd >= 0 && e->streaming) {
        enum v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        enum v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(e->fd, VIDIOC_STREAMOFF, &t_out);
        xioctl(e->fd, VIDIOC_STREAMOFF, &t_cap);
        e->streaming = false;
    }
    for (uint32_t i = 0; i < e->out_count; ++i) {
        if (e->out_bufs[i].start && e->out_bufs[i].start != MAP_FAILED) {
            munmap(e->out_bufs[i].start, e->out_bufs[i].length);
        }
    }
    for (uint32_t i = 0; i < e->cap_count; ++i) {
        if (e->cap_bufs[i].start && e->cap_bufs[i].start != MAP_FAILED) {
            munmap(e->cap_bufs[i].start, e->cap_bufs[i].length);
        }
    }
    e->out_count = 0;
    e->cap_count = 0;
    if (e->fd >= 0) {
        close(e->fd);
        e->fd = -1;
    }
    e->enc_w = e->enc_h = e->y_stride = e->y_size = 0;
}

void h264_enc_force_keyframe(H264HwEncoder *e) {
    if (e->fd >= 0) {
        set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
    }
}

bool h264_enc_encode(H264HwEncoder *e, H264FillYFn fill_y, void *user_ctx,
                     uint8_t **out_buf, size_t *out_cap, size_t *out_len,
                     bool *is_keyframe) {
    if (e->fd < 0 || !e->streaming || e->out_count == 0) return false;

    uint8_t *dst_y = (uint8_t *)e->out_bufs[0].start;
    fill_y(dst_y, e->y_stride, e->enc_w, e->enc_h, user_ctx);

    if (!qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, e->y_size)) {
        return false;
    }

    bool got_out = false;
    bool got_cap = false;
    *out_len = 0;
    *is_keyframe = false;

    for (int tries = 0; tries < 20 && (!got_out || !got_cap); ++tries) {
        struct pollfd pfd = { e->fd, POLLIN | POLLOUT, 0 };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) break;

        if (!got_out && (pfd.revents & POLLOUT)) {
            struct v4l2_buffer b;
            struct v4l2_plane pl;
            memset(&b, 0, sizeof(b));
            memset(&pl, 0, sizeof(pl));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = &pl;
            if (xioctl(e->fd, VIDIOC_DQBUF, &b) == 0) {
                got_out = true;
            }
        }
        if (!got_cap && (pfd.revents & POLLIN)) {
            struct v4l2_buffer b;
            struct v4l2_plane pl;
            memset(&b, 0, sizeof(b));
            memset(&pl, 0, sizeof(pl));
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = &pl;
            if (xioctl(e->fd, VIDIOC_DQBUF, &b) == 0) {
                uint32_t len = pl.bytesused;
                if (len > 0 && b.index < e->cap_count) {
                    if (len > *out_cap) {
                        uint8_t *nb = (uint8_t *)realloc(*out_buf, len);
                        if (nb) {
                            *out_buf = nb;
                            *out_cap = len;
                        }
                    }
                    if (*out_buf && *out_cap >= len) {
                        memcpy(*out_buf, e->cap_bufs[b.index].start, len);
                        *out_len = len;
                        *is_keyframe = (b.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
                        got_cap = true;
                    }
                }
                qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index, 0);
            }
        }
    }
    return got_cap && *out_len > 0;
}
