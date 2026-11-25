#include "camera.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#define V4L2_CID_VEYE_MV_BASE  (V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_VEYE_MV_ROI_X (V4L2_CID_VEYE_MV_BASE + 5)
#define V4L2_CID_VEYE_MV_ROI_Y (V4L2_CID_VEYE_MV_BASE + 6)

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

typedef struct {
    const uint8_t *src;
    uint32_t       src_stride;
    uint32_t       src_w;
    uint16_t       bit_depth;
} H264FillCtx;

static void fill_y_for_h264(uint8_t *dst_y, uint32_t dst_stride,
                            uint32_t ew, uint32_t eh, void *user_ctx) {
    H264FillCtx *ctx = (H264FillCtx *)user_ctx;
    if (ctx->bit_depth == 10) {
        uint32_t x_off_quads = (ctx->src_w > ew) ? ((ctx->src_w - ew) / 2 / 4) : 0;
        extract_y10p_to_y8_strided(ctx->src, ctx->src_stride, dst_y, dst_stride, ew, eh, x_off_quads);
    } else {
        uint32_t x_off_pairs = (ctx->src_w > ew) ? ((ctx->src_w - ew) / 2 / 2) : 0;
        extract_y12p_to_y8_strided(ctx->src, ctx->src_stride, dst_y, dst_stride, ew, eh, x_off_pairs);
    }
}

/* ---------------- Device discovery ---------------- */

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

    char video_dev[64] = { 0 };
    char subdev_dev[64] = { 0 };
    d = opendir("/sys/class/video4linux");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char npath[320], vname[128];
            snprintf(npath, sizeof(npath), "/sys/class/video4linux/%.200s/name", ent->d_name);
            if (!read_trim_file(npath, vname, sizeof(vname))) continue;
            if (!video_dev[0] && strcmp(vname, "unicam-image") == 0) {
                snprintf(video_dev, sizeof(video_dev), "/dev/%.50s", ent->d_name);
            } else if (!subdev_dev[0] && strncmp(vname, "mvcam ", 6) == 0) {
                snprintf(subdev_dev, sizeof(subdev_dev), "/dev/%.50s", ent->d_name);
            }
        }
        closedir(d);
    }
    if (!video_dev[0] || !subdev_dev[0]) {
        snprintf(err, err_sz, "V4L2 nodes (unicam-image / mvcam subdev) not found.");
        return false;
    }

    snprintf(cam->status.camera_model, sizeof(cam->status.camera_model), "%s", model);
    snprintf(cam->status.video_dev, sizeof(cam->status.video_dev), "%s", video_dev);
    snprintf(cam->status.subdev_dev, sizeof(cam->status.subdev_dev), "%s", subdev_dev);
    snprintf(cam->status.i2c_dev, sizeof(cam->status.i2c_dev), "%s", i2c_dev);
    return true;
}

/* ---------------- V4L2 & I2C internal helpers (caller holds ctrl_mu) ---------------- */

static void stop_v4l2_locked(Camera *cam) {
    if (cam->video_fd >= 0 && cam->status.streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(cam->video_fd, VIDIOC_REQBUFS, &req);
    }
}

