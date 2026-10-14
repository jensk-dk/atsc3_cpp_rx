#include "atsc3/tools/packets.hpp"

namespace atsc3::tools {

namespace {
void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32(Bytes& b, std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v >> 16));
    put16(b, static_cast<std::uint16_t>(v & 0xFFFF));
}
std::uint16_t checksum(const Bytes& b, std::size_t off, std::size_t len) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < len; i += 2) sum += static_cast<std::uint32_t>((b[off + i] << 8) | b[off + i + 1]);
    if (len & 1) sum += static_cast<std::uint32_t>(b[off + len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum & 0xFFFF);
}
}  // namespace

Bytes ipv4Udp(std::uint32_t src, std::uint32_t dst, std::uint16_t sport, std::uint16_t dport, const Bytes& payload,
              std::uint16_t ip_id) {
    Bytes b;
    const std::uint16_t total = static_cast<std::uint16_t>(28 + payload.size());
    b.push_back(0x45);
    b.push_back(0);
    put16(b, total);
    put16(b, ip_id);
    put16(b, 0x4000);
    b.push_back(32);
    b.push_back(17);
    put16(b, 0);
    put32(b, src);
    put32(b, dst);
    const std::uint16_t cs = checksum(b, 0, 20);
    b[10] = static_cast<std::uint8_t>(cs >> 8);
    b[11] = static_cast<std::uint8_t>(cs & 0xFF);
    put16(b, sport);
    put16(b, dport);
    put16(b, static_cast<std::uint16_t>(8 + payload.size()));
    put16(b, 0);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes ethernet(const Bytes& ipv4, bool vlan) {
    Bytes b(12, 0x02);
    if (vlan) {
        put16(b, 0x8100);
        put16(b, 0x0042);
    }
    put16(b, 0x0800);
    b.insert(b.end(), ipv4.begin(), ipv4.end());
    return b;
}

Bytes linuxSll(const Bytes& ipv4) {
    Bytes b(14, 0);
    put16(b, 0x0800);
    b.insert(b.end(), ipv4.begin(), ipv4.end());
    return b;
}

Bytes rtp(std::uint8_t pt, bool marker, std::uint16_t seq, std::uint32_t timestamp, std::uint32_t ssrc,
          const Bytes& payload) {
    Bytes b;
    b.push_back(0x80);
    b.push_back(static_cast<std::uint8_t>((marker ? 0x80 : 0) | (pt & 0x7F)));
    put16(b, seq);
    put32(b, timestamp);
    put32(b, ssrc);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

}  // namespace atsc3::tools
