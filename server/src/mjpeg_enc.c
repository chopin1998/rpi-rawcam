#include "mjpeg_enc.h"

#include <dirent.h>
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

static bool probe_jpeg_m2m_device(const char *path) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool ok = false;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
        if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
            struct v4l2_fmtdesc fdesc;
            for (uint32_t idx = 0; ; ++idx) {
                memset(&fdesc, 0, sizeof(fdesc));
                fdesc.index = idx;
                fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                if (xioctl(fd, VIDIOC_ENUM_FMT, &fdesc) < 0) break;
                if (fdesc.pixelformat == V4L2_PIX_FMT_JPEG) {
                    ok = true;
                    break;
                }
            }
        }
    }
    close(fd);
    return ok;
}

static bool discover_jpeg_encoder(char *out_path, size_t out_sz) {
    DIR *d = opendir("/sys/class/video4linux");
    if (!d) return false;
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "video", 5) != 0) continue;
        char cand[64];
        snprintf(cand, sizeof(cand), "/dev/%.50s", ent->d_name);
        if (probe_jpeg_m2m_device(cand)) {
            snprintf(out_path, out_sz, "%s", cand);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static bool qbuf_mplane(int fd, enum v4l2_buf_type type, uint32_t idx,
                        uint32_t num_planes, const uint32_t *bytesused) {
    struct v4l2_buffer b;
    struct v4l2_plane pls[MJPEG_MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(pls, 0, sizeof(pls));
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    b.length = num_planes;
    b.m.planes = pls;
    if (bytesused) {
        for (uint32_t p = 0; p < num_planes; ++p) {
            pls[p].bytesused = bytesused[p];
        }
    }
    return xioctl(fd, VIDIOC_QBUF, &b) == 0;
}

static bool alloc_mplane_bufs(int fd, enum v4l2_buf_type type, uint32_t req_count,
                              uint32_t num_planes, MjpegMmapBuf *bufs,
                              uint32_t *out_count, bool fill_neutral_uv) {
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
        struct v4l2_plane pls[MJPEG_MAX_PLANES];
        memset(&b, 0, sizeof(b));
        memset(pls, 0, sizeof(pls));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = num_planes;
        b.m.planes = pls;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return false;

        for (uint32_t p = 0; p < num_planes; ++p) {
            void *ptr = mmap(NULL, pls[p].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                             fd, pls[p].m.mem_offset);
            if (ptr == MAP_FAILED) return false;
            bufs[i].start[p] = ptr;
            bufs[i].length[p] = pls[p].length;
            if (fill_neutral_uv) {
                /* Plane 0 = Y (zero-init for any alignment padding), Plane 1..2 = U/V (0x80 neutral gray) */
                memset(ptr, (p == 0) ? 0x00 : 0x80, pls[p].length);
            }
        }
        *out_count = i + 1;
    }
    return true;
}

void mjpeg_enc_init(MjpegHwEncoder *e) {
    memset(e, 0, sizeof(*e));
    e->fd = -1;
}

bool mjpeg_enc_open(MjpegHwEncoder *e, uint32_t src_w, uint32_t src_h,
                    uint32_t quality, const char *dev_path) {
    mjpeg_enc_close(e);

    if (src_w < 96 || src_h < 32) return false;

    char discovered[64] = { 0 };
    if (dev_path && dev_path[0]) {
        snprintf(discovered, sizeof(discovered), "%s", dev_path);
    } else if (!discover_jpeg_encoder(discovered, sizeof(discovered))) {
        return false;
    }

    e->fd = open(discovered, O_RDWR | O_NONBLOCK);
    if (e->fd < 0) return false;
    snprintf(e->dev_path, sizeof(e->dev_path), "%s", discovered);
    e->src_w = src_w;
    e->src_h = src_h;

    if (quality < 5) quality = 5;
    if (quality > 100) quality = 100;
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = (int32_t)quality;
    xioctl(e->fd, VIDIOC_S_CTRL, &ctrl);

    /* Hantro RK3399 JPEG encoder requires 16-pixel aligned dimensions and
     * CAPTURE (coded JPEG) format MUST be configured BEFORE OUTPUT (raw YUV),
     * because setting CAPTURE resets OUTPUT format & plane count in hantro_v4l2. */
    uint32_t req_w = (src_w + 15u) & ~15u;
    uint32_t req_h = (src_h + 15u) & ~15u;

    struct v4l2_format cfmt;
    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cfmt.fmt.pix_mp.width = req_w;
    cfmt.fmt.pix_mp.height = req_h;
    cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    cfmt.fmt.pix_mp.num_planes = 1;
    cfmt.fmt.pix_mp.plane_fmt[0].sizeimage = req_w * req_h;
    if (xioctl(e->fd, VIDIOC_S_FMT, &cfmt) < 0) {
        fprintf(stderr, "[mjpeg] CAPTURE S_FMT failed on %s: %s\n", discovered, strerror(errno));
        mjpeg_enc_close(e);
        return false;
    }

    /* Prefer NV12M (2 planes: contiguous Y + neutral UV) and fall back to YUV420M (3 planes). */
    struct v4l2_format ofmt;
    bool ofmt_ok = false;
    const uint32_t fmts[2] = { V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_YUV420M };
    const uint32_t npls[2] = { 2, 3 };
    for (int k = 0; k < 2; ++k) {
        memset(&ofmt, 0, sizeof(ofmt));
        ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ofmt.fmt.pix_mp.width = cfmt.fmt.pix_mp.width;
        ofmt.fmt.pix_mp.height = cfmt.fmt.pix_mp.height;
        ofmt.fmt.pix_mp.pixelformat = fmts[k];
        ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        ofmt.fmt.pix_mp.num_planes = npls[k];
        if (xioctl(e->fd, VIDIOC_S_FMT, &ofmt) == 0 &&
            ofmt.fmt.pix_mp.pixelformat == fmts[k] &&
            ofmt.fmt.pix_mp.num_planes <= MJPEG_MAX_PLANES) {
            ofmt_ok = true;
            break;
        }
    }
    if (!ofmt_ok) {
        fprintf(stderr, "[mjpeg] OUTPUT S_FMT failed on %s: %s\n", discovered, strerror(errno));
        mjpeg_enc_close(e);
        return false;
    }

    e->enc_w = ofmt.fmt.pix_mp.width;
    e->enc_h = ofmt.fmt.pix_mp.height;
    e->out_num_planes = ofmt.fmt.pix_mp.num_planes;
    e->y_stride = ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    for (uint32_t p = 0; p < e->out_num_planes; ++p) {
        e->plane_size[p] = ofmt.fmt.pix_mp.plane_fmt[p].sizeimage;
    }

    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.width = src_w;
    sel.r.height = src_h;
    xioctl(e->fd, VIDIOC_S_SELECTION, &sel);

    if (!alloc_mplane_bufs(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                           MJPEG_MAX_OUT_BUFS, e->out_num_planes,
                           e->out_bufs, &e->out_count, true) ||
        !alloc_mplane_bufs(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                           MJPEG_MAX_CAP_BUFS, 1,
                           e->cap_bufs, &e->cap_count, false)) {
        fprintf(stderr, "[mjpeg] Buffer allocation failed on %s: %s\n", discovered, strerror(errno));
        mjpeg_enc_close(e);
        return false;
    }

    for (uint32_t i = 0; i < e->cap_count; ++i) {
        if (!qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 1, NULL)) {
            mjpeg_enc_close(e);
            return false;
        }
    }

    enum v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t_out) < 0 ||
        xioctl(e->fd, VIDIOC_STREAMON, &t_cap) < 0) {
        fprintf(stderr, "[mjpeg] STREAMON failed on %s: %s\n", discovered, strerror(errno));
        mjpeg_enc_close(e);
        return false;
    }

    e->streaming = true;
    printf("[mjpeg] Hardware JPEG encoder ready on %s (%ux%u -> %ux%u, q=%u, planes=%u)\n",
           e->dev_path, src_w, src_h, e->enc_w, e->enc_h, quality, e->out_num_planes);
    fflush(stdout);
    return true;
}