static bool start_v4l2_locked(Camera *cam, char *err, size_t err_sz) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = CAM_MAX_MMAP_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->video_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        snprintf(err, err_sz, "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }
    if (req.count > CAM_MAX_MMAP_BUFS) req.count = CAM_MAX_MMAP_BUFS;

    cam->buf_count = 0;
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            snprintf(err, err_sz, "VIDIOC_QUERYBUF failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
        void *ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         cam->video_fd, buf.m.offset);
        if (ptr == MAP_FAILED) {
            snprintf(err, err_sz, "mmap failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
        cam->buffers[i].start = ptr;
        cam->buffers[i].length = buf.length;
        cam->buf_count = i + 1;
    }

    for (uint32_t i = 0; i < cam->buf_count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->video_fd, VIDIOC_QBUF, &buf) < 0) {
            snprintf(err, err_sz, "VIDIOC_QBUF failed: %s", strerror(errno));
            stop_v4l2_locked(cam);
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->video_fd, VIDIOC_STREAMON, &type) < 0) {
        snprintf(err, err_sz, "VIDIOC_STREAMON failed: %s", strerror(errno));
        stop_v4l2_locked(cam);
        return false;
    }
    cam->status.streaming = true;
    return true;
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

    struct v4l2_subdev_format sfmt;
    memset(&sfmt, 0, sizeof(sfmt));
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sfmt.pad = 0;
    sfmt.format.width = width;
    sfmt.format.height = height;
    sfmt.format.code = (bit_depth == 10) ? MEDIA_BUS_FMT_Y10_1X10 : MEDIA_BUS_FMT_Y12_1X12;
    sfmt.format.field = V4L2_FIELD_NONE;
    if (xioctl(cam->subdev_fd, VIDIOC_SUBDEV_S_FMT, &sfmt) < 0) {
        snprintf(err, err_sz, "VIDIOC_SUBDEV_S_FMT failed: %s", strerror(errno));
        return false;
    }
    xioctl(cam->subdev_fd, VIDIOC_SUBDEV_G_FMT, &sfmt);

    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = sfmt.format.width ? sfmt.format.width : width;
    vfmt.fmt.pix.height = sfmt.format.height ? sfmt.format.height : height;
    vfmt.fmt.pix.pixelformat = (bit_depth == 10) ? V4L2_PIX_FMT_Y10P : V4L2_PIX_FMT_Y12P;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(cam->video_fd, VIDIOC_S_FMT, &vfmt) < 0) {
        snprintf(err, err_sz, "VIDIOC_S_FMT failed: %s", strerror(errno));
        return false;
    }

    cam->status.bit_depth = bit_depth;
    cam->status.roi_x = roi_x;
    cam->status.roi_y = roi_y;
    cam->status.width = vfmt.fmt.pix.width;
    cam->status.height = vfmt.fmt.pix.height;
    cam->status.stride = vfmt.fmt.pix.bytesperline;

    /* Reopen hardware H.264 encoder for new dimensions */
    h264_enc_close(&cam->h264_enc);
    if (!h264_enc_open(&cam->h264_enc, cam->status.width, cam->status.height, 8000000, 30, "/dev/video11")) {
        if (atomic_load(&cam->stream_mode_id) == 2) {
            snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "lossless");
            atomic_store(&cam->stream_mode_id, 0);
        }
    }
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

static void apply_adaptive_exposure_fps_locked(Camera *cam, bool *need_restart_for_fps) {
    uint32_t max_fps_reg = 6000;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MAX_FRAME_RATE, &max_fps_reg) && max_fps_reg > 0) {
        cam->status.sensor_max_fps = max_fps_reg / 100.0;
    }
    uint32_t cur_fps_reg = 3000;
    i2c_ctrl_read_u32(&cam->i2c, REG_FRAMERATE, &cur_fps_reg);
    double cur_hw_fps = cur_fps_reg / 100.0;

    double desired_fps = clamp_dbl(cam->status.target_fps, 0.1, cam->status.sensor_max_fps);
    double effective_fps = desired_fps;
    bool clamped = false;

    uint32_t exp_us = clamp_u32(cam->status.target_exposure_us, 10u, 10000000u);
    double exp_max_fps = 980000.0 / (double)exp_us;
    if (exp_max_fps < desired_fps) {
        effective_fps = exp_max_fps > 0.1 ? exp_max_fps : 0.1;
        clamped = true;
    }
    uint32_t new_fps_reg = clamp_u32((uint32_t)llround(effective_fps * 100.0), 10u, max_fps_reg);

    /* VEYE sensor locks Framerate register while streaming; request brief restart if FPS changes */
    if (need_restart_for_fps && cam->status.streaming && new_fps_reg != cur_fps_reg) {
        *need_restart_for_fps = true;
        return;
    }

    i2c_ctrl_write_u32(&cam->i2c, REG_EXPOSURE_MODE, 2); /* Manual */
    if (effective_fps < cur_hw_fps) {
        i2c_ctrl_write_u32(&cam->i2c, REG_FRAMERATE, new_fps_reg);
        usleep(8000);
        i2c_ctrl_write_u32(&cam->i2c, REG_ME_TIME, exp_us);
    } else {
        i2c_ctrl_write_u32(&cam->i2c, REG_ME_TIME, exp_us);
        usleep(8000);
        i2c_ctrl_write_u32(&cam->i2c, REG_FRAMERATE, new_fps_reg);
    }

    cam->status.effective_fps = effective_fps;
    cam->status.fps_clamped_by_exposure = clamped;
    atomic_store(&cam->cached_fps_x100, new_fps_reg);
}

