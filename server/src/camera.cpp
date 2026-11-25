#include "camera.hpp"

#include <algorithm>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace rawcam {

namespace {

constexpr uint32_t V4L2_CID_VEYE_MV_BASE   = V4L2_CID_USER_BASE + 0x1000;
constexpr uint32_t V4L2_CID_VEYE_MV_ROI_X  = V4L2_CID_VEYE_MV_BASE + 5;
constexpr uint32_t V4L2_CID_VEYE_MV_ROI_Y  = V4L2_CID_VEYE_MV_BASE + 6;

constexpr uint32_t ROI_W_ALIGN = 8;
constexpr uint32_t ROI_H_ALIGN = 4;

std::string read_trim(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

uint64_t monotonic_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

void extract_y12p_to_y8_strided(const uint8_t* src, uint32_t src_stride,
                                uint8_t* dst, uint32_t dst_stride,
                                uint32_t width, uint32_t height, uint32_t x_offset_pairs = 0) {
    const uint32_t pairs = width / 2;
    const uint32_t byte_off = x_offset_pairs * 3;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * src_stride + byte_off;
        uint8_t* d = dst + static_cast<size_t>(y) * dst_stride;
        uint32_t x = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
        for (; x + 16 <= pairs; x += 16) {
            uint8x16x3_t v = vld3q_u8(s + 3 * x);
            uint8x16x2_t out = {v.val[0], v.val[1]};
            vst2q_u8(d + 2 * x, out);
        }
#endif
        for (; x < pairs; ++x) {
            d[2 * x]     = s[3 * x];
            d[2 * x + 1] = s[3 * x + 1];
        }
    }
}

void extract_y10p_to_y8_strided(const uint8_t* src, uint32_t src_stride,
                                uint8_t* dst, uint32_t dst_stride,
                                uint32_t width, uint32_t height, uint32_t x_offset_quads = 0) {
    const uint32_t quads = width / 4;
    const uint32_t byte_off = x_offset_quads * 5;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * src_stride + byte_off;
        uint8_t* d = dst + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < quads; ++x) {
            d[4 * x]     = s[5 * x];
            d[4 * x + 1] = s[5 * x + 1];
            d[4 * x + 2] = s[5 * x + 2];
            d[4 * x + 3] = s[5 * x + 3];
        }
    }
}

} // namespace

Camera::~Camera() {
    shutdown();
}

bool Camera::discover_devices(std::string& err) {
    std::string i2c_dev;
    std::string model = "VEYE-RAWCAM";

    if (fs::exists("/sys/bus/i2c/devices")) {
        for (const auto& entry : fs::directory_iterator("/sys/bus/i2c/devices")) {
            std::string name = entry.path().filename().string();
            if (name.size() > 5 && name.rfind("-003b") == name.size() - 5) {
                std::string bus_num = name.substr(0, name.size() - 5);
                i2c_dev = "/dev/i2c-" + bus_num;
                auto model_path = entry.path() / "veye_mvcam" / "camera_model";
                if (fs::exists(model_path)) {
                    std::string m = read_trim(model_path);
                    if (!m.empty()) model = m;
                }
                break;
            }
        }
    }
    if (i2c_dev.empty()) {
        err = "VEYE camera (I2C 0x3b) not found. Load driver with: sudo ~/build/imx462-camera.sh start";
        return false;
    }

    std::string video_dev;
    std::string subdev_dev;
    if (fs::exists("/sys/class/video4linux")) {
        for (const auto& entry : fs::directory_iterator("/sys/class/video4linux")) {
            std::string dev_name = entry.path().filename().string();
            std::string v4l_name = read_trim(entry.path() / "name");
            if (video_dev.empty() && v4l_name == "unicam-image") {
                video_dev = "/dev/" + dev_name;
            } else if (subdev_dev.empty() && v4l_name.rfind("mvcam ", 0) == 0) {
                subdev_dev = "/dev/" + dev_name;
            }
        }
    }
    if (video_dev.empty() || subdev_dev.empty()) {
        err = "V4L2 nodes (unicam-image / mvcam subdev) not found. Run: sudo ~/build/imx462-camera.sh start";
        return false;
    }

    status_.camera_model = model;
    status_.video_dev = video_dev;
    status_.subdev_dev = subdev_dev;
    status_.i2c_dev = i2c_dev;
    return true;
}

