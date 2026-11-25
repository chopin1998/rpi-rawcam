#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace rawcam {

// Hardware H.264 encoder using RPi 4B V4L2 M2M (/dev/video11, bcm2835-codec-encode).
// Accepts 8-bit monochrome Y data directly into YU12 (with pre-filled U=V=0x80).
class H264HwEncoder {
public:
    H264HwEncoder() = default;
    ~H264HwEncoder() { close_enc(); }

    bool open_enc(uint32_t src_w, uint32_t src_h, uint32_t bitrate = 8000000,
                  uint32_t gop = 30, const std::string& dev = "/dev/video11") {
        close_enc();

        // bcm2835-codec max width/height is 1920, step 2
        enc_w_ = (std::min<uint32_t>(src_w, 1920) / 2) * 2;
        enc_h_ = (std::min<uint32_t>(src_h, 1920) / 2) * 2;
        if (enc_w_ < 32 || enc_h_ < 32) return false;

        fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return false;

        // Set codec controls before streaming
        set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
        set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(bitrate));
        set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, static_cast<int32_t>(gop));
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, static_cast<int32_t>(gop));
        set_ctrl(V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1);
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2);
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 16);
        set_ctrl(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 42);

        // Configure OUTPUT (YU12 raw input) FIRST
        struct v4l2_format ofmt{};
        ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ofmt.fmt.pix_mp.width = enc_w_;
        ofmt.fmt.pix_mp.height = enc_h_;
        ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
        ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        ofmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(VIDIOC_S_FMT, &ofmt) < 0) {
            std::cerr << "[h264] OUTPUT S_FMT failed: " << std::strerror(errno) << std::endl;
            close_enc(); return false;
        }

        // Configure CAPTURE (H264 output)
        struct v4l2_format cfmt{};
        cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cfmt.fmt.pix_mp.width = enc_w_;
        cfmt.fmt.pix_mp.height = enc_h_;
        cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
        cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        cfmt.fmt.pix_mp.num_planes = 1;
        cfmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
        if (xioctl(VIDIOC_S_FMT, &cfmt) < 0) {
            std::cerr << "[h264] CAPTURE S_FMT failed: " << std::strerror(errno) << std::endl;
            close_enc(); return false;
        }

        y_stride_ = ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        y_size_   = ofmt.fmt.pix_mp.plane_fmt[0].sizeimage;

        // Set OUTPUT selection crop to exact enc_w x enc_h
        struct v4l2_selection sel{};
        sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        sel.target = V4L2_SEL_TGT_CROP;
        sel.r = {0, 0, enc_w_, enc_h_};
        xioctl(VIDIOC_S_SELECTION, &sel);

        if (!alloc_buffers(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 2, out_bufs_, true)) {
            close_enc();
            return false;
        }
        if (!alloc_buffers(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 4, cap_bufs_, false)) {
            close_enc();
            return false;
        }

        // Queue all CAPTURE buffers
        for (uint32_t i = 0; i < cap_bufs_.size(); ++i) {
            if (!qbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 0)) {
                close_enc();
                return false;
            }
        }

        v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(VIDIOC_STREAMON, &t_out) < 0 || xioctl(VIDIOC_STREAMON, &t_cap) < 0) {
            std::cerr << "[h264] STREAMON failed: " << std::strerror(errno) << std::endl;
            close_enc();
            return false;
        }

        streaming_ = true;
        return true;
    }

    void close_enc() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type t_out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            v4l2_buf_type t_cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(VIDIOC_STREAMOFF, &t_out);
            xioctl(VIDIOC_STREAMOFF, &t_cap);
            streaming_ = false;
        }
        unmap_all(out_bufs_);
        unmap_all(cap_bufs_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        enc_w_ = enc_h_ = y_stride_ = y_size_ = 0;
    }

    bool is_open() const { return fd_ >= 0 && streaming_; }
    uint32_t enc_width() const { return enc_w_; }
    uint32_t enc_height() const { return enc_h_; }
    uint32_t y_stride() const { return y_stride_; }

    void force_keyframe() {
        if (fd_ >= 0) {
            set_ctrl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
        }
    }

    // Fill OUTPUT buffer's Y plane via callback, then encode synchronously (typically 2-5 ms on RPi4 VPU)
    template <typename FillYFn>
    bool encode_frame(FillYFn&& fill_y, std::vector<uint8_t>& out_nal, bool& is_keyframe) {
        if (!is_open() || out_bufs_.empty()) return false;

        // Fill Y plane directly in MMAP buffer 0 (U/V already 0x80)
        uint8_t* dst_y = static_cast<uint8_t*>(out_bufs_[0].start);
        fill_y(dst_y, y_stride_, enc_w_, enc_h_);

        if (!qbuf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, y_size_)) {
            return false;
        }

        // Wait for both OUTPUT completion and CAPTURE encoded packet
        bool got_out = false;
        bool got_cap = false;
        is_keyframe = false;
        out_nal.clear();

        for (int tries = 0; tries < 20 && (!got_out || !got_cap); ++tries) {
            struct pollfd pfd{fd_, POLLIN | POLLOUT, 0};
            int pr = ::poll(&pfd, 1, 100);
            if (pr <= 0) break;

            if (!got_out && (pfd.revents & POLLOUT)) {
                struct v4l2_buffer b{};
                struct v4l2_plane pl{};
                b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                b.memory = V4L2_MEMORY_MMAP;
                b.length = 1;
                b.m.planes = &pl;
                if (xioctl(VIDIOC_DQBUF, &b) == 0) {
                    got_out = true;
                }
            }
            if (!got_cap && (pfd.revents & POLLIN)) {
                struct v4l2_buffer b{};
                struct v4l2_plane pl{};
                b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                b.memory = V4L2_MEMORY_MMAP;
                b.length = 1;
                b.m.planes = &pl;
                if (xioctl(VIDIOC_DQBUF, &b) == 0) {
                    uint32_t len = pl.bytesused;
                    if (len > 0 && b.index < cap_bufs_.size()) {
                        const uint8_t* p = static_cast<const uint8_t*>(cap_bufs_[b.index].start);
                        out_nal.assign(p, p + len);
                        is_keyframe = (b.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
                        got_cap = true;
                    }
                    qbuf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index, 0);
                }
            }
        }
        return got_cap && !out_nal.empty();
    }