static bool apply_adaptive_with_restart(Camera *cam, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->reconfig_mu);

    bool need_restart = false;
    pthread_mutex_lock(&cam->ctrl_mu);
    apply_adaptive_exposure_fps_locked(cam, &need_restart);
    if (!need_restart) {
        refresh_hw_telemetry_locked(cam);
        pthread_mutex_unlock(&cam->ctrl_mu);
        pthread_mutex_unlock(&cam->reconfig_mu);
        return true;
    }
    pthread_mutex_unlock(&cam->ctrl_mu);

    bool was_running = atomic_load(&cam->running);
    if (was_running) {
        camera_stop_capture(cam);
    }

    pthread_mutex_lock(&cam->ctrl_mu);
    apply_adaptive_exposure_fps_locked(cam, NULL);
    refresh_hw_telemetry_locked(cam);
    pthread_mutex_unlock(&cam->ctrl_mu);

    bool ok = true;
    if (was_running) {
        ok = camera_start_capture(cam, err, err_sz);
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
    bytebuf_init(&work_buf);

    while (atomic_load(&cam->running)) {
        struct pollfd pfd = { cam->video_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(cam->video_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        const uint8_t *src = (const uint8_t *)cam->buffers[buf.index].start;
        const uint32_t bytesused = buf.bytesused;
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
        const uint32_t max_raw = stride * h;
        const uint32_t raw_payload_size = bytesused < max_raw ? bytesused : max_raw;

        FrameHeader raw_hdr;
        memset(&raw_hdr, 0, sizeof(raw_hdr));
        raw_hdr.magic[0] = RAWCAM_FRAME_MAGIC_0;
        raw_hdr.magic[1] = RAWCAM_FRAME_MAGIC_1;
        raw_hdr.magic[2] = RAWCAM_FRAME_MAGIC_2;
        raw_hdr.magic[3] = RAWCAM_FRAME_MAGIC_3;
        raw_hdr.version = RAWCAM_PROTOCOL_VERSION;
        raw_hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10P : PIXFMT_Y12P);
        raw_hdr.flags = FLAG_LOSSLESS;
        raw_hdr.width = w;
        raw_hdr.height = h;
        raw_hdr.stride = stride;
        raw_hdr.payload_size = raw_payload_size;
        raw_hdr.exposure_us = (uint32_t)atomic_load(&cam->cached_exp_us);
        raw_hdr.frame_seq = seq;
        raw_hdr.timestamp_ns = ts_ns;
        raw_hdr.gain_db_x10 = (uint32_t)atomic_load(&cam->cached_gain_x10);
        raw_hdr.fps_x100 = (uint32_t)atomic_load(&cam->cached_fps_x100);
        raw_hdr.bit_depth = bd;
        raw_hdr.roi_x = (uint16_t)rx;
        raw_hdr.roi_y = (uint16_t)ry;

        /* On-demand 10/12-bit lossless copy for snapshot requests (port 5602) */
        if (atomic_load_explicit(&cam->snap_req, memory_order_relaxed)) {
            pthread_mutex_lock(&cam->raw_mu);
            cam->latest_raw_frame.header = raw_hdr;
            bytebuf_assign(&cam->latest_raw_frame.data, src, raw_payload_size);
            cam->snap_done_seq = seq;
            atomic_store(&cam->snap_req, false);
            pthread_cond_broadcast(&cam->raw_cv);
            pthread_mutex_unlock(&cam->raw_mu);
        }

        int mode_id = atomic_load(&cam->stream_mode_id);
        if (mode_id == 0) {
            /* Lossless MIPI packed directly from Unicam DMA */
            pthread_mutex_lock(&cam->frame_mu);
            cam->latest_frame.header = raw_hdr;
            bytebuf_assign(&cam->latest_frame.data, src, raw_payload_size);
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
                if (bd == 10) {
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
            /* Hardware H.264 encode via /dev/video11 */
            if (atomic_exchange(&cam->keyframe_req, false)) {
                h264_enc_force_keyframe(&cam->h264_enc);
            }
            H264FillCtx fctx = { src, stride, w, bd };
            bool is_kf = false;
            size_t nal_len = 0;
            if (h264_enc_encode(&cam->h264_enc, fill_y_for_h264, &fctx,
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
        }

        xioctl(cam->video_fd, VIDIOC_QBUF, &buf);

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

    cam->status.sensor_width = 1944;
    cam->status.sensor_height = 1096;
    cam->status.min_roi_width = 368;
    cam->status.min_roi_height = 304;

    uint32_t v = 0;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_SENSOR_WIDTH, &v) && v > 0) cam->status.sensor_width = v;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_SENSOR_HEIGHT, &v) && v > 0) cam->status.sensor_height = v;
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MIN_ROI_WIDTH, &v) && v >= 64 && v <= cam->status.sensor_width) {
        cam->status.min_roi_width = v;
    }
    if (i2c_ctrl_read_u32(&cam->i2c, REG_MIN_ROI_HEIGHT, &v) && v >= 64 && v <= cam->status.sensor_height) {
        cam->status.min_roi_height = v;
    }

    cam->status.bit_depth = 12;
    cam->status.roi_x = 0;
    cam->status.roi_y = 0;
    cam->status.width = cam->status.sensor_width;
    cam->status.height = cam->status.sensor_height;
    snprintf(cam->status.exposure_mode, sizeof(cam->status.exposure_mode), "manual");
    cam->status.target_exposure_us = 10000;
    snprintf(cam->status.gain_mode, sizeof(cam->status.gain_mode), "manual");
    cam->status.target_gain_db = 0.0;
    cam->status.target_fps = 30.0;
    snprintf(cam->status.stream_mode, sizeof(cam->status.stream_mode), "h264");

    if (!configure_pipeline_locked(cam, cam->status.bit_depth, 0, 0,
                                   cam->status.width, cam->status.height, err, err_sz)) {
        return false;
    }

    i2c_ctrl_write_u32(&cam->i2c, REG_GAIN_MODE, 2);
    i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, 0);
    apply_adaptive_exposure_fps_locked(cam, NULL);
    refresh_hw_telemetry_locked(cam);
    return true;
}

