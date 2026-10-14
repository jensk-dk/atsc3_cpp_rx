#include "atsc3/tuners/pcap/format_detector.hpp"

#include <algorithm>
#include <sstream>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/io/inet.hpp"
#include "link_strip.hpp"

namespace atsc3::pcap {

namespace {
constexpr std::uint32_t kLlsAddress = 0xE000173Cu;  // 224.0.23.60
constexpr std::uint16_t kLlsPort = 4937;

bool looksLikeLct(ByteView u) noexcept {
    if (u.size() < 16) return false;
    const std::uint8_t v = static_cast<std::uint8_t>(u[0] >> 4);
    const std::uint8_t hdr_len = u[2];
    return v == 1 && hdr_len >= 2 && static_cast<std::size_t>(hdr_len) * 4 <= u.size();
}
bool looksLikeMmtp(ByteView u) noexcept {
    if (u.size() < 12) return false;
    const std::uint8_t v = static_cast<std::uint8_t>(u[0] >> 6);
    const std::uint8_t type = static_cast<std::uint8_t>(u[1] & 0x3F);
    return v == 1 && (type == 0 || type == 2);
}
bool isTunnelPt(std::uint8_t pt) noexcept { return pt == 97 || pt == 82 || pt == 81; }

int rank(CaptureFormat f) {
    switch (f) {
        case CaptureFormat::Bbp: return 4;
        case CaptureFormat::Stltp: return 3;
        case CaptureFormat::Alp: return 2;
        case CaptureFormat::IpBypass: return 1;
        default: return 0;
    }
}
}  // namespace

const char* toString(CaptureFormat f) noexcept {
    switch (f) {
        case CaptureFormat::Alp: return "ALP";
        case CaptureFormat::Bbp: return "BBP";
        case CaptureFormat::Stltp: return "STLTP";
        case CaptureFormat::IpBypass: return "IP bypass";
        default: return "Unsupported";
    }
}

FormatDetector::FormatDetector(DetectorConfig cfg) : cfg_(cfg) {}

bool FormatDetector::feed(const PcapRecord& rec) {
    if (records_ >= cfg_.probe_packets) return false;
    if (!have_ts_) {
        first_ts_ = rec.ts_ns;
        have_ts_ = true;
    } else if (cfg_.probe_seconds > 0 &&
               static_cast<double>(rec.ts_ns - first_ts_) > cfg_.probe_seconds * 1e9) {
        return false;
    }
    ++records_;
    if_types_[rec.interface_id] = rec.link_type;
    const ByteView frame(rec.data.data(), rec.data.size());
    if (rec.link_type == linktype::kAtscAlp) {
        ++alp_;
        ++if_alp_[rec.interface_id];
        return true;
    }
    if (linktype::isUser(rec.link_type)) {
        BbpContainerRecord b;
        if (parseBbpContainerRecord(frame, b)) {
            ++bbp_;
            ++if_bbp_[rec.interface_id];
        } else {
            ++non_ip_;
        }
        return true;
    }
    ByteView ip;
    if (!linktype::isIpBearing(rec.link_type) || !stripToIpv4(rec.link_type, frame, ip)) {
        ++non_ip_;
        return true;
    }
    classifyIp(rec.interface_id, ip.data(), ip.size());
    return true;
}

void FormatDetector::classifyIp(std::uint32_t if_id, const std::uint8_t* data, std::size_t len) {
    const ByteView ip(data, len);
    io::Ipv4Udp h;
    if (!io::parseIpv4Udp(ip, h)) {
        ++non_ip_;
        return;
    }
    if (!h.udp) {
        ++ipv4_other_;
        std::string marker;
        if (marker_.empty() && extractMarkerText(ip, marker)) marker_ = marker;
        return;
    }
    ++ipv4_udp_;
    const std::uint64_t key = (static_cast<std::uint64_t>(h.dst) << 16) | h.dport;
    FlowState& fs = flows_[key];
    fs.ev.dst = h.dst;
    fs.ev.port = h.dport;
    ++fs.ev.packets;

    io::Rtp rtp;
    const bool configured_tunnel = cfg_.stltp_address && cfg_.stltp_port && *cfg_.stltp_address == h.dst &&
                                   *cfg_.stltp_port == h.dport;
    if (io::parseRtp(h.payload, rtp) && rtp.version == 2 && rtp.cc == 0 && (isTunnelPt(rtp.pt) || configured_tunnel)) {
        if (fs.ev.cls != FlowEvidence::Class::Tunnel) {
            fs.ev.cls = FlowEvidence::Class::Tunnel;
            fs.ev.rtp_pt = rtp.pt;
            fs.ev.seq_continuous = 0;
            fs.has_seq = false;
        }
        if (fs.has_seq && static_cast<std::uint16_t>(fs.last_seq + 1) == rtp.seq) ++fs.ev.seq_continuous;
        fs.last_seq = rtp.seq;
        fs.has_seq = true;
        ++if_tunnel_[if_id];
        return;
    }
    if (h.dst == kLlsAddress && h.dport == kLlsPort) {
        fs.ev.cls = FlowEvidence::Class::Lls;
        ++if_atsc_udp_[if_id];
    } else if (io::isMulticast(h.dst) && looksLikeLct(h.payload)) {
        fs.ev.cls = FlowEvidence::Class::Lct;
        ++if_atsc_udp_[if_id];
    } else if (io::isMulticast(h.dst) && looksLikeMmtp(h.payload)) {
        fs.ev.cls = FlowEvidence::Class::Mmtp;
        ++if_atsc_udp_[if_id];
    }
}

Detection FormatDetector::finish() const {
    Detection d;
    d.records_probed = records_;
    d.ipv4_udp = ipv4_udp_;
    d.ipv4_other = ipv4_other_;
    d.non_ip = non_ip_;
    d.alp_records = alp_;
    d.bbp_records = bbp_;
    d.marker_text = marker_;
    for (const auto& [id, lt] : if_types_) {
        d.link_types.push_back(lt);
    }
    // Tunnel verdict needs sequence continuity on at least stltp_min_packets datagrams of one flow.
    std::map<std::uint32_t, bool> if_has_tunnel;
    std::map<std::uint8_t, std::uint32_t> pt_votes;
    for (const auto& [key, fs] : flows_) {
        d.flows.push_back(fs.ev);
        if (fs.ev.cls == FlowEvidence::Class::Tunnel && fs.ev.seq_continuous + 1 >= cfg_.stltp_min_packets) {
            pt_votes[fs.ev.rtp_pt] += fs.ev.packets;
            for (const auto& [id, n] : if_tunnel_) if_has_tunnel[id] = true;
        }
    }
    for (const auto& [id, lt] : if_types_) {
        CaptureFormat f = CaptureFormat::Unsupported;
        if (lt == linktype::kAtscAlp && if_alp_.count(id)) f = CaptureFormat::Alp;
        else if (linktype::isUser(lt) && if_bbp_.count(id)) f = CaptureFormat::Bbp;
        else if (if_has_tunnel.count(id)) f = CaptureFormat::Stltp;
        else if (if_atsc_udp_.count(id)) f = CaptureFormat::IpBypass;
        d.per_interface[id] = f;
        if (rank(f) > rank(d.format)) d.format = f;
    }
    if (d.format == CaptureFormat::Stltp && !pt_votes.empty()) {
        const auto best = std::max_element(pt_votes.begin(), pt_votes.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
        d.tunnel_pt = best->first;
    }
    std::sort(d.flows.begin(), d.flows.end(),
              [](const FlowEvidence& a, const FlowEvidence& b) { return a.packets > b.packets; });
    return d;
}

std::string Detection::summary() const {
    std::ostringstream os;
    os << "format=" << toString(format) << " records=" << records_probed << " link_types=";
    for (std::size_t i = 0; i < link_types.size(); ++i) os << (i ? "," : "") << link_types[i];
    os << " alp=" << alp_records << " bbp=" << bbp_records << " udp=" << ipv4_udp << " ip_other=" << ipv4_other
       << " non_ip=" << non_ip;
    if (tunnel_pt) os << " tunnel_pt=" << static_cast<int>(*tunnel_pt);
    std::size_t shown = 0;
    for (const FlowEvidence& f : flows) {
        if (shown++ >= 8) break;
        const char* cls = f.cls == FlowEvidence::Class::Tunnel ? "tunnel"
                          : f.cls == FlowEvidence::Class::Lls  ? "lls"
                          : f.cls == FlowEvidence::Class::Lct  ? "lct"
                          : f.cls == FlowEvidence::Class::Mmtp ? "mmtp"
                                                                : "udp";
        os << " [" << io::ipv4ToString(f.dst) << ":" << f.port << " " << cls << " n=" << f.packets;
        if (f.cls == FlowEvidence::Class::Tunnel) os << " pt=" << static_cast<int>(f.rtp_pt);
        os << "]";
    }
    if (!marker_text.empty()) os << " marker=\"" << marker_text << "\"";
    return os.str();
}

Detection detectCaptureFormat(PcapReader& reader, const DetectorConfig& cfg) {
    FormatDetector det(cfg);
    PcapRecord rec;
    while (reader.next(rec)) {
        if (!det.feed(rec)) break;
    }
    reader.rewind();
    return det.finish();
}

}  // namespace atsc3::pcap
