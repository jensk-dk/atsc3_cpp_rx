#pragma once
// BBP → STLTP packer (A/324 §9.3.2.1, §6.3): inner BBP RTP streams and outer CTP tunnel packets.
#include <cstdint>
#include <vector>

#include "atsc3/tools/packets.hpp"

namespace atsc3::tools {

struct StltpPackerConfig {
    std::uint32_t inner_src = ip4(192, 168, 1, 10);
    std::uint32_t inner_dst = ip4(239, 0, 51, 48);
    std::uint16_t inner_sport = 40000;
    std::size_t inner_fragment = 1316;   // BBP bytes per inner RTP packet
    std::size_t tunnel_payload = 1360;   // outer tunnel payload size (fixed, ST 2022-1 style)
    std::uint8_t protocol_version = 0;   // as seen on air; the 2024 standard says 1
};

/// Inner RTP/UDP/IP packets for one BBP: M=1 and SSRC=BBP length on the first packet (PT 78, port 30000+plp).
std::vector<Bytes> bppsPackets(std::uint8_t plp, const Bytes& bbp, std::uint16_t& inner_seq, std::uint32_t rtp_ts,
                               const StltpPackerConfig& cfg = {});

/// Packs inner IP packets into outer STLTP tunnel payloads (RTP PT 97 with packet_offset/marker).
/// The result is the UDP payload of each outer packet; wrap with ipv4Udp/ethernet for a capture.
std::vector<Bytes> stltpTunnel(const std::vector<Bytes>& inner, std::uint16_t& outer_seq,
                               const StltpPackerConfig& cfg = {});

}  // namespace atsc3::tools
