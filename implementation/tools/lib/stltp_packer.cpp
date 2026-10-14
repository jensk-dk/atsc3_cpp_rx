#include "atsc3/tools/stltp_packer.hpp"

#include <algorithm>

namespace atsc3::tools {

std::vector<Bytes> bppsPackets(std::uint8_t plp, const Bytes& bbp, std::uint16_t& seq, std::uint32_t rtp_ts,
                               const StltpPackerConfig& cfg) {
    std::vector<Bytes> out;
    const std::uint16_t port = static_cast<std::uint16_t>(30000 + plp);
    for (std::size_t off = 0; off < bbp.size(); off += cfg.inner_fragment) {
        const std::size_t n = std::min(cfg.inner_fragment, bbp.size() - off);
        const Bytes chunk(bbp.begin() + static_cast<std::ptrdiff_t>(off), bbp.begin() + static_cast<std::ptrdiff_t>(off + n));
        const bool first = off == 0;
        out.push_back(ipv4Udp(cfg.inner_src, cfg.inner_dst, cfg.inner_sport, port,
                              rtp(78, first, seq++, rtp_ts, first ? static_cast<std::uint32_t>(bbp.size()) : 0, chunk)));
    }
    return out;
}

std::vector<Bytes> stltpTunnel(const std::vector<Bytes>& inner, std::uint16_t& seq, const StltpPackerConfig& cfg) {
    Bytes stream;
    std::vector<std::size_t> starts;
    for (const Bytes& p : inner) {
        starts.push_back(stream.size());
        stream.insert(stream.end(), p.begin(), p.end());
    }
    std::vector<Bytes> out;
    std::size_t next_start = 0;
    for (std::size_t off = 0; off < stream.size(); off += cfg.tunnel_payload) {
        const std::size_t n = std::min(cfg.tunnel_payload, stream.size() - off);
        bool marker = false;
        std::uint16_t packet_offset = 0;
        while (next_start < starts.size() && starts[next_start] < off) ++next_start;
        if (next_start < starts.size() && starts[next_start] < off + n) {
            marker = true;
            packet_offset = static_cast<std::uint16_t>(starts[next_start] - off);
        }
        const std::uint32_t ctp = (static_cast<std::uint32_t>(cfg.protocol_version) << 30) | packet_offset;
        Bytes chunk(stream.begin() + static_cast<std::ptrdiff_t>(off), stream.begin() + static_cast<std::ptrdiff_t>(off + n));
        out.push_back(rtp(97, marker, seq++, 0x1000, ctp, chunk));
    }
    return out;
}

}  // namespace atsc3::tools
