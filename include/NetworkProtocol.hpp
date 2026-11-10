#pragma once

#include <cstdint>
#include <cstring>
#include <chrono>
#include <tuple>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#pragma pack(push, 1)

namespace EyeCam::Network {

// ============================================================================
// 1. ENDIAN CONVERSION HELPERS
// ============================================================================

inline uint16_t swap16(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

inline uint32_t swap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8)  |
           ((v & 0x00FF0000U) >> 8)  |
           ((v & 0xFF000000U) >> 24);
}

inline uint64_t swap64(uint64_t v) {
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8)  |
           ((v & 0x000000FF00000000ULL) >> 8)  |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

inline bool isSystemLittleEndian() {
    uint16_t num = 1;
    return *reinterpret_cast<uint8_t*>(&num) == 1;
}

inline uint16_t hostToNet16(uint16_t v) { return isSystemLittleEndian() ? swap16(v) : v; }
inline uint32_t hostToNet32(uint32_t v) { return isSystemLittleEndian() ? swap32(v) : v; }
inline uint64_t hostToNet64(uint64_t v) { return isSystemLittleEndian() ? swap64(v) : v; }

inline uint16_t netToHost16(uint16_t v) { return isSystemLittleEndian() ? swap16(v) : v; }
inline uint32_t netToHost32(uint32_t v) { return isSystemLittleEndian() ? swap32(v) : v; }
inline uint64_t netToHost64(uint64_t v) { return isSystemLittleEndian() ? swap64(v) : v; }

// ============================================================================
// 2. MEDIA PLANE - 24-BYTE CANONICAL HEADER
// ============================================================================

// 'E' = 0x45, 'C' = 0x43 in Network Byte Order
constexpr uint16_t MEDIA_MAGIC_BE = 0x4543;
constexpr uint16_t MAX_PAYLOAD_CHUNK_SIZE = 1400; // MTU 1500 safety threshold

enum class MediaCodec : uint8_t {
    Unspecified = 0,
    H264 = 1,
    HEVC = 2, // H.265
    AV1 = 3
};

enum MediaFrameFlags : uint8_t {
    None = 0,
    Keyframe = 1 << 0,      // 0x01: IDR / SPS / PPS
    StartOfFrame = 1 << 1,  // 0x02: First NALU fragment
    EndOfFrame = 1 << 2     // 0x04: Last NALU fragment
};

/**
 * Binary Media Packet Header (24 bytes fixed size):
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Magic ('EC')        |     Flags     |   Codec Type  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         Sequence Number       |           Frame ID            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       Frame ID (cont.)        |         Fragment Index        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         Fragment Total        |                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
 * |                 Presentation Timestamp (PTS in us)            |
 * |                         (64-bit uint)                         |
 * +                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                               |         Payload Length        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct MediaPacketHeader {
    uint16_t magic;           // 0x4543 (ASCII "EC") in Network Byte Order
    uint8_t  flags;           // Bitmask of MediaFrameFlags
    uint8_t  codec;           // MediaCodec
    uint16_t sequence_num;    // Packet sequence counter
    uint32_t frame_id;        // Frame counter
    uint16_t fragment_index;  // Fragment index (0 .. fragment_total - 1)
    uint16_t fragment_total;  // Total fragments in this frame
    uint64_t pts_us;          // Presentation Timestamp in microseconds
    uint16_t payload_len;     // Video payload length following this header

    bool isValid() const {
        return magic == hostToNet16(MEDIA_MAGIC_BE) || magic == MEDIA_MAGIC_BE;
    }

    bool isKeyframe() const {
        return (flags & MediaFrameFlags::Keyframe) != 0;
    }

    bool isStartOfFrame() const {
        return (flags & MediaFrameFlags::StartOfFrame) != 0;
    }

    bool isEndOfFrame() const {
        return (flags & MediaFrameFlags::EndOfFrame) != 0;
    }

    void deserializeFromNet() {
        sequence_num = netToHost16(sequence_num);
        frame_id = netToHost32(frame_id);
        fragment_index = netToHost16(fragment_index);
        fragment_total = netToHost16(fragment_total);
        pts_us = netToHost64(pts_us);
        payload_len = netToHost16(payload_len);
    }

    void serializeToNet() {
        magic = hostToNet16(MEDIA_MAGIC_BE);
        sequence_num = hostToNet16(sequence_num);
        frame_id = hostToNet32(frame_id);
        fragment_index = hostToNet16(fragment_index);
        fragment_total = hostToNet16(fragment_total);
        pts_us = hostToNet64(pts_us);
        payload_len = hostToNet16(payload_len);
    }
};

static_assert(sizeof(MediaPacketHeader) == 24, "MediaPacketHeader must be exactly 24 bytes!");

// ============================================================================
// 3. CONTROL PLANE - 8-BYTE CANONICAL HEADER
// ============================================================================

// 'C' = 0x43, 'P' = 0x50 in Network Byte Order
constexpr uint16_t CONTROL_MAGIC_BE = 0x4350;

enum class ControlMessageType : uint16_t {
    JsonCommand = 1,        // Text JSON command
    ProtobufCommand = 2,    // Binary Protobuf message
    FramePing = 3,          // Latency measurement ping (T1)
    FramePong = 4,          // Latency measurement pong (T1, T2, T3)
    KeyframeRequest = 5,    // Urgent IDR request (PLI)
    KeepAlive = 6           // Heartbeat
};

/**
 * Binary Control Frame Header (8 bytes):
 * +---------------------------------------------------------------+
 * | Magic (2B) | MessageType (2B) | PayloadLength (4B)            |
 * +---------------------------------------------------------------+
 */
struct ControlFrameHeader {
    uint16_t magic;         // 0x4350 (ASCII "CP") in Network Byte Order
    uint16_t msg_type;      // ControlMessageType
    uint32_t payload_len;   // Byte length of payload

    bool isValid() const {
        return magic == hostToNet16(CONTROL_MAGIC_BE) || magic == CONTROL_MAGIC_BE;
    }

    void deserializeFromNet() {
        msg_type = netToHost16(msg_type);
        payload_len = netToHost32(payload_len);
    }

    void serializeToNet() {
        magic = hostToNet16(CONTROL_MAGIC_BE);
        msg_type = hostToNet16(msg_type);
        payload_len = hostToNet32(payload_len);
    }
};

static_assert(sizeof(ControlFrameHeader) == 8, "ControlFrameHeader must be exactly 8 bytes!");

#pragma pack(pop)

// ============================================================================
// 4. TIMING & NTP UTILITIES
// ============================================================================

inline uint64_t getMonotonicTimeUs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

inline std::tuple<double, double> calculateNtpMetrics(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    int64_t client_rtt = static_cast<int64_t>(t4 - t1);
    int64_t server_proc = static_cast<int64_t>(t3 - t2);
    int64_t rtt = client_rtt - server_proc;

    int64_t delta1 = static_cast<int64_t>(t2 - t1);
    int64_t delta2 = static_cast<int64_t>(t3 - t4);
    double offset = static_cast<double>(delta1 + delta2) / 2.0;

    return { static_cast<double>(rtt) / 1000.0, offset / 1000.0 };
}

} // namespace EyeCam::Network