bool Camera::init(std::string& err) {
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    if (!discover_devices(err)) return false;

    video_fd_ = ::open(status_.video_dev.c_str(), O_RDWR | O_NONBLOCK);
    if (video_fd_ < 0) {
        err = "Failed to open " + status_.video_dev + ": " + std::strerror(errno);
        return false;
    }

    subdev_fd_ = ::open(status_.subdev_dev.c_str(), O_RDWR);
    if (subdev_fd_ < 0) {
        err = "Failed to open " + status_.subdev_dev + ": " + std::strerror(errno);
        ::close(video_fd_);
        video_fd_ = -1;
        return false;
    }

    if (!i2c_.open_dev(status_.i2c_dev, 0x3b)) {
        err = "Failed to open " + status_.i2c_dev + ": " + std::strerror(errno);
        ::close(subdev_fd_);
        ::close(video_fd_);
        subdev_fd_ = video_fd_ = -1;
        return false;
    }

    uint32_t v = 0;
    if (i2c_.read_u32(reg::SENSOR_WIDTH, v) && v > 0) status_.sensor_width = v;
    if (i2c_.read_u32(reg::SENSOR_HEIGHT, v) && v > 0) status_.sensor_height = v;
    if (i2c_.read_u32(reg::MIN_ROI_WIDTH, v) && v >= 64 && v <= status_.sensor_width) status_.min_roi_width = v;
    if (i2c_.read_u32(reg::MIN_ROI_HEIGHT, v) && v >= 64 && v <= status_.sensor_height) status_.min_roi_height = v;

    status_.bit_depth = 12;
    status_.roi_x = 0;
    status_.roi_y = 0;
    status_.width = status_.sensor_width;
    status_.height = status_.sensor_height;
    status_.exposure_mode = "manual";
    status_.target_exposure_us = 10000;
    status_.gain_mode = "manual";
    status_.target_gain_db = 0.0;
    status_.target_fps = 30.0;
    status_.stream_mode = "h264";
    stream_mode_id_.store(2);

    if (!configure_pipeline_locked(status_.bit_depth, status_.roi_x, status_.roi_y,
                                   status_.width, status_.height, err)) {
        return false;
    }

    i2c_.write_u32(reg::GAIN_MODE, 2);
    i2c_.write_u32(reg::MANUAL_GAIN, 0);
    apply_adaptive_exposure_fps_locked(nullptr);
    refresh_hw_telemetry_locked();
    return true;
}

void Camera::shutdown() {
    stop_capture();
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    h264_enc_.close_enc();
    stop_v4l2_locked();
    i2c_.close_dev();
    if (subdev_fd_ >= 0) {
        ::close(subdev_fd_);
        subdev_fd_ = -1;
    }
    if (video_fd_ >= 0) {
        ::close(video_fd_);
        video_fd_ = -1;
    }
}