private:
    struct Buf {
        void* start = nullptr;
        size_t length = 0;
    };

    int xioctl(unsigned long req, void* arg) {
        int r;
        do {
            r = ::ioctl(fd_, req, arg);
        } while (r == -1 && errno == EINTR);
        return r;
    }

    void set_ctrl(uint32_t id, int32_t val) {
        struct v4l2_control c{};
        c.id = id;
        c.value = val;
        xioctl(VIDIOC_S_CTRL, &c);
    }

    bool alloc_buffers(v4l2_buf_type type, uint32_t count, std::vector<Buf>& vec, bool fill_neutral_uv) {
        struct v4l2_requestbuffers req{};
        req.count = count;
        req.type = type;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(VIDIOC_REQBUFS, &req) < 0 || req.count == 0) return false;

        vec.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer b{};
            struct v4l2_plane pl{};
            b.type = type;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            b.length = 1;
            b.m.planes = &pl;
            if (xioctl(VIDIOC_QUERYBUF, &b) < 0) return false;
            void* ptr = ::mmap(nullptr, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd_, pl.m.mem_offset);
            if (ptr == MAP_FAILED) return false;
            vec[i] = {ptr, pl.length};
            if (fill_neutral_uv) {
                // Fill entire buffer with 0x80 so U & V planes are neutral gray (monochrome!)
                std::memset(ptr, 0x80, pl.length);
            }
        }
        return true;
    }

    bool qbuf(v4l2_buf_type type, uint32_t idx, uint32_t bytesused) {
        struct v4l2_buffer b{};
        struct v4l2_plane pl{};
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = idx;
        b.length = 1;
        b.m.planes = &pl;
        if (bytesused > 0) pl.bytesused = bytesused;
        return xioctl(VIDIOC_QBUF, &b) == 0;
    }

    static void unmap_all(std::vector<Buf>& vec) {
        for (auto& b : vec) {
            if (b.start && b.start != MAP_FAILED) {
                ::munmap(b.start, b.length);
            }
        }
        vec.clear();
    }

    int fd_ = -1;
    bool streaming_ = false;
    uint32_t enc_w_ = 0;
    uint32_t enc_h_ = 0;
    uint32_t y_stride_ = 0;
    uint32_t y_size_ = 0;
    std::vector<Buf> out_bufs_;
    std::vector<Buf> cap_bufs_;
};

} // namespace rawcam
