#pragma once
// Capture format detection for the pcap virtual tuner (spec 5.1.2).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/tuners/pcap/pcap_reader.hpp"

namespace atsc3::pcap {

enum class CaptureFormat { Alp, Bbp, Stltp, IpBypass, Unsupported };
const char* toString(CaptureFormat f) noexcept;

struct DetectorConfig {
    std::uint32_t probe_packets = 256;
    double probe_seconds = 2.0;
    std::uint32_t stltp_min_packets = 8;
    std::uint32_t stltp_reorder_window = 64;
    std::optional<std::uint32_t> stltp_address;  // configured tunnel destination (host order)
    std::optional<std::uint16_t> stltp_port;
};

struct FlowEvidence {
    enum class Class { Tunnel, Lls, Lct, Mmtp, OtherUdp };
    std::uint32_t dst = 0;
    std::uint16_t port = 0;
    Class cls = Class::OtherUdp;
    std::uint32_t packets = 0;
    std::uint8_t rtp_pt = 0;           // for Tunnel
    std::uint32_t seq_continuous = 0;  // consecutive sequence numbers seen (Tunnel)
};

struct Detection {
    CaptureFormat format = CaptureFormat::Unsupported;
    std::map<std::uint32_t, CaptureFormat> per_interface;  // pcapng interfaces
    std::vector<std::uint16_t> link_types;
    std::vector<FlowEvidence> flows;       // UDP flows seen during the probe
    std::uint64_t records_probed = 0;
    std::uint64_t ipv4_udp = 0, ipv4_other = 0, non_ip = 0, alp_records = 0, bbp_records = 0;
    std::optional<std::uint8_t> tunnel_pt;  // dominant tunnel payload type when format == Stltp
    std::string marker_text;                // e.g. the DekTec ICMP "bootAddress=..." marker
    std::string summary() const;
};

class FormatDetector {
public:
    explicit FormatDetector(DetectorConfig cfg = {});
    /// Feeds one record; returns false once the probe window is exhausted.
    bool feed(const PcapRecord& rec);
    Detection finish() const;

private:
    struct FlowState {
        FlowEvidence ev;
        std::uint16_t last_seq = 0;
        bool has_seq = false;
    };
    void classifyIp(std::uint32_t if_id, const std::uint8_t* ip, std::size_t len);

    DetectorConfig cfg_;
    std::uint64_t records_ = 0;
    std::int64_t first_ts_ = 0;
    bool have_ts_ = false;
    std::map<std::uint64_t, FlowState> flows_;  // key: dst<<16 | port
    std::map<std::uint32_t, std::uint16_t> if_types_;
    std::map<std::uint32_t, std::uint64_t> if_alp_, if_bbp_, if_atsc_udp_, if_tunnel_;
    std::uint64_t ipv4_udp_ = 0, ipv4_other_ = 0, non_ip_ = 0, alp_ = 0, bbp_ = 0;
    std::string marker_;
};

/// Probes the reader (up to the configured window) and rewinds it afterwards.
Detection detectCaptureFormat(PcapReader& reader, const DetectorConfig& cfg = {});

}  // namespace atsc3::pcap