bool Camera::configure_pipeline_locked(uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                                       uint32_t width, uint32_t height, std::string& err) {
    if (bit_depth != 10 && bit_depth != 12) {
        err = "bit_depth must be 10 or 12";
        return false;
    }

    roi_x = (roi_x / ROI_W_ALIGN) * ROI_W_ALIGN;
    roi_y = (roi_y / ROI_H_ALIGN) * ROI_H_ALIGN;
    if (roi_x + status_.min_roi_width > status_.sensor_width) {
        roi_x = ((status_.sensor_width - status_.min_roi_width) / ROI_W_ALIGN) * ROI_W_ALIGN;
    }
    if (roi_y + status_.min_roi_height > status_.sensor_height) {
        roi_y = ((status_.sensor_height - status_.min_roi_height) / ROI_H_ALIGN) * ROI_H_ALIGN;
    }
    width  = std::clamp(width, status_.min_roi_width, status_.sensor_width - roi_x);
    height = std::clamp(height, status_.min_roi_height, status_.sensor_height - roi_y);
    width  = (width / ROI_W_ALIGN) * ROI_W_ALIGN;
    height = (height / ROI_H_ALIGN) * ROI_H_ALIGN;

    struct v4l2_control ctrl{};
    ctrl.id = V4L2_CID_VEYE_MV_ROI_X;
    ctrl.value = static_cast<int32_t>(roi_x);
    if (xioctl(subdev_fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        err = std::string("VIDIOC_S_CTRL ROI_X failed: ") + std::strerror(errno);
        return false;
    }
    ctrl.id = V4L2_CID_VEYE_MV_ROI_Y;
    ctrl.value = static_cast<int32_t>(roi_y);
    if (xioctl(subdev_fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        err = std::string("VIDIOC_S_CTRL ROI_Y failed: ") + std::strerror(errno);
        return false;
    }

    struct v4l2_subdev_format sfmt{};
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sfmt.pad = 0;
    sfmt.format.width = width;
    sfmt.format.height = height;
    sfmt.format.code = (bit_depth == 10) ? MEDIA_BUS_FMT_Y10_1X10 : MEDIA_BUS_FMT_Y12_1X12;
    sfmt.format.field = V4L2_FIELD_NONE;
    if (xioctl(subdev_fd_, VIDIOC_SUBDEV_S_FMT, &sfmt) < 0) {
        err = std::string("VIDIOC_SUBDEV_S_FMT failed: ") + std::strerror(errno);
        return false;
    }
    xioctl(subdev_fd_, VIDIOC_SUBDEV_G_FMT, &sfmt);

    struct v4l2_format vfmt{};
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = sfmt.format.width ? sfmt.format.width : width;
    vfmt.fmt.pix.height = sfmt.format.height ? sfmt.format.height : height;
    vfmt.fmt.pix.pixelformat = (bit_depth == 10) ? V4L2_PIX_FMT_Y10P : V4L2_PIX_FMT_Y12P;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(video_fd_, VIDIOC_S_FMT, &vfmt) < 0) {
        err = std::string("VIDIOC_S_FMT failed: ") + std::strerror(errno);
        return false;
    }

    status_.bit_depth = bit_depth;
    status_.roi_x = roi_x;
    status_.roi_y = roi_y;
    status_.width = vfmt.fmt.pix.width;
    status_.height = vfmt.fmt.pix.height;
    status_.stride = vfmt.fmt.pix.bytesperline;

    // Reopen hardware H.264 encoder to match new dimensions
    h264_enc_.close_enc();
    if (!h264_enc_.open_enc(status_.width, status_.height, 8000000, 30)) {
        // Fallback to lossless if hardware encoder unavailable
        if (stream_mode_id_.load() == 2) {
            status_.stream_mode = "lossless";
            stream_mode_id_.store(0);
        }
    }
    keyframe_req_.store(true);
    return true;
}

bool Camera::start_v4l2_locked(std::string& err) {
    struct v4l2_requestbuffers req{};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(video_fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        err = std::string("VIDIOC_REQBUFS failed: ") + std::strerror(errno);
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(video_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            err = std::string("VIDIOC_QUERYBUF failed: ") + std::strerror(errno);
            stop_v4l2_locked();
            return false;
        }
        void* ptr = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                           video_fd_, buf.m.offset);
        if (ptr == MAP_FAILED) {
            err = std::string("mmap failed: ") + std::strerror(errno);
            stop_v4l2_locked();
            return false;
        }
        buffers_[i] = {ptr, buf.length};
    }

    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(video_fd_, VIDIOC_QBUF, &buf) < 0) {
            err = std::string("VIDIOC_QBUF failed: ") + std::strerror(errno);
            stop_v4l2_locked();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(video_fd_, VIDIOC_STREAMON, &type) < 0) {
        err = std::string("VIDIOC_STREAMON failed: ") + std::strerror(errno);
        stop_v4l2_locked();
        return false;
    }

    status_.streaming = true;
    return true;
}

