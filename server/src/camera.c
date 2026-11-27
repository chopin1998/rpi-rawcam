#include "camera.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#define V4L2_CID_VEYE_MV_BASE         (V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_VEYE_MV_TRIGGER_MODE (V4L2_CID_VEYE_MV_BASE + 1)
#define V4L2_CID_VEYE_MV_ROI_X        (V4L2_CID_VEYE_MV_BASE + 5)
#define V4L2_CID_VEYE_MV_ROI_Y        (V4L2_CID_VEYE_MV_BASE + 6)

#define ROI_W_ALIGN 8u
#define ROI_H_ALIGN 4u

/* ---------------- ByteBuf & FrameBuffer helpers ---------------- */

void bytebuf_init(ByteBuf *b) {
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

bool bytebuf_reserve(ByteBuf *b, size_t n) {
    if (n <= b->cap) return true;
    uint8_t *p = (uint8_t *)realloc(b->data, n);
    if (!p) return false;
    b->data = p;
    b->cap = n;
    return true;
}

bool bytebuf_assign(ByteBuf *b, const void *src, size_t n) {
    if (!bytebuf_reserve(b, n)) return false;
    if (n > 0) memcpy(b->data, src, n);
    b->size = n;
    return true;
}

void bytebuf_free(ByteBuf *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

void framebuf_init(FrameBuffer *f) {
    memset(&f->header, 0, sizeof(f->header));
    bytebuf_init(&f->data);
}

void framebuf_free(FrameBuffer *f) {
    bytebuf_free(&f->data);
}

/* ---------------- Low-level helpers ---------------- */

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clamp_dbl(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct timespec monotonic_deadline_ms(int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void init_monotonic_cond(pthread_cond_t *cv) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &attr);
    pthread_condattr_destroy(&attr);
}

static bool read_trim_file(const char *path, char *out, size_t out_sz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(out, (int)out_sz, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return len > 0;
}

/* ---------------- NEON / scalar top-8-bit extractors ---------------- */

static void extract_y12p_to_y8_strided(const uint8_t *src, uint32_t src_stride,
                                       uint8_t *dst, uint32_t dst_stride,
                                       uint32_t width, uint32_t height,
                                       uint32_t x_offset_pairs) {
    const uint32_t pairs = width / 2;
    const uint32_t byte_off = x_offset_pairs * 3;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_stride + byte_off;
        uint8_t *d = dst + (size_t)y * dst_stride;
        uint32_t x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
        for (; x + 16 <= pairs; x += 16) {
            uint8x16x3_t v = vld3q_u8(s + 3 * x);
            uint8x16x2_t out = { { v.val[0], v.val[1] } };
            vst2q_u8(d + 2 * x, out);
        }
#endif
        for (; x < pairs; ++x) {
            d[2 * x]     = s[3 * x];
            d[2 * x + 1] = s[3 * x + 1];
        }
    }
}

static void extract_y10p_to_y8_strided(const uint8_t *src, uint32_t src_stride,
                                       uint8_t *dst, uint32_t dst_stride,
                                       uint32_t width, uint32_t height,
                                       uint32_t x_offset_quads) {
    const uint32_t quads = width / 4;
    const uint32_t byte_off = x_offset_quads * 5;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_stride + byte_off;
        uint8_t *d = dst + (size_t)y * dst_stride;
        for (uint32_t x = 0; x < quads; ++x) {
            d[4 * x]     = s[5 * x];
            d[4 * x + 1] = s[5 * x + 1];
            d[4 * x + 2] = s[5 * x + 2];
            d[4 * x + 3] = s[5 * x + 3];
        }
    }
}

/* RK3399 RKISP1_V10 writes 16-bit Big-Endian MSB-aligned RAW10/RAW12:
 *   byte0 = D[bits-1 : bits-8] (high 8 bits)
 *   byte1 = D[bits-9 : 0] << (16 - bits)
 * Extract high 8 bits for preview8 / hardware JPEG Y plane. */
static void extract_rkisp1_be_to_y8_strided(const uint8_t *src, uint32_t src_stride,
                                            uint8_t *dst, uint32_t dst_stride,
                                            uint32_t width, uint32_t height,
                                            uint32_t x_offset_px) {
    const uint32_t byte_off = x_offset_px * 2;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_stride + byte_off;
        uint8_t *d = dst + (size_t)y * dst_stride;
        uint32_t x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
        for (; x + 16 <= width; x += 16) {
            uint8x16x2_t v = vld2q_u8(s + 2 * x);
            vst1q_u8(d + x, v.val[0]);
        }
#endif
        for (; x < width; ++x) {
            d[x] = s[2 * x];
        }
    }
}

/* Convert RK3399 RKISP1_V10 16-bit Big-Endian MSB-aligned RAW10/RAW12 into
 * standard V4L2 unpacked Little-Endian LSB-aligned uint16 (PIXFMT_Y10 / Y12). */
static void unpack_rkisp1_be_to_le_u16(const uint8_t *src, uint32_t src_stride,
                                       uint8_t *dst, uint32_t width, uint32_t height,
                                       uint16_t bit_depth) {
    const unsigned int shift = (bit_depth == 10) ? 6u : 4u;
    const uint32_t dst_stride = width * 2;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = src + (size_t)y * src_stride;
        uint16_t *d = (uint16_t *)(dst + (size_t)y * dst_stride);
        uint32_t x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
        if (bit_depth == 10) {
            for (; x + 8 <= width; x += 8) {
                uint8x16_t raw = vld1q_u8(s + 2 * x);
                uint16x8_t be  = vreinterpretq_u16_u8(vrev16q_u8(raw));
                vst1q_u16(d + x, vshrq_n_u16(be, 6));
            }
        } else {
            for (; x + 8 <= width; x += 8) {
                uint8x16_t raw = vld1q_u8(s + 2 * x);
                uint16x8_t be  = vreinterpretq_u16_u8(vrev16q_u8(raw));
                vst1q_u16(d + x, vshrq_n_u16(be, 4));
            }
        }
#endif
        for (; x < width; ++x) {
            uint16_t be = ((uint16_t)s[2 * x] << 8) | (uint16_t)s[2 * x + 1];
            d[x] = (uint16_t)(be >> shift);
        }
    }
}

typedef struct {
    const uint8_t *src;
    uint32_t       src_stride;
    uint32_t       src_w;
    uint16_t       bit_depth;
    CamBackend     backend;
} PreviewFillCtx;

static void fill_y_for_preview(uint8_t *dst_y, uint32_t dst_stride,
                               uint32_t ew, uint32_t eh, void *user_ctx) {
    PreviewFillCtx *ctx = (PreviewFillCtx *)user_ctx;
    if (ctx->backend == CAM_BACKEND_RKISP1) {
        uint32_t x_off = (ctx->src_w > ew) ? ((ctx->src_w - ew) / 2) : 0;
        extract_rkisp1_be_to_y8_strided(ctx->src, ctx->src_stride, dst_y, dst_stride, ew, eh, x_off);
    } else if (ctx->bit_depth == 10) {
        uint32_t x_off_quads = (ctx->src_w > ew) ? ((ctx->src_w - ew) / 2 / 4) : 0;
        extract_y10p_to_y8_strided(ctx->src, ctx->src_stride, dst_y, dst_stride, ew, eh, x_off_quads);
    } else {
        uint32_t x_off_pairs = (ctx->src_w > ew) ? ((ctx->src_w - ew) / 2 / 2) : 0;
        extract_y12p_to_y8_strided(ctx->src, ctx->src_stride, dst_y, dst_stride, ew, eh, x_off_pairs);
    }
}

/* ---------------- Device discovery ---------------- */

