#pragma once
// STLTP demultiplexer (A/324 §6.3, §8.2, §9.3.2.1, §9.4; spec 5.2): outer CTP tunnel packets in,
// Baseband Packets (per PLP), Preamble, T&M and ALPTP-tunneled ALP packets out.
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::stltp {

constexpr std::uint8_t kPtTmp = 76;       // Timing & Management
constexpr std::uint8_t kPtPreamble = 77;
constexpr std::uint8_t kPtBbp = 78;
constexpr std::uint8_t kPtSecurityKey = 79;
constexpr std::uint8_t kPtSecurity = 80;
constexpr std::uint8_t kPtDstp = 81;
constexpr std::uint8_t kPtAlptp = 82;
constexpr std::uint8_t kPtStltp = 97;
constexpr std::uint16_t kPortBbpBase = 30000;  // + plp (0..63), +100 per extra channel
constexpr std::uint16_t kPortPreamble = 30064;
constexpr std::uint16_t kPortTmp = 30065;
constexpr std::uint16_t kPortSecurity = 30066;

struct Stats {
    std::uint64_t outer_packets = 0, outer_not_rtp = 0, outer_other_pt = 0, outer_seq_gaps = 0;
    std::uint64_t outer_resync = 0, inner_packets = 0, inner_not_udp = 0, inner_no_rtp = 0;
    std::uint64_t inner_bad_len = 0, bpps_no_start = 0, bpps_seq_gap = 0, bpps_incomplete = 0;
    std::uint64_t bpps_overrun = 0, bbp = 0, preamble = 0, tmp = 0, security = 0, inner_other = 0;
    std::uint64_t alptp_packets = 0, alptp_unsupported = 0;
    std::uint8_t protocol_version = 0xFF;  // last seen outer protocol_version
};

struct Callbacks {
    std::function<void(std::uint8_t plp, atsc3::ByteView bbp)> onBbp;
    std::function<void(atsc3::ByteView payload)> onPreamble;
    std::function<void(atsc3::ByteView payload)> onTmp;
    std::function<void(std::uint8_t plp, std::uint8_t sid, atsc3::ByteView alp)> onAlp;  // ALPTP
    std::function<void(atsc3::ByteView ip)> onOtherInner;  // inner IP packets not understood
};

class Demux {
public:
    explicit Demux(Callbacks cb);
    /// Feeds the UDP payload of one outer tunnel packet (RTP header included).
    void feedTunnelPacket(atsc3::ByteView udp_payload);
    const Stats& stats() const noexcept { return stats_; }
    void reset();

private:
    void processInner(atsc3::ByteView ip);
    void processAlptp(atsc3::ByteView payload, bool marker, std::uint16_t packet_offset);
    void drainAlptp();
    void onBbpPacket(std::uint16_t port, const struct Rtp0& r);

    struct Bpps {
        std::uint32_t expected = 0;
        std::vector<std::uint8_t> data;
        std::uint16_t last_seq = 0;
    };

    Callbacks cb_;
    Stats stats_;
    std::vector<std::uint8_t> buf_;
    bool synced_ = false;
    bool have_seq_ = false;
    std::uint16_t last_seq_ = 0;
    std::map<std::uint16_t, Bpps> bpps_;
    std::vector<std::uint8_t> alptp_buf_;
    bool alptp_synced_ = false;
};

}  // namespace atsc3::stltp