void camera_shutdown(Camera *cam) {
    camera_stop_capture(cam);
    pthread_mutex_lock(&cam->ctrl_mu);
    h264_enc_close(&cam->h264_enc);
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
    pthread_mutex_lock(&cam->ctrl_mu);
    snprintf(cam->status.exposure_mode, sizeof(cam->status.exposure_mode), "manual");
    if (exposure_us > 0) {
        cam->status.target_exposure_us = clamp_u32(exposure_us, 10u, 10000000u);
    }
    pthread_mutex_unlock(&cam->ctrl_mu);
    return apply_adaptive_with_restart(cam, err, err_sz);
}

bool camera_set_gain(Camera *cam, const char *mode, double gain_db, char *err, size_t err_sz) {
    (void)mode;
    (void)err;
    (void)err_sz;
    pthread_mutex_lock(&cam->ctrl_mu);
    snprintf(cam->status.gain_mode, sizeof(cam->status.gain_mode), "manual");
    cam->status.target_gain_db = clamp_dbl(gain_db, 0.0, 48.0);
    uint32_t gain_reg = (uint32_t)llround(cam->status.target_gain_db * 10.0);
    i2c_ctrl_write_u32(&cam->i2c, REG_GAIN_MODE, 2);
    i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, gain_reg);
    refresh_hw_telemetry_locked(cam);
    pthread_mutex_unlock(&cam->ctrl_mu);
    return true;
}

bool camera_set_fps(Camera *cam, double target_fps, char *err, size_t err_sz) {
    if (target_fps <= 0.0 || target_fps > 240.0) {
        snprintf(err, err_sz, "target_fps out of range");
        return false;
    }
    pthread_mutex_lock(&cam->ctrl_mu);
    cam->status.target_fps = target_fps;
    pthread_mutex_unlock(&cam->ctrl_mu);
    return apply_adaptive_with_restart(cam, err, err_sz);
}