static bool discover_rkisp1_media(char *out_path, size_t out_sz) {
    for (int i = 0; i < 16; ++i) {
        char cand[64];
        snprintf(cand, sizeof(cand), "/dev/media%d", i);
        int fd = open(cand, O_RDWR);
        if (fd < 0) continue;
        struct media_device_info info;
        memset(&info, 0, sizeof(info));
        bool match = (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0 &&
                      strcmp(info.driver, "rkisp1") == 0);
        close(fd);
        if (match) {
            snprintf(out_path, out_sz, "%s", cand);
            return true;
        }
    }
    return false;
}

static bool discover_devices(Camera *cam, char *err, size_t err_sz) {
    char i2c_dev[64] = { 0 };
    char model[64] = "VEYE-RAWCAM";

    DIR *d = opendir("/sys/bus/i2c/devices");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *name = ent->d_name;
            size_t len = strlen(name);
            if (len > 5 && strcmp(name + len - 5, "-003b") == 0) {
                char bus[32];
                size_t bus_len = len - 5;
                if (bus_len >= sizeof(bus)) bus_len = sizeof(bus) - 1;
                memcpy(bus, name, bus_len);
                bus[bus_len] = '\0';
                snprintf(i2c_dev, sizeof(i2c_dev), "/dev/i2c-%s", bus);

                char mpath[256];
                snprintf(mpath, sizeof(mpath), "/sys/bus/i2c/devices/%s/veye_mvcam/camera_model", name);
                read_trim_file(mpath, model, sizeof(model));
                break;
            }
        }
        closedir(d);
    }
    if (!i2c_dev[0]) {
        snprintf(err, err_sz, "VEYE camera (I2C 0x3b) not found. Load driver first.");
        return false;
    }

    char unicam_dev[64] = { 0 };
    char rkisp1_dev[64] = { 0 };
    char subdev_dev[64] = { 0 };
    char csi_subdev[64] = { 0 };
    char isp_subdev[64] = { 0 };
    char rsz_mp_subdev[64] = { 0 };

    d = opendir("/sys/class/video4linux");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char npath[320], vname[128];
            snprintf(npath, sizeof(npath), "/sys/class/video4linux/%.200s/name", ent->d_name);
            if (!read_trim_file(npath, vname, sizeof(vname))) continue;
            if (!unicam_dev[0] && strcmp(vname, "unicam-image") == 0) {
                snprintf(unicam_dev, sizeof(unicam_dev), "/dev/%.50s", ent->d_name);
            } else if (!rkisp1_dev[0] && strcmp(vname, "rkisp1_mainpath") == 0) {
                snprintf(rkisp1_dev, sizeof(rkisp1_dev), "/dev/%.50s", ent->d_name);
            } else if (!subdev_dev[0] && strncmp(vname, "mvcam ", 6) == 0) {
                snprintf(subdev_dev, sizeof(subdev_dev), "/dev/%.50s", ent->d_name);
            } else if (!csi_subdev[0] && strcmp(vname, "rkisp1_csi") == 0) {
                snprintf(csi_subdev, sizeof(csi_subdev), "/dev/%.50s", ent->d_name);
            } else if (!isp_subdev[0] && strcmp(vname, "rkisp1_isp") == 0) {
                snprintf(isp_subdev, sizeof(isp_subdev), "/dev/%.50s", ent->d_name);
            } else if (!rsz_mp_subdev[0] && strcmp(vname, "rkisp1_resizer_mainpath") == 0) {
                snprintf(rsz_mp_subdev, sizeof(rsz_mp_subdev), "/dev/%.50s", ent->d_name);
            }
        }
        closedir(d);
    }

    if (unicam_dev[0] && subdev_dev[0]) {
        cam->backend = CAM_BACKEND_UNICAM;
        snprintf(cam->status.platform, sizeof(cam->status.platform), "rpi");
        snprintf(cam->status.video_dev, sizeof(cam->status.video_dev), "%s", unicam_dev);
    } else if (rkisp1_dev[0] && subdev_dev[0]) {
        cam->backend = CAM_BACKEND_RKISP1;
        snprintf(cam->status.platform, sizeof(cam->status.platform), "rk3399");
        snprintf(cam->status.video_dev, sizeof(cam->status.video_dev), "%s", rkisp1_dev);
        snprintf(cam->csi_subdev, sizeof(cam->csi_subdev), "%s", csi_subdev);
        snprintf(cam->isp_subdev, sizeof(cam->isp_subdev), "%s", isp_subdev);
        snprintf(cam->rsz_mp_subdev, sizeof(cam->rsz_mp_subdev), "%s", rsz_mp_subdev);
        if (!cam->csi_subdev[0] || !cam->isp_subdev[0] || !cam->rsz_mp_subdev[0] ||
            !discover_rkisp1_media(cam->media_dev, sizeof(cam->media_dev))) {
            snprintf(err, err_sz, "RKISP1 media/subdev nodes incomplete (csi/isp/resizer/media).");
            return false;
        }
    } else {
        snprintf(err, err_sz, "V4L2 nodes (unicam-image or rkisp1_mainpath / mvcam subdev) not found.");
        return false;
    }

    snprintf(cam->status.camera_model, sizeof(cam->status.camera_model), "%s", model);
    snprintf(cam->status.subdev_dev, sizeof(cam->status.subdev_dev), "%s", subdev_dev);
    snprintf(cam->status.i2c_dev, sizeof(cam->status.i2c_dev), "%s", i2c_dev);
    return true;
}

/* ---------------- V4L2 & I2C internal helpers (caller holds ctrl_mu) ---------------- */

