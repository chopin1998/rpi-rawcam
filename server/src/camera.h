#ifndef RAWCAM_CAMERA_H
#define RAWCAM_CAMERA_H

#include "h264_enc.h"
#include "i2c_ctrl.h"
#include "protocol.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAM_MAX_MMAP_BUFS 6

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} ByteBuf;

void bytebuf_init(ByteBuf *b);
bool bytebuf_reserve(ByteBuf *b, size_t n);
bool bytebuf_assign(ByteBuf *b, const void *src, size_t n);
void bytebuf_free(ByteBuf *b);

typedef struct {
    FrameHeader header;
    ByteBuf     data;
} FrameBuffer;

void framebuf_init(FrameBuffer *f);
void framebuf_free(FrameBuffer *f);

typedef struct {
    char     camera_model[64];
    char     video_dev[64];
    char     subdev_dev[64];
    char     i2c_dev[64];

    uint32_t sensor_width;
    uint32_t sensor_height;
    uint32_t min_roi_width;
    uint32_t min_roi_height;

    uint16_t bit_depth;        /* 10 or 12 */
    uint32_t roi_x;
    uint32_t roi_y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    char     stream_mode[16];  /* "h264", "lossless", or "preview8" */

    char     exposure_mode[16];
    uint32_t target_exposure_us;
    uint32_t actual_exposure_us;

    char     gain_mode[16];
    double   target_gain_db;
    double   actual_gain_db;

    double   target_fps;
    double   effective_fps;
    double   sensor_max_fps;
    double   measured_fps;
    bool     fps_clamped_by_exposure;

    uint64_t frame_seq;
    uint64_t dropped_frames;
    bool     streaming;
} CameraStatus;

typedef struct {
    void  *start;
    size_t length;
} CamMmapBuf;

typedef struct {
    int            video_fd;
    int            subdev_fd;
    I2cCtrl        i2c;
    H264HwEncoder  h264_enc;

    CamMmapBuf     buffers[CAM_MAX_MMAP_BUFS];
    uint32_t       buf_count;

    pthread_mutex_t reconfig_mu;
    pthread_mutex_t ctrl_mu;
    CameraStatus    status;

    atomic_bool     running;
    bool            cap_thread_started;
    pthread_t       cap_thread;

    /* Latest stream frame (H264 / lossless / preview8) */
    pthread_mutex_t frame_mu;
    pthread_cond_t  frame_cv;
    FrameBuffer     latest_frame;
    uint64_t        latest_seq;
    atomic_uint_fast64_t frame_seq_counter;
    atomic_uint_fast64_t dropped_frames;

    /* On-demand latest lossless 10/12-bit RAW frame for snapshots */
    pthread_mutex_t raw_mu;
    pthread_cond_t  raw_cv;
    atomic_bool     snap_req;
    uint64_t        snap_done_seq;
    FrameBuffer     latest_raw_frame;

    atomic_uint_fast32_t cached_exp_us;
    atomic_uint_fast32_t cached_gain_x10;
    atomic_uint_fast32_t cached_fps_x100;
    atomic_int           stream_mode_id; /* 0=lossless, 1=preview8, 2=h264 */
    atomic_bool          keyframe_req;
} Camera;

bool camera_init(Camera *cam, char *err, size_t err_sz);
void camera_shutdown(Camera *cam);

bool camera_start_capture(Camera *cam, char *err, size_t err_sz);
void camera_stop_capture(Camera *cam);

void camera_get_status(Camera *cam, CameraStatus *out);
bool camera_set_exposure(Camera *cam, const char *mode, uint32_t exposure_us, char *err, size_t err_sz);
bool camera_set_gain(Camera *cam, const char *mode, double gain_db, char *err, size_t err_sz);
bool camera_set_fps(Camera *cam, double target_fps, char *err, size_t err_sz);
bool camera_set_format(Camera *cam, uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                       uint32_t width, uint32_t height, char *err, size_t err_sz);
bool camera_set_stream_mode(Camera *cam, const char *mode, char *err, size_t err_sz);

bool camera_wait_next_frame(Camera *cam, uint64_t last_seq, FrameBuffer *out, int timeout_ms);
bool camera_get_latest_raw_frame(Camera *cam, FrameBuffer *out);
bool camera_capture_long_shot(Camera *cam, uint32_t exposure_us, double gain_db,
                              FrameBuffer *out, char *err, size_t err_sz);
void camera_request_keyframe(Camera *cam);

#endif /* RAWCAM_CAMERA_H */