void Camera::stop_v4l2_locked() {
    if (video_fd_ >= 0 && status_.streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(video_fd_, VIDIOC_STREAMOFF, &type);
        status_.streaming = false;
    }
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) {
            ::munmap(b.start, b.length);
        }
    }
    buffers_.clear();
    if (video_fd_ >= 0) {
        struct v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(video_fd_, VIDIOC_REQBUFS, &req);
    }
}

void Camera::apply_adaptive_exposure_fps_locked(bool* need_restart_for_fps) {
    uint32_t max_fps_reg = 6000;
    if (i2c_.read_u32(reg::MAX_FRAME_RATE, max_fps_reg) && max_fps_reg > 0) {
        status_.sensor_max_fps = max_fps_reg / 100.0;
    }
    uint32_t cur_fps_reg = 3000;
    i2c_.read_u32(reg::FRAMERATE, cur_fps_reg);
    double cur_hw_fps = cur_fps_reg / 100.0;

    double desired_fps = std::clamp(status_.target_fps, 0.1, status_.sensor_max_fps);
    double effective_fps = desired_fps;
    bool clamped = false;

    uint32_t exp_us = std::clamp(status_.target_exposure_us, 10u, 10000000u);
    double exp_max_fps = 980000.0 / static_cast<double>(exp_us);
    if (exp_max_fps < desired_fps) {
        effective_fps = std::max(0.1, exp_max_fps);
        clamped = true;
    }
    uint32_t new_fps_reg = static_cast<uint32_t>(std::lround(effective_fps * 100.0));
    new_fps_reg = std::clamp<uint32_t>(new_fps_reg, 10, max_fps_reg);

    if (need_restart_for_fps && status_.streaming && new_fps_reg != cur_fps_reg) {
        *need_restart_for_fps = true;
        return;
    }

    i2c_.write_u32(reg::EXPOSURE_MODE, 2); // Manual
    if (effective_fps < cur_hw_fps) {
        i2c_.write_u32(reg::FRAMERATE, new_fps_reg);
        ::usleep(8000);
        i2c_.write_u32(reg::ME_TIME, exp_us);
    } else {
        i2c_.write_u32(reg::ME_TIME, exp_us);
        ::usleep(8000);
        i2c_.write_u32(reg::FRAMERATE, new_fps_reg);
    }

    status_.effective_fps = effective_fps;
    status_.fps_clamped_by_exposure = clamped;
    cached_fps_x100_.store(new_fps_reg);
}

bool Camera::apply_adaptive_with_restart(std::string& err) {
    std::lock_guard<std::mutex> rlk(reconfig_mu_);
    bool need_restart = false;
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        apply_adaptive_exposure_fps_locked(&need_restart);
        if (!need_restart) {
            refresh_hw_telemetry_locked();
            return true;
        }
    }
    bool was_running = running_.load();
    if (was_running) stop_capture();
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        apply_adaptive_exposure_fps_locked(nullptr);
        refresh_hw_telemetry_locked();
    }
    if (was_running) {
        return start_capture(err);
    }
    return true;
}

void Camera::refresh_hw_telemetry_locked() {
    uint32_t v = 0;
    if (i2c_.read_u32(reg::EXP_TIME, v)) {
        status_.actual_exposure_us = v;
        cached_exp_us_.store(v);
    }
    if (i2c_.read_u32(reg::CUR_GAIN, v)) {
        status_.actual_gain_db = v / 10.0;
        cached_gain_x10_.store(v);
    }
    if (i2c_.read_u32(reg::FRAMERATE, v) && v > 0) {
        status_.effective_fps = v / 100.0;
        cached_fps_x100_.store(v);
    }
    if (i2c_.read_u32(reg::MAX_FRAME_RATE, v) && v > 0) {
        status_.sensor_max_fps = v / 100.0;
    }
}