static enum v4l2_buf_type cam_buf_type(const Camera *cam) {
    return (cam->backend == CAM_BACKEND_RKISP1)
           ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
           : V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static bool cam_qbuf(const Camera *cam, uint32_t index) {
    struct v4l2_buffer buf;
    struct v4l2_plane pl;
    memset(&buf, 0, sizeof(buf));
    memset(&pl, 0, sizeof(pl));
    buf.type = cam_buf_type(cam);
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (cam->backend == CAM_BACKEND_RKISP1) {
        buf.length = 1;
        buf.m.planes = &pl;
    }
    return xioctl(cam->video_fd, VIDIOC_QBUF, &buf) == 0;
}

static bool cam_dqbuf(const Camera *cam, uint32_t *out_index, uint32_t *out_bytesused) {
    struct v4l2_buffer buf;
    struct v4l2_plane pl;
    memset(&buf, 0, sizeof(buf));
    memset(&pl, 0, sizeof(pl));
    buf.type = cam_buf_type(cam);
    buf.memory = V4L2_MEMORY_MMAP;
    if (cam->backend == CAM_BACKEND_RKISP1) {
        buf.length = 1;
        buf.m.planes = &pl;
    }
    if (xioctl(cam->video_fd, VIDIOC_DQBUF, &buf) < 0) return false;
    *out_index = buf.index;
    *out_bytesused = (cam->backend == CAM_BACKEND_RKISP1) ? pl.bytesused : buf.bytesused;
    return true;
}

static void stop_v4l2_locked(Camera *cam) {
    enum v4l2_buf_type type = cam_buf_type(cam);
    if (cam->video_fd >= 0 && cam->status.streaming) {
        xioctl(cam->video_fd, VIDIOC_STREAMOFF, &type);
        cam->status.streaming = false;
    }
    for (uint32_t i = 0; i < cam->buf_count; ++i) {
        if (cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED) {
            munmap(cam->buffers[i].start, cam->buffers[i].length);
        }
    }
    cam->buf_count = 0;
    if (cam->video_fd >= 0) {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 0;
        req.type = type;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(cam->video_fd, VIDIOC_REQBUFS, &req);
    }
}

static bool start_v4l2_locked(Camera *cam, char *err, size_t err_sz) {
    enum v4l2_buf_type type = cam_buf_type(cam);
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = CAM_MAX_MMAP_BUFS;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->video_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        snprintf(err, err_sz, "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }
    if (req.count > CAM_MAX_MMAP_BUFS) req.count = CAM_MAX_MMAP_BUFS;

    cam->buf_count = 0;
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane pl;
        memset(&buf, 0, sizeof(buf));
        memset(&pl, 0, sizeof(pl));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cam->backend == CAM_BACKEND_RKISP1) {
            buf.length = 1;
            buf.m.planes = &pl;
        }
        if (xioctl(cam->video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            snprintf(err, err_sz, "VIDIOC_QUERYBUF failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
        size_t map_len = (cam->backend == CAM_BACKEND_RKISP1) ? pl.length : buf.length;
        off_t map_off  = (cam->backend == CAM_BACKEND_RKISP1) ? pl.m.mem_offset : buf.m.offset;
        void *ptr = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                         cam->video_fd, map_off);
        if (ptr == MAP_FAILED) {
            snprintf(err, err_sz, "mmap failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
        cam->buffers[i].start = ptr;
        cam->buffers[i].length = map_len;
        cam->buf_count = i + 1;
    }

    for (uint32_t i = 0; i < cam->buf_count; ++i) {
        if (!cam_qbuf(cam, i)) {
            snprintf(err, err_sz, "VIDIOC_QBUF failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
    }

    if (xioctl(cam->video_fd, VIDIOC_STREAMON, &type) < 0) {
        snprintf(err, err_sz, "VIDIOC_STREAMON failed: %s", strerror(errno));
        stop_v4l2_locked(cam);
        return false;
    }
    cam->status.streaming = true;
    return true;
}

static bool subdev_set_fmt_crop(const char *path, uint32_t pad, uint32_t code,
                                uint32_t w, uint32_t h, bool set_crop,
                                char *err, size_t err_sz) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        snprintf(err, err_sz, "Failed to open subdev %s: %s", path, strerror(errno));
        return false;
    }
    struct v4l2_subdev_format sfmt;
    memset(&sfmt, 0, sizeof(sfmt));
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sfmt.pad = pad;
    sfmt.format.width = w;
    sfmt.format.height = h;
    sfmt.format.code = code;
    sfmt.format.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &sfmt) < 0) {
        snprintf(err, err_sz, "SUBDEV_S_FMT failed on %s:%u: %s", path, pad, strerror(errno));
        close(fd);
        return false;
    }
    if (set_crop) {
        struct v4l2_subdev_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        sel.pad = pad;
        sel.target = V4L2_SEL_TGT_CROP;
        sel.r.left = 0;
        sel.r.top = 0;
        sel.r.width = w;
        sel.r.height = h;
        if (xioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &sel) < 0) {
            snprintf(err, err_sz, "SUBDEV_S_SELECTION crop failed on %s:%u: %s",
                     path, pad, strerror(errno));
            close(fd);
            return false;
        }
    }
    close(fd);
    return true;
}

static void rkisp1_disable_selfpath_link(const char *media_dev) {
    int mfd = open(media_dev, O_RDWR);
    if (mfd < 0) return;

    uint32_t isp_id = 0, sp_id = 0;
    struct media_entity_desc edesc;
    memset(&edesc, 0, sizeof(edesc));
    edesc.id = MEDIA_ENT_ID_FLAG_NEXT;
    while (xioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &edesc) == 0) {
        if (strcmp(edesc.name, "rkisp1_isp") == 0) isp_id = edesc.id;
        else if (strcmp(edesc.name, "rkisp1_resizer_selfpath") == 0) sp_id = edesc.id;
        edesc.id |= MEDIA_ENT_ID_FLAG_NEXT;
    }

    if (isp_id && sp_id) {
        struct media_pad_desc pads[8];
        struct media_link_desc links[16];
        struct media_links_enum lenum;
        memset(&lenum, 0, sizeof(lenum));
        lenum.entity = isp_id;
        lenum.pads = pads;
        lenum.links = links;
        if (xioctl(mfd, MEDIA_IOC_ENUM_LINKS, &lenum) == 0) {
            for (int i = 0; i < 16; ++i) {
                if (links[i].source.entity == isp_id &&
                    links[i].sink.entity == sp_id &&
                    (links[i].flags & MEDIA_LNK_FL_ENABLED)) {
                    links[i].flags &= ~MEDIA_LNK_FL_ENABLED;
                    xioctl(mfd, MEDIA_IOC_SETUP_LINK, &links[i]);
                    break;
                }
            }
        }
    }
    close(mfd);
}