void mjpeg_enc_close(MjpegHwEncoder *e) {
    if (e->fd >= 0 && e->streaming) {
        enum v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        enum v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(e->fd, VIDIOC_STREAMOFF, &t_out);
        xioctl(e->fd, VIDIOC_STREAMOFF, &t_cap);
        e->streaming = false;
    }
    for (uint32_t i = 0; i < e->out_count; ++i) {
        for (uint32_t p = 0; p < MJPEG_MAX_PLANES; ++p) {
            if (e->out_bufs[i].start[p] && e->out_bufs[i].start[p] != MAP_FAILED) {
                munmap(e->out_bufs[i].start[p], e->out_bufs[i].length[p]);
                e->out_bufs[i].start[p] = NULL;
            }
        }
    }
    for (uint32_t i = 0; i < e->cap_count; ++i) {
        if (e->cap_bufs[i].start[0] && e->cap_bufs[i].start[0] != MAP_FAILED) {
            munmap(e->cap_bufs[i].start[0], e->cap_bufs[i].length[0]);
            e->cap_bufs[i].start[0] = NULL;
        }
    }
    e->out_count = 0;
    e->cap_count = 0;
    if (e->fd >= 0) {
        close(e->fd);
        e->fd = -1;
    }
    e->src_w = e->src_h = e->enc_w = e->enc_h = e->y_stride = e->out_num_planes = 0;
}

bool mjpeg_enc_encode(MjpegHwEncoder *e, MjpegFillYFn fill_y, void *user_ctx,
                      uint8_t **out_buf, size_t *out_cap, size_t *out_len) {
    if (e->fd < 0 || !e->streaming || e->out_count == 0) return false;

    uint32_t copy_w = e->src_w < e->enc_w ? e->src_w : e->enc_w;
    uint32_t copy_h = e->src_h < e->enc_h ? e->src_h : e->enc_h;
    uint8_t *dst_y = (uint8_t *)e->out_bufs[0].start[0];
    fill_y(dst_y, e->y_stride, copy_w, copy_h, user_ctx);

    if (!qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0,
                     e->out_num_planes, e->plane_size)) {
        return false;
    }

    bool got_out = false;
    bool got_cap = false;
    *out_len = 0;

    for (int tries = 0; tries < 20 && (!got_out || !got_cap); ++tries) {
        struct pollfd pfd = { e->fd, POLLIN | POLLOUT, 0 };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) break;

        if (!got_out && (pfd.revents & POLLOUT)) {
            struct v4l2_buffer b;
            struct v4l2_plane pls[MJPEG_MAX_PLANES];
            memset(&b, 0, sizeof(b));
            memset(pls, 0, sizeof(pls));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = e->out_num_planes;
            b.m.planes = pls;
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
                        memcpy(*out_buf, e->cap_bufs[b.index].start[0], len);
                        *out_len = len;
                        got_cap = true;
                    }
                }
                qbuf_mplane(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index, 1, NULL);
            }
        }
    }
    return got_cap && *out_len > 0;
}
