#pragma once
#include <cstdint>

namespace rawcam {

constexpr char FRAME_MAGIC[4] = {'R', 'A', 'W', 'C'};
constexpr uint16_t PROTOCOL_VERSION = 1;

enum class PixelFormat : uint16_t {
    Y10P = 1,       // 10-bit MIPI packed (5 bytes per 4 pixels), lossless
    Y12P = 2,       // 12-bit MIPI packed (3 bytes per 2 pixels), lossless
    Y10  = 3,       // 10-bit unpacked in uint16 LE, lossless
    Y12  = 4,       // 12-bit unpacked in uint16 LE, lossless
    Y8_PREVIEW = 5, // 8-bit top-bit preview (lossy display mode)
    H264 = 6,       // Hardware H.264 Annex-B access unit (lossy display mode)
};

constexpr uint32_t FLAG_LOSSLESS = 1u << 0;
constexpr uint32_t FLAG_PREVIEW  = 1u << 1;
constexpr uint32_t FLAG_KEYFRAME = 1u << 2;

#pragma pack(push, 1)
struct FrameHeader {
    char     magic[4];
    uint16_t version;
    uint16_t pixel_format;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t payload_size;
    uint32_t exposure_us;
    uint64_t frame_seq;
    uint64_t timestamp_ns;
    uint32_t gain_db_x10;
    uint32_t fps_x100;
    uint16_t bit_depth;
    uint16_t roi_x;
    uint16_t roi_y;
    uint16_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 64, "FrameHeader must be 64 bytes");

} // namespace rawcam