static bool configure_pipeline_locked(Camera *cam, uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                                      uint32_t width, uint32_t height, char *err, size_t err_sz) {
    if (bit_depth != 10 && bit_depth != 12) {
        snprintf(err, err_sz, "bit_depth must be 10 or 12");
        return false;
    }

    roi_x = (roi_x / ROI_W_ALIGN) * ROI_W_ALIGN;
    roi_y = (roi_y / ROI_H_ALIGN) * ROI_H_ALIGN;
    if (roi_x + cam->status.min_roi_width > cam->status.sensor_width) {
        roi_x = ((cam->status.sensor_width - cam->status.min_roi_width) / ROI_W_ALIGN) * ROI_W_ALIGN;
    }
    if (roi_y + cam->status.min_roi_height > cam->status.sensor_height) {
        roi_y = ((cam->status.sensor_height - cam->status.min_roi_height) / ROI_H_ALIGN) * ROI_H_ALIGN;
    }
    width  = clamp_u32(width, cam->status.min_roi_width, cam->status.sensor_width - roi_x);
    height = clamp_u32(height, cam->status.min_roi_height, cam->status.sensor_height - roi_y);
    width  = (width / ROI_W_ALIGN) * ROI_W_ALIGN;
    height = (height / ROI_H_ALIGN) * ROI_H_ALIGN;

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_VEYE_MV_TRIGGER_MODE;
    ctrl.value = 0;
    xioctl(cam->subdev_fd, VIDIOC_S_CTRL, &ctrl);

    ctrl.id = V4L2_CID_VEYE_MV_ROI_X;
    ctrl.value = (int32_t)roi_x;
    if (xioctl(cam->subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        snprintf(err, err_sz, "VIDIOC_S_CTRL ROI_X failed: %s", strerror(errno));
        return false;
    }
    ctrl.id = V4L2_CID_VEYE_MV_ROI_Y;
    ctrl.value = (int32_t)roi_y;
    if (xioctl(cam->subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        snprintf(err, err_sz, "VIDIOC_S_CTRL ROI_Y failed: %s", strerror(errno));
        return false;
    }

    uint32_t mbus_code = (bit_depth == 10) ? MEDIA_BUS_FMT_Y10_1X10 : MEDIA_BUS_FMT_Y12_1X12;
    struct v4l2_subdev_format sfmt;
    memset(&sfmt, 0, sizeof(sfmt));
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sfmt.pad = 0;
    sfmt.format.width = width;
    sfmt.format.height = height;
    sfmt.format.code = mbus_code;
    sfmt.format.field = V4L2_FIELD_NONE;
    if (xioctl(cam->subdev_fd, VIDIOC_SUBDEV_S_FMT, &sfmt) < 0) {
        snprintf(err, err_sz, "VIDIOC_SUBDEV_S_FMT failed: %s", strerror(errno));
        return false;
    }
    xioctl(cam->subdev_fd, VIDIOC_SUBDEV_G_FMT, &sfmt);
    uint32_t eff_w = sfmt.format.width ? sfmt.format.width : width;
    uint32_t eff_h = sfmt.format.height ? sfmt.format.height : height;

    if (cam->backend == CAM_BACKEND_RKISP1) {
        rkisp1_disable_selfpath_link(cam->media_dev);
        if (!subdev_set_fmt_crop(cam->csi_subdev, 0, mbus_code, eff_w, eff_h, false, err, err_sz) ||
            !subdev_set_fmt_crop(cam->isp_subdev, 0, mbus_code, eff_w, eff_h, true, err, err_sz) ||
            !subdev_set_fmt_crop(cam->isp_subdev, 2, mbus_code, eff_w, eff_h, true, err, err_sz) ||
            !subdev_set_fmt_crop(cam->rsz_mp_subdev, 0, mbus_code, eff_w, eff_h, true, err, err_sz) ||
            !subdev_set_fmt_crop(cam->rsz_mp_subdev, 1, mbus_code, eff_w, eff_h, false, err, err_sz)) {
            return false;
        }

        struct v4l2_format vfmt;
        memset(&vfmt, 0, sizeof(vfmt));
        vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        vfmt.fmt.pix_mp.width = eff_w;
        vfmt.fmt.pix_mp.height = eff_h;
        vfmt.fmt.pix_mp.pixelformat = (bit_depth == 10) ? V4L2_PIX_FMT_Y10 : V4L2_PIX_FMT_Y12;
        vfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        vfmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(cam->video_fd, VIDIOC_S_FMT, &vfmt) < 0) {
            snprintf(err, err_sz, "VIDIOC_S_FMT (mplane) failed: %s", strerror(errno));
            return false;
        }
        cam->status.width = vfmt.fmt.pix_mp.width;
        cam->status.height = vfmt.fmt.pix_mp.height;
        cam->status.stride = vfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        struct v4l2_format vfmt;
        memset(&vfmt, 0, sizeof(vfmt));
        vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vfmt.fmt.pix.width = eff_w;
        vfmt.fmt.pix.height = eff_h;
        vfmt.fmt.pix.pixelformat = (bit_depth == 10) ? V4L2_PIX_FMT_Y10P : V4L2_PIX_FMT_Y12P;
        vfmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(cam->video_fd, VIDIOC_S_FMT, &vfmt) < 0) {
            snprintf(err, err_sz, "VIDIOC_S_FMT failed: %s", strerror(errno));
            return false;
        }
        cam->status.width = vfmt.fmt.pix.width;
        cam->status.height = vfmt.fmt.pix.height;
        cam->status.stride = vfmt.fmt.pix.bytesperline;
    }

    cam->status.bit_depth = bit_depth;
    cam->status.roi_x = roi_x;
    cam->status.roi_y = roi_y;

    /* Reopen hardware preview encoder for new dimensions */
    h264_enc_close(&cam->h264_enc);
    mjpeg_enc_close(&cam->mjpeg_enc);
    if (cam->backend == CAM_BACKEND_RKISP1) {
        if (!mjpeg_enc_open(&cam->mjpeg_enc, cam->status.width, cam->status.height, 85, NULL)) {
            if (atomic_load(&cam->stream_mode_id) == 3) {
                snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "lossless");
                atomic_store(&cam->stream_mode_id, 0);
            }
        }
    } else {
        if (!h264_enc_open(&cam->h264_enc, cam->status.width, cam->status.height, 8000000, 30, "/dev/video11")) {
            if (atomic_load(&cam->stream_mode_id) == 2) {
                snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "lossless");
                atomic_store(&cam->stream_mode_id, 0);
            }
        }
    }
    cam->status.h264_supported = cam->h264_enc.streaming;
    cam->status.mjpeg_supported = cam->mjpeg_enc.streaming;
    atomic_store(&cam->keyframe_req, true);
    return true;
}

static void refresh_hw_telemetry_locked(Camera *cam) {
    uint32_t v = 0;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_EXP_TIME, &v)) {
        cam->status.actual_exposure_us = v;
        atomic_store(&cam->cached_exp_us, v);
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_CUR_GAIN, &v)) {
        cam->status.actual_gain_db = v / 10.0;
        atomic_store(&cam->cached_gain_x10, v);
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_FRAMERATE, &v) && v > 0) {
        cam->status.effective_fps = v / 100.0;
        atomic_store(&cam->cached_fps_x100, v);
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MAX_FRAME_RATE, &v) && v > 0) {
        cam->status.sensor_max_fps = v / 100.0;
    }
}

static void append_error(char *err, size_t size, const char *message) {
    size_t used = strlen(err);
    if (used < size) snprintf(err + used, size - used, "%s%s", used ? "; " : "", message);
}

static bool write_gain_locked(Camera *cam, double gain_db) {
    return i2c_ctrl_write_u32(&cam->i2c, REG_GAIN_MODE, 2) &&
           i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, (uint32_t)llround(gain_db * 10.0));
}

static bool apply_timing_locked(Camera *cam, uint32_t exp_us, double target_fps,
                                bool *need_restart, char *err, size_t err_sz) {
    uint32_t max_reg, current_reg;
    if (!i2c_ctrl_read_u32(&cam->i2c, REG_MAX_FRAME_RATE, &max_reg) || max_reg < 10 ||
        !i2c_ctrl_read_u32(&cam->i2c, REG_FRAMERATE, &current_reg) || current_reg == 0) {
        snprintf(err, err_sz, "I2C frame rate read failed");
        return false;
    }
    cam->status.sensor_max_fps = max_reg / 100.0;
    double desired = clamp_dbl(target_fps, 0.1, cam->status.sensor_max_fps);
    double exposure_limit = 980000.0 / (double)exp_us;
    double effective = clamp_dbl(exposure_limit, 0.1, desired);
    uint32_t rate = clamp_u32((uint32_t)llround(effective * 100.0), 10u, max_reg);
    if (need_restart && cam->status.streaming && rate != current_reg) {
        *need_restart = true;
        return true;
    }

    /* Lower the rate before extending exposure. Shorten exposure before raising it. */
    bool lower_rate = rate < current_reg;
    if (!i2c_ctrl_write_u32(&cam->i2c, REG_EXPOSURE_MODE, 2) ||
        !i2c_ctrl_write_u32(&cam->i2c, lower_rate ? REG_FRAMERATE : REG_ME_TIME,
                            lower_rate ? rate : exp_us)) goto write_failed;
    usleep(8000);
    if (!i2c_ctrl_write_u32(&cam->i2c, lower_rate ? REG_ME_TIME : REG_FRAMERATE,
                            lower_rate ? exp_us : rate)) goto write_failed;

    cam->status.target_exposure_us = exp_us;
    cam->status.target_fps = target_fps;
    cam->status.effective_fps = rate / 100.0;
    cam->status.fps_clamped_by_exposure = exposure_limit < desired;
    atomic_store(&cam->cached_fps_x100, rate);
    return true;

write_failed:
    snprintf(err, err_sz, "I2C write failed setting exposure/frame rate");
    return false;
}

static bool update_timing(Camera *cam, uint32_t exp_us, double fps, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->reconfig_mu);
    pthread_mutex_lock(&cam->ctrl_mu);
    uint32_t old_exp = cam->status.target_exposure_us;
    double old_fps = cam->status.target_fps;
    if (!exp_us) exp_us = old_exp;
    if (!fps) fps = old_fps;
    bool restart = false;
    bool ok = apply_timing_locked(cam, exp_us, fps, &restart, err, err_sz);
    if (restart) {
        pthread_mutex_unlock(&cam->ctrl_mu);
        camera_stop_capture(cam);
        pthread_mutex_lock(&cam->ctrl_mu);
        ok = apply_timing_locked(cam, exp_us, fps, NULL, err, err_sz);
    }
    if (!ok) {
        char rollback[128];
        if (!apply_timing_locked(cam, old_exp, old_fps, NULL, rollback, sizeof(rollback))) {
            append_error(err, err_sz, "previous exposure/frame rate could not be restored");
        }
    }
    refresh_hw_telemetry_locked(cam);
    pthread_mutex_unlock(&cam->ctrl_mu);
    if (restart) {
        char reason[128];
        if (!camera_start_capture(cam, reason, sizeof(reason))) {
            if (ok) snprintf(err, err_sz, "%s", reason);
            else append_error(err, err_sz, reason);
            ok = false;
        }
    }
    pthread_mutex_unlock(&cam->reconfig_mu);
    return ok;
}

