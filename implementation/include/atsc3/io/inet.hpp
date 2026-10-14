#pragma once
// Small IPv4/UDP/RTP header helpers shared by the io, stltp and ip layers.
#include <cstdint>
#include <string>
#include <string_view>

#include "atsc3/byte_view.hpp"

namespace atsc3::io {

std::string ipv4ToString(std::uint32_t host_order);
bool parseIpv4(std::string_view text, std::uint32_t& host_order);
constexpr bool isMulticast(std::uint32_t host_order) noexcept { return (host_order >> 28) == 0xE; }

struct Ipv4Udp {
    std::uint32_t src = 0, dst = 0;  // host order
    std::uint8_t protocol = 0;
    std::uint16_t total_len = 0;
    bool fragment = false;           // MF set or offset != 0
    bool udp = false;
    std::uint16_t sport = 0, dport = 0;
    ByteView payload;                // UDP payload when udp == true
};

/// Parses an IPv4 header (and the UDP header if present). Returns false if not a sane IPv4 packet.
bool parseIpv4Udp(ByteView ip, Ipv4Udp& out) noexcept;

struct Rtp {
    std::uint8_t version = 0, cc = 0, pt = 0;
    bool padding = false, extension = false, marker = false;
    std::uint16_t seq = 0;
    std::uint32_t timestamp = 0, ssrc = 0;
    ByteView ext;      // extension data (after the 4-byte extension header) when present
    ByteView payload;  // after header, CSRCs, extension and minus padding
};

/// Parses an RTP fixed header (RFC 3550). Returns false if shorter than 12 bytes.
bool parseRtp(ByteView d, Rtp& out) noexcept;

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace atsc3::io