bool Camera::start_capture(std::string& err) {
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    if (running_.load()) return true;
    if (!start_v4l2_locked(err)) return false;
    running_.store(true);
    cap_thread_ = std::thread(&Camera::capture_loop, this);
    return true;
}

void Camera::stop_capture() {
    running_.store(false);
    frame_cv_.notify_all();
    if (cap_thread_.joinable()) {
        cap_thread_.join();
    }
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    stop_v4l2_locked();
}

void Camera::capture_loop() {
    uint64_t fps_frames = 0;
    auto fps_t0 = std::chrono::steady_clock::now();
    auto telem_t0 = fps_t0;

    std::vector<uint8_t> work_buf;

    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = video_fd_;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) continue;

        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(video_fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        const uint8_t* src = static_cast<const uint8_t*>(buffers_[buf.index].start);
        const uint32_t bytesused = buf.bytesused;
        const uint64_t ts_ns = monotonic_ns();

        uint32_t w, h, stride, rx, ry;
        uint16_t bd;
        {
            std::lock_guard<std::mutex> lk(ctrl_mu_);
            w = status_.width;
            h = status_.height;
            stride = status_.stride;
            rx = status_.roi_x;
            ry = status_.roi_y;
            bd = status_.bit_depth;
        }

        const uint64_t seq = frame_seq_counter_.fetch_add(1) + 1;
        const uint32_t raw_payload_size = std::min<uint32_t>(bytesused, stride * h);

        // Build lossless RAW header & always keep latest lossless frame for 12-bit snapshots
        FrameHeader raw_hdr{};
        std::memcpy(raw_hdr.magic, FRAME_MAGIC, 4);
        raw_hdr.version = PROTOCOL_VERSION;
        raw_hdr.pixel_format = static_cast<uint16_t>(bd == 10 ? PixelFormat::Y10P : PixelFormat::Y12P);
        raw_hdr.flags = FLAG_LOSSLESS;
        raw_hdr.width = w;
        raw_hdr.height = h;
        raw_hdr.stride = stride;
        raw_hdr.payload_size = raw_payload_size;
        raw_hdr.exposure_us = cached_exp_us_.load();
        raw_hdr.frame_seq = seq;
        raw_hdr.timestamp_ns = ts_ns;
        raw_hdr.gain_db_x10 = cached_gain_x10_.load();
        raw_hdr.fps_x100 = cached_fps_x100_.load();
        raw_hdr.bit_depth = bd;
        raw_hdr.roi_x = static_cast<uint16_t>(rx);
        raw_hdr.roi_y = static_cast<uint16_t>(ry);

        if (snap_req_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> rlk(raw_mu_);
            latest_raw_frame_.header = raw_hdr;
            latest_raw_frame_.data.resize(raw_payload_size);
            std::memcpy(latest_raw_frame_.data.data(), src, raw_payload_size);
            snap_done_seq_ = seq;
            snap_req_.store(false);
            raw_cv_.notify_all();
        }

        int mode_id = stream_mode_id_.load();
        if (mode_id == 0) {
            // Lossless MIPI packed directly from DMA
            std::lock_guard<std::mutex> flk(frame_mu_);
            latest_frame_.header = raw_hdr;
            latest_frame_.data.resize(raw_payload_size);
            std::memcpy(latest_frame_.data.data(), src, raw_payload_size);
            latest_seq_ = seq;
        } else if (mode_id == 1) {
            // Uncompressed 8-bit preview
            FrameHeader hdr = raw_hdr;
            hdr.pixel_format = static_cast<uint16_t>(PixelFormat::Y8_PREVIEW);
            hdr.flags = FLAG_PREVIEW;
            hdr.stride = w;
            hdr.payload_size = w * h;
            work_buf.resize(hdr.payload_size);
            if (bd == 10) {
                extract_y10p_to_y8_strided(src, stride, work_buf.data(), w, w, h, 0);
            } else {
                extract_y12p_to_y8_strided(src, stride, work_buf.data(), w, w, h, 0);
            }
            std::lock_guard<std::mutex> flk(frame_mu_);
            latest_frame_.header = hdr;
            latest_frame_.data.swap(work_buf);
            latest_seq_ = seq;
        } else if (mode_id == 2 && h264_enc_.is_open()) {
            // Hardware H.264 encode via /dev/video11 VPU
            if (keyframe_req_.exchange(false)) {
                h264_enc_.force_keyframe();
            }
            bool is_kf = false;
            bool encoded = h264_enc_.encode_frame(
                [&](uint8_t* dst_y, uint32_t dst_stride, uint32_t ew, uint32_t eh) {
                    if (bd == 10) {
                        uint32_t x_off_quads = (w > ew) ? ((w - ew) / 2 / 4) : 0;
                        extract_y10p_to_y8_strided(src, stride, dst_y, dst_stride, ew, eh, x_off_quads);
                    } else {
                        uint32_t x_off_pairs = (w > ew) ? ((w - ew) / 2 / 2) : 0;
                        extract_y12p_to_y8_strided(src, stride, dst_y, dst_stride, ew, eh, x_off_pairs);
                    }
                },
                work_buf, is_kf);

            if (encoded) {
                FrameHeader hdr = raw_hdr;
                hdr.pixel_format = static_cast<uint16_t>(PixelFormat::H264);
                hdr.flags = FLAG_PREVIEW | (is_kf ? FLAG_KEYFRAME : 0u);
                hdr.width = h264_enc_.enc_width();
                hdr.height = h264_enc_.enc_height();
                hdr.stride = hdr.width;
                hdr.payload_size = static_cast<uint32_t>(work_buf.size());

                std::lock_guard<std::mutex> flk(frame_mu_);
                latest_frame_.header = hdr;
                latest_frame_.data.swap(work_buf);
                latest_seq_ = seq;
            }
        }

        // Requeue V4L2 buffer immediately
        xioctl(video_fd_, VIDIOC_QBUF, &buf);
        frame_cv_.notify_all();

        ++fps_frames;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - fps_t0).count();
        if (dt >= 0.5) {
            double mfps = fps_frames / dt;
            fps_frames = 0;
            fps_t0 = now;
            std::lock_guard<std::mutex> lk(ctrl_mu_);
            status_.measured_fps = mfps;
            status_.frame_seq = seq;
            status_.dropped_frames = dropped_frames_.load();
        }
        double tdt = std::chrono::duration<double>(now - telem_t0).count();
        if (tdt >= 0.5) {
            telem_t0 = now;
            std::lock_guard<std::mutex> lk(ctrl_mu_);
            refresh_hw_telemetry_locked();
        }
    }
}