/* ---------------- Capture thread ---------------- */

static void *capture_loop(void *arg) {
    Camera *cam = (Camera *)arg;
    uint64_t fps_frames = 0;
    uint64_t fps_t0 = monotonic_ns();
    uint64_t telem_t0 = fps_t0;

    ByteBuf work_buf;
    ByteBuf raw_conv_buf;
    bytebuf_init(&work_buf);
    bytebuf_init(&raw_conv_buf);

    while (atomic_load(&cam->running)) {
        struct pollfd pfd = { cam->video_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;

        uint32_t buf_idx = 0, bytesused = 0;
        if (!cam_dqbuf(cam, &buf_idx, &bytesused)) {
            if (errno == EAGAIN) continue;
            break;
        }

        const uint8_t *src = (const uint8_t *)cam->buffers[buf_idx].start;
        const uint64_t ts_ns = monotonic_ns();

        pthread_mutex_lock(&cam->ctrl_mu);
        uint32_t w = cam->status.width;
        uint32_t h = cam->status.height;
        uint32_t stride = cam->status.stride;
        uint32_t rx = cam->status.roi_x;
        uint32_t ry = cam->status.roi_y;
        uint16_t bd = cam->status.bit_depth;
        pthread_mutex_unlock(&cam->ctrl_mu);

        const uint64_t seq = atomic_fetch_add(&cam->frame_seq_counter, 1) + 1;
        const bool is_rkisp1 = (cam->backend == CAM_BACKEND_RKISP1);
        const uint32_t max_raw = stride * h;
        const uint32_t raw_payload_size = is_rkisp1 ? (w * h * 2) : (bytesused < max_raw ? bytesused : max_raw);

        FrameHeader raw_hdr;
        memset(&raw_hdr, 0, sizeof(raw_hdr));
        raw_hdr.magic[0] = RAWCAM_FRAME_MAGIC_0;
        raw_hdr.magic[1] = RAWCAM_FRAME_MAGIC_1;
        raw_hdr.magic[2] = RAWCAM_FRAME_MAGIC_2;
        raw_hdr.magic[3] = RAWCAM_FRAME_MAGIC_3;
        raw_hdr.version = RAWCAM_PROTOCOL_VERSION;
        if (is_rkisp1) {
            raw_hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10 : PIXFMT_Y12);
            raw_hdr.stride = w * 2;
        } else {
            raw_hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10P : PIXFMT_Y12P);
            raw_hdr.stride = stride;
        }
        raw_hdr.flags = FLAG_LOSSLESS;
        raw_hdr.width = w;
        raw_hdr.height = h;
        raw_hdr.payload_size = raw_payload_size;
        raw_hdr.exposure_us = (uint32_t)atomic_load(&cam->cached_exp_us);
        raw_hdr.frame_seq = seq;
        raw_hdr.timestamp_ns = ts_ns;
        raw_hdr.gain_db_x10 = (uint32_t)atomic_load(&cam->cached_gain_x10);
        raw_hdr.fps_x100 = (uint32_t)atomic_load(&cam->cached_fps_x100);
        raw_hdr.bit_depth = bd;
        raw_hdr.roi_x = (uint16_t)rx;
        raw_hdr.roi_y = (uint16_t)ry;

        int mode_id = atomic_load(&cam->stream_mode_id);
        bool want_snap = atomic_load_explicit(&cam->snap_req, memory_order_relaxed);
        const uint8_t *lossless_ptr = src;
        if (is_rkisp1 && (want_snap || mode_id == 0)) {
            if (bytebuf_reserve(&raw_conv_buf, raw_payload_size)) {
                raw_conv_buf.size = raw_payload_size;
                unpack_rkisp1_be_to_le_u16(src, stride, raw_conv_buf.data, w, h, bd);
                lossless_ptr = raw_conv_buf.data;
            }
        }

        /* On-demand 10/12-bit lossless copy for snapshot requests (port 5602) */
        if (want_snap) {
            pthread_mutex_lock(&cam->raw_mu);
            cam->latest_raw_frame.header = raw_hdr;
            bytebuf_assign(&cam->latest_raw_frame.data, lossless_ptr, raw_payload_size);
            cam->snap_done_seq = seq;
            atomic_store(&cam->snap_req, false);
            pthread_cond_broadcast(&cam->raw_cv);
            pthread_mutex_unlock(&cam->raw_mu);
        }

        if (mode_id == 0) {
            /* Lossless RAW (MIPI packed on Unicam, standard uint16 LE on RKISP1) */
            pthread_mutex_lock(&cam->frame_mu);
            cam->latest_frame.header = raw_hdr;
            bytebuf_assign(&cam->latest_frame.data, lossless_ptr, raw_payload_size);
            cam->latest_seq = seq;
            pthread_cond_broadcast(&cam->frame_cv);
            pthread_mutex_unlock(&cam->frame_mu);
        } else if (mode_id == 1) {
            /* Uncompressed 8-bit preview */
            FrameHeader hdr = raw_hdr;
            hdr.pixel_format = (uint16_t)PIXFMT_Y8_PREVIEW;
            hdr.flags = FLAG_PREVIEW;
            hdr.stride = w;
            hdr.payload_size = w * h;
            if (bytebuf_reserve(&work_buf, hdr.payload_size)) {
                work_buf.size = hdr.payload_size;
                if (is_rkisp1) {
                    extract_rkisp1_be_to_y8_strided(src, stride, work_buf.data, w, w, h, 0);
                } else if (bd == 10) {
                    extract_y10p_to_y8_strided(src, stride, work_buf.data, w, w, h, 0);
                } else {
                    extract_y12p_to_y8_strided(src, stride, work_buf.data, w, w, h, 0);
                }
                pthread_mutex_lock(&cam->frame_mu);
                cam->latest_frame.header = hdr;
                bytebuf_assign(&cam->latest_frame.data, work_buf.data, work_buf.size);
                cam->latest_seq = seq;
                pthread_cond_broadcast(&cam->frame_cv);
                pthread_mutex_unlock(&cam->frame_mu);
            }
        } else if (mode_id == 2 && cam->h264_enc.streaming) {
            /* Hardware H.264 encode via /dev/video11 (RPi) */
            if (atomic_exchange(&cam->keyframe_req, false)) {
                h264_enc_force_keyframe(&cam->h264_enc);
            }
            uint32_t top = (h - cam->h264_enc.enc_h) / 2;
            PreviewFillCtx fctx = { src + (size_t)top * stride, stride, w, bd, cam->backend };
            bool is_kf = false;
            size_t nal_len = 0;
            if (h264_enc_encode(&cam->h264_enc, fill_y_for_preview, &fctx,
                                &work_buf.data, &work_buf.cap, &nal_len, &is_kf)) {
                work_buf.size = nal_len;
                FrameHeader hdr = raw_hdr;
                hdr.pixel_format = (uint16_t)PIXFMT_H264;
                hdr.flags = FLAG_PREVIEW | (is_kf ? FLAG_KEYFRAME : 0u);
                hdr.width = cam->h264_enc.enc_w;
                hdr.height = cam->h264_enc.enc_h;
                hdr.stride = hdr.width;
                hdr.payload_size = (uint32_t)nal_len;

                pthread_mutex_lock(&cam->frame_mu);
                cam->latest_frame.header = hdr;
                bytebuf_assign(&cam->latest_frame.data, work_buf.data, nal_len);
                cam->latest_seq = seq;
                pthread_cond_broadcast(&cam->frame_cv);
                pthread_mutex_unlock(&cam->frame_mu);
            }
        } else if (mode_id == 3 && cam->mjpeg_enc.streaming) {
            /* Hardware JPEG/MJPEG encode via Hantro VPU (RK3399) */
            PreviewFillCtx fctx = { src, stride, w, bd, cam->backend };
            size_t jpg_len = 0;
            if (mjpeg_enc_encode(&cam->mjpeg_enc, fill_y_for_preview, &fctx,
                                 &work_buf.data, &work_buf.cap, &jpg_len)) {
                work_buf.size = jpg_len;
                FrameHeader hdr = raw_hdr;
                hdr.pixel_format = (uint16_t)PIXFMT_MJPEG;
                hdr.flags = FLAG_PREVIEW | FLAG_KEYFRAME;
                hdr.width = cam->mjpeg_enc.src_w;
                hdr.height = cam->mjpeg_enc.src_h;
                hdr.stride = hdr.width;
                hdr.payload_size = (uint32_t)jpg_len;

                pthread_mutex_lock(&cam->frame_mu);
                cam->latest_frame.header = hdr;
                bytebuf_assign(&cam->latest_frame.data, work_buf.data, jpg_len);
                cam->latest_seq = seq;
                pthread_cond_broadcast(&cam->frame_cv);
                pthread_mutex_unlock(&cam->frame_mu);
            }
        }

        cam_qbuf(cam, buf_idx);

        ++fps_frames;
        uint64_t now = monotonic_ns();
        double dt = (double)(now - fps_t0) / 1e9;
        if (dt >= 0.5) {
            double mfps = (double)fps_frames / dt;
            fps_frames = 0;
            fps_t0 = now;
            pthread_mutex_lock(&cam->ctrl_mu);
            cam->status.measured_fps = mfps;
            cam->status.frame_seq = seq;
            cam->status.dropped_frames = atomic_load(&cam->dropped_frames);
            pthread_mutex_unlock(&cam->ctrl_mu);
        }
        if ((double)(now - telem_t0) / 1e9 >= 0.5) {
            telem_t0 = now;
            pthread_mutex_lock(&cam->ctrl_mu);
            refresh_hw_telemetry_locked(cam);
            pthread_mutex_unlock(&cam->ctrl_mu);
        }
    }

    bytebuf_free(&raw_conv_buf);
    bytebuf_free(&work_buf);
    return NULL;
}

