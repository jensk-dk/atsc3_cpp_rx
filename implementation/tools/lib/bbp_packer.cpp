#include "atsc3/tools/bbp_packer.hpp"

#include <algorithm>

namespace atsc3::tools {

std::size_t kpayloadBytes64k(unsigned k) {
    // Kldpc = 64800 * k/15 ; Kpayload = Kldpc - 192 (BCH) ; bytes = /8
    return (64800u * k / 15u - 192u) / 8u;
}

std::vector<Bytes> packBbps(const std::vector<Bytes>& alps, const BbpPackerConfig& cfg, std::uint16_t& counter) {
    Bytes stream;
    std::vector<std::size_t> starts;
    for (const Bytes& a : alps) {
        starts.push_back(stream.size());
        stream.insert(stream.end(), a.begin(), a.end());
    }
    std::vector<Bytes> out;
    const std::size_t header_fixed = 4 + (cfg.counter ? 2u : 0u);
    if (cfg.kpayload_bytes <= header_fixed + 1) return out;
    const std::size_t capacity = cfg.kpayload_bytes - header_fixed;
    std::size_t pos = 0, next_start = 0;
    while (pos < stream.size()) {
        const std::size_t n = std::min(capacity, stream.size() - pos);
        while (next_start < starts.size() && starts[next_start] < pos) ++next_start;
        std::uint16_t pointer = 8191;
        if (next_start < starts.size() && starts[next_start] < pos + n) {
            pointer = static_cast<std::uint16_t>(starts[next_start] - pos);
        }
        const std::size_t pad = capacity - n;
        const std::size_t ext_len = pad + (cfg.counter ? 2u : 0u);
        Bytes b;
        b.push_back(static_cast<std::uint8_t>(0x80 | (pointer & 0x7F)));
        b.push_back(static_cast<std::uint8_t>((((pointer >> 7) & 0x3F) << 2) | 0x02));  // OFI = 10
        const std::uint8_t ext_type = cfg.counter ? 0x00 : 0x07;
        b.push_back(static_cast<std::uint8_t>((ext_type << 5) | (ext_len & 0x1F)));
        b.push_back(static_cast<std::uint8_t>((ext_len >> 5) & 0xFF));
        if (cfg.counter) {
            b.push_back(static_cast<std::uint8_t>(counter >> 8));
            b.push_back(static_cast<std::uint8_t>(counter & 0xFF));
            counter = static_cast<std::uint16_t>(counter + 1);
        }
        b.insert(b.end(), pad, 0);
        b.insert(b.end(), stream.begin() + static_cast<std::ptrdiff_t>(pos),
                 stream.begin() + static_cast<std::ptrdiff_t>(pos + n));
        out.push_back(std::move(b));
        pos += n;
    }
    return out;
}

}  // namespace atsc3::tools