CameraStatus Camera::get_status() {
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    refresh_hw_telemetry_locked();
    status_.dropped_frames = dropped_frames_.load();
    return status_;
}

bool Camera::set_exposure(const std::string& mode, uint32_t exposure_us, std::string& err) {
    (void)mode;
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        status_.exposure_mode = "manual";
        if (exposure_us > 0) {
            status_.target_exposure_us = std::clamp(exposure_us, 10u, 10000000u);
        }
    }
    return apply_adaptive_with_restart(err);
}

bool Camera::set_gain(const std::string& mode, double gain_db, std::string& err) {
    (void)mode;
    (void)err;
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    status_.gain_mode = "manual";
    status_.target_gain_db = std::clamp(gain_db, 0.0, 48.0);
    uint32_t gain_reg = static_cast<uint32_t>(std::lround(status_.target_gain_db * 10.0));
    i2c_.write_u32(reg::GAIN_MODE, 2);
    i2c_.write_u32(reg::MANUAL_GAIN, gain_reg);
    refresh_hw_telemetry_locked();
    return true;
}

bool Camera::set_fps(double target_fps, std::string& err) {
    if (target_fps <= 0.0 || target_fps > 240.0) {
        err = "target_fps out of range";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        status_.target_fps = target_fps;
    }
    return apply_adaptive_with_restart(err);
}