/* ---------------- Public API ---------------- */

bool camera_init(Camera *cam, char *err, size_t err_sz) {
    memset(cam, 0, sizeof(*cam));
    cam->video_fd = -1;
    cam->subdev_fd = -1;
    i2c_ctrl_init(&cam->i2c);
    h264_enc_init(&cam->h264_enc);
    mjpeg_enc_init(&cam->mjpeg_enc);
    framebuf_init(&cam->latest_frame);
    framebuf_init(&cam->latest_raw_frame);

    pthread_mutex_init(&cam->reconfig_mu, NULL);
    pthread_mutex_init(&cam->ctrl_mu, NULL);
    pthread_mutex_init(&cam->frame_mu, NULL);
    pthread_mutex_init(&cam->raw_mu, NULL);
    init_monotonic_cond(&cam->frame_cv);
    init_monotonic_cond(&cam->raw_cv);

    atomic_init(&cam->running, false);
    atomic_init(&cam->frame_seq_counter, 0);
    atomic_init(&cam->dropped_frames, 0);
    atomic_init(&cam->snap_req, false);
    atomic_init(&cam->cached_exp_us, 10000);
    atomic_init(&cam->cached_gain_x10, 0);
    atomic_init(&cam->cached_fps_x100, 3000);
    atomic_init(&cam->stream_mode_id, 2);
    atomic_init(&cam->keyframe_req, true);

    if (!discover_devices(cam, err, err_sz)) return false;

    cam->video_fd = open(cam->status.video_dev, O_RDWR | O_NONBLOCK);
    if (cam->video_fd < 0) {
        snprintf(err, err_sz, "Failed to open %s: %s", cam->status.video_dev, strerror(errno));
        return false;
    }
    cam->subdev_fd = open(cam->status.subdev_dev, O_RDWR);
    if (cam->subdev_fd < 0) {
        snprintf(err, err_sz, "Failed to open %s: %s", cam->status.subdev_dev, strerror(errno));
        close(cam->video_fd);
        cam->video_fd = -1;
        return false;
    }
    if (!i2c_ctrl_open(&cam->i2c, cam->status.i2c_dev, 0x3b)) {
        snprintf(err, err_sz, "Failed to open %s: %s", cam->status.i2c_dev, strerror(errno));
        close(cam->subdev_fd);
        close(cam->video_fd);
        cam->subdev_fd = cam->video_fd = -1;
        return false;
    }

    snprintf(cam->status.sensor_type, sizeof(cam->status.sensor_type), "mono");
    cam->status.sensor_width = 1944;
    cam->status.sensor_height = 1096;
    cam->status.min_roi_width = 368;
    cam->status.min_roi_height = 304;
    cam->status.format_cap_mask = 0x6; /* default: RAW10 + RAW12 */

    uint32_t v = 0;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_FORMAT_CAP, &v) && (v & 0x6u)) {
        cam->status.format_cap_mask = v;
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_SENSOR_WIDTH, &v) && v > 0) cam->status.sensor_width = v;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_SENSOR_HEIGHT, &v) && v > 0) cam->status.sensor_height = v;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MIN_ROI_WIDTH, &v) && v >= 64 && v <= cam->status.sensor_width) {
        cam->status.min_roi_width = v;
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MIN_ROI_HEIGHT, &v) && v >= 64 && v <= cam->status.sensor_height) {
        cam->status.min_roi_height = v;
    }

    cam->status.bit_depth = (cam->status.format_cap_mask & 0x4u) ? 12 : 10;
    cam->status.roi_x = 0;
    cam->status.roi_y = 0;
    cam->status.width = cam->status.sensor_width;
    cam->status.height = cam->status.sensor_height;
    snprintf(cam->status.exposure_mode, sizeof(cam->status.exposure_mode), "manual");
    cam->status.target_exposure_us = 10000;
    snprintf(cam->status.gain_mode, sizeof(cam->status.gain_mode), "manual");
    cam->status.target_gain_db = 0.0;
    cam->status.target_fps = 30.0;
    if (cam->backend == CAM_BACKEND_RKISP1) {
        snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "mjpeg");
        atomic_store(&cam->stream_mode_id, 3);
    } else {
        snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "h264");
        atomic_store(&cam->stream_mode_id, 2);
    }

    if (!configure_pipeline_locked(cam, cam->status.bit_depth, 0, 0,
                                   cam->status.width, cam->status.height, err, err_sz)) {
        return false;
    }

    if (!write_gain_locked(cam, cam->status.target_gain_db)) {
        snprintf(err, err_sz, "I2C write failed setting initial gain");
        return false;
    }
    if (!apply_timing_locked(cam, cam->status.target_exposure_us, cam->status.target_fps,
                             NULL, err, err_sz)) return false;
    refresh_hw_telemetry_locked(cam);
    return true;
}

