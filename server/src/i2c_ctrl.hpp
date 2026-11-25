#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rawcam {

// VEYE VEYE-RAWCAM register addresses (from veye_mvcam.h)
namespace reg {
    constexpr uint16_t MODEL_NAME      = 0x0004;
    constexpr uint16_t FORMAT_CAP      = 0x0028;
    constexpr uint16_t PIXEL_FORMAT    = 0x0804;
    constexpr uint16_t SENSOR_WIDTH    = 0x0808;
    constexpr uint16_t SENSOR_HEIGHT   = 0x080C;
    constexpr uint16_t MAX_FRAME_RATE  = 0x0810; // 0.01 fps
    constexpr uint16_t FRAMERATE       = 0x0814; // 0.01 fps
    constexpr uint16_t ROI_WIDTH       = 0x0818;
    constexpr uint16_t ROI_HEIGHT      = 0x081C;
    constexpr uint16_t ROI_OFFSET_X    = 0x0820;
    constexpr uint16_t ROI_OFFSET_Y    = 0x0824;
    constexpr uint16_t MIN_ROI_WIDTH   = 0x0844;
    constexpr uint16_t MIN_ROI_HEIGHT  = 0x0848;

    constexpr uint16_t EXPOSURE_MODE   = 0x0C04; // 0=Auto, 1=Once, 2=Manual
    constexpr uint16_t TARGET_BRIGHT   = 0x0C08;
    constexpr uint16_t ME_TIME         = 0x0C10; // Manual exposure us
    constexpr uint16_t AE_MAX_TIME     = 0x0C14; // Auto exposure max us
    constexpr uint16_t EXP_TIME        = 0x0C18; // Current actual exposure us
    constexpr uint16_t GAIN_MODE       = 0x0C1C; // 0=Auto, 1=Once, 2=Manual
    constexpr uint16_t MANUAL_GAIN     = 0x0C20; // Manual gain 0.1 dB
    constexpr uint16_t AG_MAX_GAIN     = 0x0C24; // Auto gain max 0.1 dB
    constexpr uint16_t CUR_GAIN        = 0x0C28; // Current actual gain 0.1 dB
} // namespace reg

class I2cCtrl {
public:
    I2cCtrl() = default;
    ~I2cCtrl() { close_dev(); }

    bool open_dev(const std::string& dev_path, uint16_t addr = 0x3b) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = ::open(dev_path.c_str(), O_RDWR);
        addr_ = addr;
        return fd_ >= 0;
    }

    void close_dev() {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool read_u32(uint16_t reg_addr, uint32_t& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ < 0) return false;
        uint8_t wbuf[2] = {static_cast<uint8_t>(reg_addr >> 8), static_cast<uint8_t>(reg_addr & 0xFF)};
        uint8_t rbuf[4] = {};
        struct i2c_msg msgs[2]{};
        msgs[0].addr  = addr_;
        msgs[0].flags = 0;
        msgs[0].len   = 2;
        msgs[0].buf   = wbuf;
        msgs[1].addr  = addr_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len   = 4;
        msgs[1].buf   = rbuf;

        struct i2c_rdwr_ioctl_data data{};
        data.msgs  = msgs;
        data.nmsgs = 2;
        if (::ioctl(fd_, I2C_RDWR, &data) != 2) {
            return false;
        }
        uint32_t net_val = 0;
        std::memcpy(&net_val, rbuf, 4);
        out = ntohl(net_val);
        return true;
    }

    bool write_u32(uint16_t reg_addr, uint32_t val) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ < 0) return false;
        uint8_t buf[6];
        buf[0] = static_cast<uint8_t>(reg_addr >> 8);
        buf[1] = static_cast<uint8_t>(reg_addr & 0xFF);
        uint32_t net_val = htonl(val);
        std::memcpy(buf + 2, &net_val, 4);

        struct i2c_msg msg{};
        msg.addr  = addr_;
        msg.flags = 0;
        msg.len   = 6;
        msg.buf   = buf;

        struct i2c_rdwr_ioctl_data data{};
        data.msgs  = &msg;
        data.nmsgs = 1;
        return ::ioctl(fd_, I2C_RDWR, &data) == 1;
    }

private:
    int fd_ = -1;
    uint16_t addr_ = 0x3b;
    std::mutex mu_;
};

} // namespace rawcam