bool Camera::set_format(uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                        uint32_t width, uint32_t height, std::string& err) {
    std::lock_guard<std::mutex> rlk(reconfig_mu_);
    bool was_running = running_.load();
    if (was_running) {
        stop_capture();
    }
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        if (!configure_pipeline_locked(bit_depth, roi_x, roi_y, width, height, err)) {
            if (was_running) {
                std::string dummy;
                start_v4l2_locked(dummy);
            }
            return false;
        }
        apply_adaptive_exposure_fps_locked(nullptr);
        refresh_hw_telemetry_locked();
    }
    if (was_running) {
        return start_capture(err);
    }
    return true;
}

bool Camera::set_stream_mode(const std::string& mode, std::string& err) {
    if (mode != "lossless" && mode != "preview8" && mode != "h264") {
        err = "stream_mode must be 'h264', 'lossless', or 'preview8'";
        return false;
    }
    std::lock_guard<std::mutex> lk(ctrl_mu_);
    if (mode == "h264" && !h264_enc_.is_open()) {
        if (!h264_enc_.open_enc(status_.width, status_.height, 8000000, 30)) {
            err = "Hardware H.264 encoder (/dev/video11) unavailable";
            return false;
        }
    }
    status_.stream_mode = mode;
    if (mode == "lossless") stream_mode_id_.store(0);
    else if (mode == "preview8") stream_mode_id_.store(1);
    else {
        stream_mode_id_.store(2);
        keyframe_req_.store(true);
    }
    return true;
}

bool Camera::wait_next_frame(uint64_t last_seq, FrameBuffer& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(frame_mu_);
    bool ok = frame_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        return latest_seq_ > last_seq;
    });
    if (!ok || latest_seq_ <= last_seq) {
        return false;
    }
    if (last_seq > 0 && latest_seq_ > last_seq + 1) {
        dropped_frames_.fetch_add(latest_seq_ - last_seq - 1);
    }
    out.header = latest_frame_.header;
    out.data = latest_frame_.data;
    return true;
}

bool Camera::get_latest_raw_frame(FrameBuffer& out) {
    std::unique_lock<std::mutex> rlk(raw_mu_);
    uint64_t prev = snap_done_seq_;
    snap_req_.store(true);
    bool ok = raw_cv_.wait_for(rlk, std::chrono::milliseconds(1500), [&] {
        return snap_done_seq_ > prev || !running_.load();
    });
    if (!ok || latest_raw_frame_.data.empty()) return false;
    out.header = latest_raw_frame_.header;
    out.data = latest_raw_frame_.data;
    return true;
}


