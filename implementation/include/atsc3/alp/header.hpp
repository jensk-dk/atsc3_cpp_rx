#pragma once
// ALP packet header (A/330 §5.1–§5.4; spec 5.4.1).
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "atsc3/byte_view.hpp"

namespace atsc3::alp {

enum class PacketType : std::uint8_t {
    Ipv4 = 0,
    Reserved1 = 1,
    CompressedIp = 2,
    Reserved3 = 3,
    Signaling = 4,
    Reserved5 = 5,
    TypeExtension = 6,
    MpegTs = 7
};
const char* toString(PacketType t) noexcept;

enum class Layout : std::uint8_t { Single, Segment, Concatenation, MpegTs };

struct SignalingInfo {
    std::uint8_t type = 0;            // 0x01 LMT, 0x02 RDT, 0xF0–0xFF user private
    std::uint16_t type_extension = 0;
    std::uint8_t version = 0;
    std::uint8_t format = 0;          // 00 binary, 01 XML, 10 JSON
    std::uint8_t encoding = 0;        // 00 none, 01 DEFLATE
};

struct HeaderExtension {
    std::uint8_t type = 0;            // 0xF0–0xFF user private, else reserved
    ByteView bytes;
};

struct Header {
    PacketType type = PacketType::Ipv4;
    Layout layout = Layout::Single;
    bool header_mode = false;                 // HM (single packets only)
    std::size_t header_length = 0;            // bytes before the payload
    std::size_t payload_length = 0;           // bytes after the header (all components)
    // segmentation
    std::uint8_t segment_sn = 0;
    bool last_segment = false;
    // concatenation: count + 2 components; the last length is implied
    std::uint8_t component_count = 0;
    std::array<std::uint16_t, 10> component_lengths{};
    // signaling / type extension
    SignalingInfo signaling;
    std::uint16_t extended_type = 0;
    // optional extension header
    std::optional<std::uint8_t> sid;
    std::optional<HeaderExtension> extension;
    // MPEG-2 TS encapsulation
    std::uint8_t num_ts = 0;                  // 1..16
    bool ahf = false, hdm = false;
    std::uint8_t dnp = 0;

    std::size_t totalLength() const noexcept { return header_length + payload_length; }
};

enum class ParseStatus { Ok, NeedMore, Invalid };

/// Parses the header at the start of `data`. NeedMore when `data` is shorter than the header
/// (the payload may still be incomplete after Ok: compare data.size() with totalLength()).
ParseStatus parseHeader(ByteView data, Header& h) noexcept;

/// Minimum number of bytes needed to decide the full header length (2 for IP, 1 for TS).
constexpr std::size_t kMinHeaderBytes = 1;

}  // namespace atsc3::alp
