#pragma once
#include "h264_enc.hpp"
#include "i2c_ctrl.hpp"
#include "protocol.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rawcam {

struct CameraStatus {
    std::string camera_model;
    std::string video_dev;
    std::string subdev_dev;
    std::string i2c_dev;

    uint32_t sensor_width = 1944;
    uint32_t sensor_height = 1096;
    uint32_t min_roi_width = 368;
    uint32_t min_roi_height = 304;

    uint16_t bit_depth = 12;        // 10 or 12
    uint32_t roi_x = 0;
    uint32_t roi_y = 0;
    uint32_t width = 1944;
    uint32_t height = 1096;
    uint32_t stride = 0;
    std::string stream_mode = "h264"; // "h264", "lossless", or "preview8"

    std::string exposure_mode = "manual";
    uint32_t target_exposure_us = 10000;
    uint32_t actual_exposure_us = 10000;

    std::string gain_mode = "manual";
    double target_gain_db = 0.0;
    double actual_gain_db = 0.0;

    double target_fps = 30.0;
    double effective_fps = 30.0;
    double sensor_max_fps = 60.0;
    double measured_fps = 0.0;
    bool fps_clamped_by_exposure = false;

    uint64_t frame_seq = 0;
    uint64_t dropped_frames = 0;
    bool streaming = false;
};

struct FrameBuffer {
    FrameHeader header{};
    std::vector<uint8_t> data;
};

class Camera {
public:
    Camera() = default;
    ~Camera();

    bool init(std::string& err);
    void shutdown();

    bool start_capture(std::string& err);
    void stop_capture();

    CameraStatus get_status();
    bool set_exposure(const std::string& mode, uint32_t exposure_us, std::string& err);
    bool set_gain(const std::string& mode, double gain_db, std::string& err);
    bool set_fps(double target_fps, std::string& err);
    bool set_format(uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                    uint32_t width, uint32_t height, std::string& err);
    bool set_stream_mode(const std::string& mode, std::string& err);

    bool wait_next_frame(uint64_t last_seq, FrameBuffer& out, int timeout_ms);
    bool get_latest_raw_frame(FrameBuffer& out);
    bool capture_long_shot(uint32_t exposure_us, double gain_db, FrameBuffer& out, std::string& err);
    void request_keyframe();
    void note_dropped_frame();

private:
    struct MmapBuf {
        void* start = nullptr;
        size_t length = 0;
    };

    bool discover_devices(std::string& err);
    bool configure_pipeline_locked(uint16_t bit_depth, uint32_t roi_x, uint32_t roi_y,
                                   uint32_t width, uint32_t height, std::string& err);
    bool start_v4l2_locked(std::string& err);
    void stop_v4l2_locked();
    void apply_adaptive_exposure_fps_locked(bool* need_restart_for_fps = nullptr);
    bool apply_adaptive_with_restart(std::string& err);
    void refresh_hw_telemetry_locked();
    void capture_loop();

    int video_fd_ = -1;
    int subdev_fd_ = -1;
    I2cCtrl i2c_;
    H264HwEncoder h264_enc_;

    std::vector<MmapBuf> buffers_;

    std::mutex reconfig_mu_;
    std::mutex ctrl_mu_;
    CameraStatus status_;
    std::atomic<bool> running_{false};
    std::thread cap_thread_;

    // Latest stream frame (H264 / lossless / preview8)
    std::mutex frame_mu_;
    std::condition_variable frame_cv_;
    FrameBuffer latest_frame_;
    uint64_t latest_seq_ = 0;
    std::atomic<uint64_t> frame_seq_counter_{0};
    std::atomic<uint64_t> dropped_frames_{0};

    // Always-available latest lossless 10/12-bit RAW frame for 12-bit snapshot
    std::mutex raw_mu_;
    std::condition_variable raw_cv_;
    std::atomic<bool> snap_req_{false};
    uint64_t snap_done_seq_{0};
    FrameBuffer latest_raw_frame_;

    std::atomic<uint32_t> cached_exp_us_{10000};
    std::atomic<uint32_t> cached_gain_x10_{0};
    std::atomic<uint32_t> cached_fps_x100_{3000};
    std::atomic<int> stream_mode_id_{2}; // 0=lossless, 1=preview8, 2=h264
    std::atomic<bool> keyframe_req_{true};
};

} // namespace rawcam
