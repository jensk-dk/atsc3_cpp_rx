#pragma once
// Packet builders shared by converters and tests.
#include <cstdint>
#include <vector>

namespace atsc3::tools {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

Bytes ipv4Udp(std::uint32_t src, std::uint32_t dst, std::uint16_t sport, std::uint16_t dport, const Bytes& payload,
              std::uint16_t ip_id = 0);
Bytes ethernet(const Bytes& ipv4, bool vlan = false);
Bytes linuxSll(const Bytes& ipv4);
Bytes rtp(std::uint8_t pt, bool marker, std::uint16_t seq, std::uint32_t timestamp, std::uint32_t ssrc,
          const Bytes& payload);

}  // namespace atsc3::tools