void camera_shutdown(Camera *cam) {
    camera_stop_capture(cam);
    pthread_mutex_lock(&cam->ctrl_mu);
    h264_enc_close(&cam->h264_enc);
    mjpeg_enc_close(&cam->mjpeg_enc);
    stop_v4l2_locked(cam);
    i2c_ctrl_close(&cam->i2c);
    if (cam->subdev_fd >= 0) {
        close(cam->subdev_fd);
        cam->subdev_fd = -1;
    }
    if (cam->video_fd >= 0) {
        close(cam->video_fd);
        cam->video_fd = -1;
    }
    pthread_mutex_unlock(&cam->ctrl_mu);

    framebuf_free(&cam->latest_frame);
    framebuf_free(&cam->latest_raw_frame);
}

bool camera_start_capture(Camera *cam, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->ctrl_mu);
    if (atomic_load(&cam->running)) {
        pthread_mutex_unlock(&cam->ctrl_mu);
        return true;
    }
    if (!start_v4l2_locked(cam, err, err_sz)) {
        pthread_mutex_unlock(&cam->ctrl_mu);
        return false;
    }
    atomic_store(&cam->running, true);
    if (pthread_create(&cam->cap_thread, NULL, capture_loop, cam) != 0) {
        atomic_store(&cam->running, false);
        stop_v4l2_locked(cam);
        snprintf(err, err_sz, "pthread_create failed");
        pthread_mutex_unlock(&cam->ctrl_mu);
        return false;
    }
    cam->cap_thread_started = true;
    pthread_mutex_unlock(&cam->ctrl_mu);
    return true;
}

void camera_stop_capture(Camera *cam) {
    atomic_store(&cam->running, false);
    pthread_mutex_lock(&cam->frame_mu);
    pthread_cond_broadcast(&cam->frame_cv);
    pthread_mutex_unlock(&cam->frame_mu);

    if (cam->cap_thread_started) {
        pthread_join(cam->cap_thread, NULL);
        cam->cap_thread_started = false;
    }

    pthread_mutex_lock(&cam->ctrl_mu);
    stop_v4l2_locked(cam);
    pthread_mutex_unlock(&cam->ctrl_mu);
}

void camera_get_status(Camera *cam, CameraStatus *out) {
    pthread_mutex_lock(&cam->ctrl_mu);
    refresh_hw_telemetry_locked(cam);
    cam->status.dropped_frames = atomic_load(&cam->dropped_frames);
    *out = cam->status;
    pthread_mutex_unlock(&cam->ctrl_mu);
}

bool camera_set_exposure(Camera *cam, const char *mode, uint32_t exposure_us, char *err, size_t err_sz) {
    (void)mode;
    if (exposure_us) exposure_us = clamp_u32(exposure_us, 10u, 10000000u);
    return update_timing(cam, exposure_us, 0, err, err_sz);
}

bool camera_set_gain(Camera *cam, const char *mode, double gain_db, char *err, size_t err_sz) {
    (void)mode;
    pthread_mutex_lock(&cam->reconfig_mu);
    pthread_mutex_lock(&cam->ctrl_mu);
    double clamped = clamp_dbl(gain_db, 0.0, 48.0);
    bool ok = write_gain_locked(cam, clamped);
    if (ok) cam->status.target_gain_db = clamped;
    else snprintf(err, err_sz, "I2C write failed while setting gain");
    refresh_hw_telemetry_locked(cam);
    pthread_mutex_unlock(&cam->ctrl_mu);
    pthread_mutex_unlock(&cam->reconfig_mu);
    return ok;
}

bool camera_set_fps(Camera *cam, double target_fps, char *err, size_t err_sz) {
    if (target_fps <= 0.0 || target_fps > 240.0) {
        snprintf(err, err_sz, "target_fps out of range");
        return false;
    }
    return update_timing(cam, 0, target_fps, err, err_sz);
}

bool camera_set_format(Camera *cam, uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                       uint32_t width, uint32_t height, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->reconfig_mu);
    bool was_running = atomic_load(&cam->running);
    if (was_running) camera_stop_capture(cam);

    pthread_mutex_lock(&cam->ctrl_mu);
    bool ok = configure_pipeline_locked(cam, bit_depth, roi_x, roi_y, width, height, err, err_sz);
    if (ok) {
        ok = apply_timing_locked(cam, cam->status.target_exposure_us, cam->status.target_fps,
                                 NULL, err, err_sz);
        refresh_hw_telemetry_locked(cam);
    }
    pthread_mutex_unlock(&cam->ctrl_mu);

    if (was_running) {
        char dummy[128];
        if (!camera_start_capture(cam, ok ? err : dummy, ok ? err_sz : sizeof(dummy))) {
            ok = false;
        }
    }
    pthread_mutex_unlock(&cam->reconfig_mu);
    return ok;
}

bool camera_set_stream_mode(Camera *cam, const char *mode, char *err, size_t err_sz) {
    int id = -1;
    if (strcmp(mode, "lossless") == 0) id = 0;
    else if (strcmp(mode, "preview8") == 0) id = 1;
    else if (strcmp(mode, "h264") == 0) id = 2;
    else if (strcmp(mode, "mjpeg") == 0) id = 3;
    else {
        snprintf(err, err_sz, "stream_mode must be 'h264', 'mjpeg', 'lossless', or 'preview8'");
        return false;
    }

    pthread_mutex_lock(&cam->ctrl_mu);
    if (id == 2 && !cam->h264_enc.streaming) {
        if (!h264_enc_open(&cam->h264_enc, cam->status.width, cam->status.height, 8000000, 30, "/dev/video11")) {
            pthread_mutex_unlock(&cam->ctrl_mu);
            snprintf(err, err_sz, "Hardware H.264 encoder unavailable on this platform (use 'mjpeg')");
            return false;
        }
    } else if (id == 3 && !cam->mjpeg_enc.streaming) {
        if (!mjpeg_enc_open(&cam->mjpeg_enc, cam->status.width, cam->status.height, 85, NULL)) {
            pthread_mutex_unlock(&cam->ctrl_mu);
            snprintf(err, err_sz, "Hardware MJPEG encoder unavailable on this platform");
            return false;
        }
    }
    snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "%s", mode);
    atomic_store(&cam->stream_mode_id, id);
    if (id == 2) atomic_store(&cam->keyframe_req, true);
    pthread_mutex_unlock(&cam->ctrl_mu);
    return true;
}

bool camera_wait_next_frame(Camera *cam, uint64_t last_seq, FrameBuffer *out, int timeout_ms) {
    struct timespec dl = monotonic_deadline_ms(timeout_ms);
    pthread_mutex_lock(&cam->frame_mu);
    while (cam->latest_seq <= last_seq) {
        int rc = pthread_cond_timedwait(&cam->frame_cv, &cam->frame_mu, &dl);
        if (rc == ETIMEDOUT) break;
    }
    if (cam->latest_seq <= last_seq) {
        pthread_mutex_unlock(&cam->frame_mu);
        return false;
    }
    if (last_seq > 0 && cam->latest_seq > last_seq + 1) {
        atomic_fetch_add(&cam->dropped_frames, cam->latest_seq - last_seq - 1);
    }
    out->header = cam->latest_frame.header;
    bool ok = bytebuf_assign(&out->data, cam->latest_frame.data.data, cam->latest_frame.data.size);
    pthread_mutex_unlock(&cam->frame_mu);
    return ok;
}

bool camera_get_latest_raw_frame(Camera *cam, FrameBuffer *out) {
    struct timespec dl = monotonic_deadline_ms(1500);
    pthread_mutex_lock(&cam->raw_mu);
    uint64_t prev = cam->snap_done_seq;
    atomic_store(&cam->snap_req, true);
    while (cam->snap_done_seq <= prev && atomic_load(&cam->running)) {
        int rc = pthread_cond_timedwait(&cam->raw_cv, &cam->raw_mu, &dl);
        if (rc == ETIMEDOUT) break;
    }
    bool ok = false;
    if (cam->snap_done_seq > prev && cam->latest_raw_frame.data.size > 0) {
        out->header = cam->latest_raw_frame.header;
        ok = bytebuf_assign(&out->data, cam->latest_raw_frame.data.data, cam->latest_raw_frame.data.size);
    }
    pthread_mutex_unlock(&cam->raw_mu);
    return ok;
}