bool Camera::capture_long_shot(uint32_t exposure_us, double gain_db, FrameBuffer& out, std::string& err) {
    std::lock_guard<std::mutex> rlk(reconfig_mu_);

    exposure_us = std::clamp(exposure_us, 100u, 10000000u);
    gain_db = std::clamp(gain_db, 0.0, 48.0);

    bool was_running = running_.load();
    if (was_running) {
        stop_capture();
    }

    uint32_t saved_exp_us = 10000;
    double saved_gain_db = 0.0;
    double saved_fps = 30.0;
    uint32_t w = 1944, h = 1096, stride = 2944, rx = 0, ry = 0;
    uint16_t bd = 12;

    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        saved_exp_us = status_.target_exposure_us;
        saved_gain_db = status_.target_gain_db;
        saved_fps = status_.target_fps;
        w = status_.width;
        h = status_.height;
        stride = status_.stride;
        rx = status_.roi_x;
        ry = status_.roi_y;
        bd = status_.bit_depth;

        // Configure sensor for single long-exposure shot
        uint32_t max_fps_reg = 6000;
        i2c_.read_u32(reg::MAX_FRAME_RATE, max_fps_reg);
        double exp_fps = 980000.0 / static_cast<double>(exposure_us);
        uint32_t fps_reg = static_cast<uint32_t>(std::lround(exp_fps * 100.0));
        fps_reg = std::clamp<uint32_t>(fps_reg, 10, max_fps_reg);

        i2c_.write_u32(reg::EXPOSURE_MODE, 2);
        i2c_.write_u32(reg::FRAMERATE, fps_reg);
        ::usleep(10000);
        i2c_.write_u32(reg::ME_TIME, exposure_us);
        i2c_.write_u32(reg::GAIN_MODE, 2);
        i2c_.write_u32(reg::MANUAL_GAIN, static_cast<uint32_t>(std::lround(gain_db * 10.0)));
        ::usleep(10000);
        refresh_hw_telemetry_locked();

        if (!start_v4l2_locked(err)) {
            // Restore before returning
            status_.target_exposure_us = saved_exp_us;
            status_.target_gain_db = saved_gain_db;
            status_.target_fps = saved_fps;
            i2c_.write_u32(reg::MANUAL_GAIN, static_cast<uint32_t>(std::lround(saved_gain_db * 10.0)));
            apply_adaptive_exposure_fps_locked(nullptr);
            return false;
        }
    }

    // Wait for a properly integrated frame (discard an early flush frame if dt < 40% of exposure)
    bool got = false;
    auto t_start = std::chrono::steady_clock::now();
    int timeout_ms = static_cast<int>(exposure_us / 1000) + 4000;

    for (int attempt = 0; attempt < 3 && !got; ++attempt) {
        struct pollfd pfd{video_fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            err = "Timed out waiting for long-exposure frame";
            break;
        }
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(video_fd_, VIDIOC_DQBUF, &buf) < 0) {
            err = std::string("VIDIOC_DQBUF failed: ") + std::strerror(errno);
            break;
        }
        double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_start).count();

        // If this first frame arrived far too fast, it was a stale pre-exposure frame
        if (attempt == 0 && exposure_us >= 500000 && elapsed_ms < (exposure_us / 1000.0) * 0.4) {
            xioctl(video_fd_, VIDIOC_QBUF, &buf);
            t_start = std::chrono::steady_clock::now();
            continue;
        }

        const uint8_t* src = static_cast<const uint8_t*>(buffers_[buf.index].start);
        const uint32_t payload_size = std::min<uint32_t>(buf.bytesused, stride * h);
        const uint64_t seq = frame_seq_counter_.fetch_add(1) + 1;

        uint32_t hw_exp = exposure_us, hw_gain = static_cast<uint32_t>(std::lround(gain_db * 10.0)), hw_fps = 100;
        i2c_.read_u32(reg::EXP_TIME, hw_exp);
        i2c_.read_u32(reg::CUR_GAIN, hw_gain);
        i2c_.read_u32(reg::FRAMERATE, hw_fps);

        FrameHeader hdr{};
        std::memcpy(hdr.magic, FRAME_MAGIC, 4);
        hdr.version = PROTOCOL_VERSION;
        hdr.pixel_format = static_cast<uint16_t>(bd == 10 ? PixelFormat::Y10P : PixelFormat::Y12P);
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
        hdr.roi_x = static_cast<uint16_t>(rx);
        hdr.roi_y = static_cast<uint16_t>(ry);

        out.header = hdr;
        out.data.assign(src, src + payload_size);
        xioctl(video_fd_, VIDIOC_QBUF, &buf);
        got = true;
    }

    // Stop single-shot V4L2, restore live-stream settings, and resume live stream
    {
        std::lock_guard<std::mutex> lk(ctrl_mu_);
        stop_v4l2_locked();
        status_.target_exposure_us = saved_exp_us;
        status_.target_gain_db = saved_gain_db;
        status_.target_fps = saved_fps;
        i2c_.write_u32(reg::GAIN_MODE, 2);
        i2c_.write_u32(reg::MANUAL_GAIN, static_cast<uint32_t>(std::lround(saved_gain_db * 10.0)));
        apply_adaptive_exposure_fps_locked(nullptr);
        refresh_hw_telemetry_locked();
        keyframe_req_.store(true);
    }

    if (was_running) {
        std::string restart_err;
        start_capture(restart_err);
    }
    return got;
}

void Camera::request_keyframe() {
    keyframe_req_.store(true);
}

void Camera::note_dropped_frame() {
    dropped_frames_.fetch_add(1);
}

} // namespace rawcam