bool camera_set_format(Camera *cam, uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                       uint32_t width, uint32_t height, char *err, size_t err_sz) {
    pthread_mutex_lock(&cam->reconfig_mu);
    bool was_running = atomic_load(&cam->running);
    if (was_running) camera_stop_capture(cam);

    pthread_mutex_lock(&cam->ctrl_mu);
    bool ok = configure_pipeline_locked(cam, bit_depth, roi_x, roi_y, width, height, err, err_sz);
    if (ok) {
        apply_adaptive_exposure_fps_locked(cam, NULL);
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
    else {
        snprintf(err, err_sz, "stream_mode must be 'h264', 'lossless', or 'preview8'");
        return false;
    }

    pthread_mutex_lock(&cam->ctrl_mu);
    if (id == 2 && !cam->h264_enc.streaming) {
        if (!h264_enc_open(&cam->h264_enc, cam->status.width, cam->status.height, 8000000, 30, "/dev/video11")) {
            pthread_mutex_unlock(&cam->ctrl_mu);
            snprintf(err, err_sz, "Hardware H.264 encoder (/dev/video11) unavailable");
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

    uint32_t max_fps_reg = 6000;
    i2c_ctrl_read_u32(&cam->i2c, REG_MAX_FRAME_RATE, &max_fps_reg);
    double exp_fps = 980000.0 / (double)exposure_us;
    uint32_t fps_reg = clamp_u32((uint32_t)llround(exp_fps * 100.0), 10u, max_fps_reg);

    i2c_ctrl_write_u32(&cam->i2c, REG_EXPOSURE_MODE, 2);
    i2c_ctrl_write_u32(&cam->i2c, REG_FRAMERATE, fps_reg);
    usleep(10000);
    i2c_ctrl_write_u32(&cam->i2c, REG_ME_TIME, exposure_us);
    i2c_ctrl_write_u32(&cam->i2c, REG_GAIN_MODE, 2);
    i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, (uint32_t)llround(gain_db * 10.0));
    usleep(10000);
    refresh_hw_telemetry_locked(cam);

    if (!start_v4l2_locked(cam, err, err_sz)) {
        cam->status.target_exposure_us = saved_exp_us;
        cam->status.target_gain_db = saved_gain_db;
        cam->status.target_fps = saved_fps;
        i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, (uint32_t)llround(saved_gain_db * 10.0));
        apply_adaptive_exposure_fps_locked(cam, NULL);
        pthread_mutex_unlock(&cam->ctrl_mu);
        pthread_mutex_unlock(&cam->reconfig_mu);
        return false;
    }
    pthread_mutex_unlock(&cam->ctrl_mu);

    bool got = false;
    uint64_t t_start = monotonic_ns();
    int timeout_ms = (int)(exposure_us / 1000) + 4000;

    for (int attempt = 0; attempt < 3 && !got; ++attempt) {
        struct pollfd pfd = { cam->video_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            snprintf(err, err_sz, "Timed out waiting for long-exposure frame");
            break;
        }
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(cam->video_fd, VIDIOC_DQBUF, &buf) < 0) {
            snprintf(err, err_sz, "VIDIOC_DQBUF failed: %s", strerror(errno));
            break;
        }
        double elapsed_ms = (double)(monotonic_ns() - t_start) / 1e6;
        if (attempt == 0 && exposure_us >= 500000 && elapsed_ms < (exposure_us / 1000.0) * 0.4) {
            xioctl(cam->video_fd, VIDIOC_QBUF, &buf);
            t_start = monotonic_ns();
            continue;
        }

        const uint8_t *src = (const uint8_t *)cam->buffers[buf.index].start;
        uint32_t max_raw = stride * h;
        uint32_t payload_size = buf.bytesused < max_raw ? buf.bytesused : max_raw;
        uint64_t seq = atomic_fetch_add(&cam->frame_seq_counter, 1) + 1;

        uint32_t hw_exp = exposure_us;
        uint32_t hw_gain = (uint32_t)llround(gain_db * 10.0);
        uint32_t hw_fps = 100;
        i2c_ctrl_read_u32(&cam->i2c, REG_EXP_TIME, &hw_exp);
        i2c_ctrl_read_u32(&cam->i2c, REG_CUR_GAIN, &hw_gain);
        i2c_ctrl_read_u32(&cam->i2c, REG_FRAMERATE, &hw_fps);

        FrameHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic[0] = RAWCAM_FRAME_MAGIC_0;
        hdr.magic[1] = RAWCAM_FRAME_MAGIC_1;
        hdr.magic[2] = RAWCAM_FRAME_MAGIC_2;
        hdr.magic[3] = RAWCAM_FRAME_MAGIC_3;
        hdr.version = RAWCAM_PROTOCOL_VERSION;
        hdr.pixel_format = (uint16_t)(bd == 10 ? PIXFMT_Y10P : PIXFMT_Y12P);
        hdr.flags = FLAG_LOSSLESS;
        hdr.width = w;
        hdr.height = h;
        hdr.stride = stride;
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
        got = bytebuf_assign(&out->data, src, payload_size);
        xioctl(cam->video_fd, VIDIOC_QBUF, &buf);
    }

    pthread_mutex_lock(&cam->ctrl_mu);
    stop_v4l2_locked(cam);
    cam->status.target_exposure_us = saved_exp_us;
    cam->status.target_gain_db = saved_gain_db;
    cam->status.target_fps = saved_fps;
    i2c_ctrl_write_u32(&cam->i2c, REG_GAIN_MODE, 2);
    i2c_ctrl_write_u32(&cam->i2c, REG_MANUAL_GAIN, (uint32_t)llround(saved_gain_db * 10.0));
    apply_adaptive_exposure_fps_locked(cam, NULL);
    refresh_hw_telemetry_locked(cam);
    atomic_store(&cam->keyframe_req, true);
    pthread_mutex_unlock(&cam->ctrl_mu);

    if (was_running) {
        char restart_err[128];
        camera_start_capture(cam, restart_err, sizeof(restart_err));
    }
    pthread_mutex_unlock(&cam->reconfig_mu);
    return got;
}

void camera_request_keyframe(Camera *cam) {
    atomic_store(&cam->keyframe_req, true);
}