bool camera_capture_long_shot(Camera *cam, uint32_t exposure_us, double gain_db,
                              FrameBuffer *out, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->reconfig_mu);
    exposure_us = clamp_u32(exposure_us, 100u, 10000000u);
    gain_db = clamp_dbl(gain_db, 0.0, 48.0);

    bool was_running = atomic_load(&cam->running);
    if (was_running) camera_stop_capture(cam);

    bool got = false;
    bool v4l2_started = false;

    pthread_mutex_lock(&cam->ctrl_mu);
    uint32_t saved_exp_us = cam->status.target_exposure_us;
    double   saved_gain_db = cam->status.target_gain_db;
    double   saved_fps = cam->status.target_fps;
    uint32_t w = cam->status.width;
    uint32_t h = cam->status.height;
    uint32_t stride = cam->status.stride;
    uint32_t rx = cam->status.roi_x;
    uint32_t ry = cam->status.roi_y;
    uint16_t bd = cam->status.bit_depth;

    err[0] = '\0';
    uint32_t max_fps_reg;
    if (!i2c_ctrl_read_u32(&cam->i2c, REG_MAX_FRAME_RATE, &max_fps_reg) || max_fps_reg < 10) {
        snprintf(err, err_sz, "I2C frame rate read failed configuring long shot");
        pthread_mutex_unlock(&cam->ctrl_mu);
        goto restore;
    }
    double exp_fps = 980000.0 / (double)exposure_us;
    uint32_t fps_reg = clamp_u32((uint32_t)llround(exp_fps * 100.0), 10u, max_fps_reg);

    bool i2c_ok =
        i2c_ctrl_write_u32(&cam->i2c, REG_EXPOSURE_MODE, 2) &&
        i2c_ctrl_write_u32(&cam->i2c, REG_FRAMERATE, fps_reg);
    if (i2c_ok) {
        usleep(10000);
        i2c_ok =
            i2c_ctrl_write_u32(&cam->i2c, REG_ME_TIME, exposure_us) &&
            write_gain_locked(cam, gain_db);
    }
    if (!i2c_ok) {
        snprintf(err, err_sz, "I2C write failed configuring long shot");
        pthread_mutex_unlock(&cam->ctrl_mu);
        goto restore;
    }

    usleep(10000);
    refresh_hw_telemetry_locked(cam);

    if (!start_v4l2_locked(cam, err, err_sz)) {
        pthread_mutex_unlock(&cam->ctrl_mu);
        goto restore;
    }
    v4l2_started = true;
    pthread_mutex_unlock(&cam->ctrl_mu);

    uint64_t t_start = monotonic_ns();
    int timeout_ms = (int)(exposure_us / 1000) + 4000;

    for (int attempt = 0; attempt < 3 && !got; ++attempt) {
        struct pollfd pfd = { cam->video_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            snprintf(err, err_sz, "Timed out waiting for long-exposure frame");
            break;
        }
        uint32_t buf_idx = 0, bytesused = 0;
        if (!cam_dqbuf(cam, &buf_idx, &bytesused)) {
            snprintf(err, err_sz, "VIDIOC_DQBUF failed: %s", strerror(errno));
            break;
        }
        double elapsed_ms = (double)(monotonic_ns() - t_start) / 1e6;
        if (attempt == 0 && exposure_us >= 500000 && elapsed_ms < (exposure_us / 1000.0) * 0.4) {
            cam_qbuf(cam, buf_idx);
            t_start = monotonic_ns();
            continue;
        }

        const uint8_t *src = (const uint8_t *)cam->buffers[buf_idx].start;
        const bool is_rkisp1 = (cam->backend == CAM_BACKEND_RKISP1);
        uint32_t max_raw = stride * h;
        uint32_t payload_size = is_rkisp1 ? (w * h * 2) : (bytesused < max_raw ? bytesused : max_raw);
        uint64_t seq = atomic_fetch_add(&cam->frame_seq_counter, 1) + 1;

        uint32_t hw_exp = 0, hw_gain = 0, hw_fps = 0;
        if (!i2c_ctrl_read_u32(&cam->i2c, REG_EXP_TIME, &hw_exp) ||
            !i2c_ctrl_read_u32(&cam->i2c, REG_CUR_GAIN, &hw_gain) ||
            !i2c_ctrl_read_u32(&cam->i2c, REG_FRAMERATE, &hw_fps)) {
            cam_qbuf(cam, buf_idx);
            snprintf(err, err_sz, "I2C readback failed for long-shot exposure/gain metadata");
            break;
        }

        FrameHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic[0] = RAWCAM_FRAME_MAGIC_0;
        hdr.magic[1] = RAWCAM_FRAME_MAGIC_1;
        hdr.magic[2] = RAWCAM_FRAME_MAGIC_2;
        hdr.magic[3] = RAWCAM_FRAME_MAGIC_3;
        hdr.version = RAWCAM_PROTOCOL_VERSION;
        if (is_rkisp1) {
            hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10 : PIXFMT_Y12);
            hdr.stride = w * 2;
        } else {
            hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10P : PIXFMT_Y12P);
            hdr.stride = stride;
        }
        hdr.flags = FLAG_LOSSLESS;
        hdr.width = w;
        hdr.height = h;
        hdr.payload_size = payload_size;
        hdr.exposure_us = hw_exp;
        hdr.frame_seq = seq;
        hdr.timestamp_ns = monotonic_ns();
        hdr.gain_db_x10 = hw_gain;
        hdr.fps_x100 = hw_fps;
        hdr.bit_depth = bd;
        hdr.roi_x = (uint16_t)rx;
        hdr.roi_y = (uint16_t)ry;

        out->header = hdr;
        if (is_rkisp1) {
            got = bytebuf_reserve(&out->data, payload_size);
            if (got) {
                out->data.size = payload_size;
                unpack_rkisp1_be_to_le_u16(src, stride, out->data.data, w, h, bd);
            }
        } else {
            got = bytebuf_assign(&out->data, src, payload_size);
        }
        cam_qbuf(cam, buf_idx);
        if (!got) {
            snprintf(err, err_sz, "Out of memory copying long-shot frame");
            break;
        }
    }

restore:
    pthread_mutex_lock(&cam->ctrl_mu);
    if (v4l2_started) {
        stop_v4l2_locked(cam);
    }
    if (!write_gain_locked(cam, saved_gain_db)) {
        append_error(err, err_sz, "live gain restore failed");
    }
    char timing_err[128];
    if (!apply_timing_locked(cam, saved_exp_us, saved_fps, NULL, timing_err, sizeof(timing_err))) {
        append_error(err, err_sz, "live exposure/frame rate restore failed");
    }
    refresh_hw_telemetry_locked(cam);
    atomic_store(&cam->keyframe_req, true);
    pthread_mutex_unlock(&cam->ctrl_mu);

    if (was_running) {
        char restart_err[128] = { 0 };
        if (!camera_start_capture(cam, restart_err, sizeof(restart_err))) {
            append_error(err, err_sz, "live stream restart failed");
            append_error(err, err_sz, restart_err);
        }
    }
    pthread_mutex_unlock(&cam->reconfig_mu);
    return got;
}

void camera_request_keyframe(Camera *cam) {
    atomic_store(&cam->keyframe_req, true);
}
