#include "atsc3/stltp/demux.hpp"

#include "atsc3/io/inet.hpp"

namespace atsc3::stltp {

using atsc3::ByteView;
using atsc3::io::Ipv4Udp;
using atsc3::io::Rtp;

struct Rtp0 : Rtp {};

Demux::Demux(Callbacks cb) : cb_(std::move(cb)) {}

void Demux::reset() {
    buf_.clear();
    synced_ = false;
    have_seq_ = false;
    bpps_.clear();
    alptp_buf_.clear();
    alptp_synced_ = false;
}

void Demux::feedTunnelPacket(ByteView udp_payload) {
    Rtp r;
    if (!atsc3::io::parseRtp(udp_payload, r) || r.version != 2) {
        ++stats_.outer_not_rtp;
        return;
    }
    ++stats_.outer_packets;
    // CTP header word in the SSRC position: protocol_version(2) redundancy(2) channels(2) reserved(10) packet_offset(16)
    stats_.protocol_version = static_cast<std::uint8_t>(r.ssrc >> 30);
    const std::uint16_t packet_offset = static_cast<std::uint16_t>(r.ssrc & 0xFFFF);
    if (have_seq_ && static_cast<std::uint16_t>(last_seq_ + 1) != r.seq) {
        ++stats_.outer_seq_gaps;
        synced_ = false;
        alptp_synced_ = false;
        buf_.clear();
        alptp_buf_.clear();
        bpps_.clear();
    }
    last_seq_ = r.seq;
    have_seq_ = true;

    if (r.pt == kPtAlptp) {
        processAlptp(r.payload, r.marker, packet_offset);
        return;
    }
    if (r.pt != kPtStltp) {
        ++stats_.outer_other_pt;
        return;
    }
    if (!synced_) {
        if (!r.marker) return;
        if (packet_offset > r.payload.size()) return;
        buf_.assign(r.payload.begin() + packet_offset, r.payload.end());
        synced_ = true;
    } else {
        buf_.insert(buf_.end(), r.payload.begin(), r.payload.end());
    }
    // Reframe inner IPv4 packets by their total length.
    std::size_t off = 0;
    while (buf_.size() - off >= 20) {
        const std::uint8_t* p = buf_.data() + off;
        if ((p[0] >> 4) != 4) {
            ++stats_.outer_resync;
            synced_ = false;
            buf_.clear();
            return;
        }
        const std::uint16_t tl = atsc3::io::be16(p + 2);
        if (tl < 20) {
            ++stats_.inner_bad_len;
            synced_ = false;
            buf_.clear();
            return;
        }
        if (buf_.size() - off < tl) break;
        processInner(ByteView(p, tl));
        off += tl;
    }
    if (off) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off));
}

void Demux::processInner(ByteView ip) {
    ++stats_.inner_packets;
    Ipv4Udp h;
    if (!atsc3::io::parseIpv4Udp(ip, h) || !h.udp) {
        ++stats_.inner_not_udp;
        if (cb_.onOtherInner) cb_.onOtherInner(ip);
        return;
    }
    Rtp0 r;
    if (!atsc3::io::parseRtp(h.payload, r)) {
        ++stats_.inner_no_rtp;
        if (cb_.onOtherInner) cb_.onOtherInner(ip);
        return;
    }
    const std::uint16_t port = static_cast<std::uint16_t>(h.dport % 100);  // channel offset 100 per RF channel
    const std::uint16_t base = static_cast<std::uint16_t>(h.dport - port);
    if (base < kPortBbpBase || base > kPortBbpBase + 300) {
        ++stats_.inner_other;
        if (cb_.onOtherInner) cb_.onOtherInner(ip);
        return;
    }
    if (port < 64) {
        onBbpPacket(h.dport, r);
    } else if (port == kPortPreamble - kPortBbpBase) {
        ++stats_.preamble;
        if (cb_.onPreamble) cb_.onPreamble(r.payload);
    } else if (port == kPortTmp - kPortBbpBase) {
        ++stats_.tmp;
        if (cb_.onTmp) cb_.onTmp(r.payload);
    } else if (port == kPortSecurity - kPortBbpBase) {
        ++stats_.security;
    } else {
        ++stats_.inner_other;
        if (cb_.onOtherInner) cb_.onOtherInner(ip);
    }
}

void Demux::onBbpPacket(std::uint16_t port, const Rtp0& r) {
    auto it = bpps_.find(port);
    if (r.marker) {
        if (it != bpps_.end() && it->second.data.size() < it->second.expected) ++stats_.bpps_incomplete;
        Bpps& b = bpps_[port];
        b.expected = r.ssrc;
        b.data.assign(r.payload.begin(), r.payload.end());
        b.last_seq = r.seq;
        it = bpps_.find(port);
    } else {
        if (it == bpps_.end()) {
            ++stats_.bpps_no_start;
            return;
        }
        Bpps& b = it->second;
        if (static_cast<std::uint16_t>(b.last_seq + 1) != r.seq) {
            ++stats_.bpps_seq_gap;
            bpps_.erase(it);
            return;
        }
        b.data.insert(b.data.end(), r.payload.begin(), r.payload.end());
        b.last_seq = r.seq;
    }
    Bpps& b = it->second;
    if (b.data.size() >= b.expected) {
        if (b.data.size() > b.expected) ++stats_.bpps_overrun;
        ++stats_.bbp;
        const std::uint8_t plp = static_cast<std::uint8_t>((port - kPortBbpBase) % 100);
        if (cb_.onBbp) cb_.onBbp(plp, ByteView(b.data.data(), b.expected));
        bpps_.erase(it);
    }
}

// ALPTP (A/324 §8.2): tunneled packets are alptp_information_header() + ALP packet.
void Demux::processAlptp(ByteView payload, bool marker, std::uint16_t packet_offset) {
    if (!alptp_synced_) {
        if (!marker || packet_offset > payload.size()) return;
        alptp_buf_.assign(payload.begin() + packet_offset, payload.end());
        alptp_synced_ = true;
    } else {
        alptp_buf_.insert(alptp_buf_.end(), payload.begin(), payload.end());
    }
    drainAlptp();
}

void Demux::drainAlptp() {
    std::size_t off = 0;
    for (;;) {
        if (alptp_buf_.size() - off < 4) break;
        const std::uint8_t* p = alptp_buf_.data() + off;
        const std::uint16_t length = atsc3::io::be16(p);
        if (length == 0) {
            // Security Data Stream packet follows (RTP/UDP/IP) — not supported: resync at next marker.
            ++stats_.alptp_unsupported;
            alptp_synced_ = false;
            alptp_buf_.clear();
            return;
        }
        const std::uint8_t sid = p[2];
        const std::uint8_t plp = static_cast<std::uint8_t>(p[3] >> 2);
        const bool lls_flag = (p[3] & 0x02) != 0;
        (void)lls_flag;
        // bits after plp_id: lls_flag(1) lmt_rdt_flag(1) | rap(1) time_limit(1) wakeup/res(2) signed(1) reserved(27)
        if (alptp_buf_.size() - off < 8) break;
        const std::uint8_t b4 = p[4];
        const bool time_limit = (b4 & 0x40) != 0;
        const bool signed_flag = (b4 & 0x08) != 0;
        std::size_t hdr = 8;
        if (time_limit) hdr += 4;
        if (signed_flag) {
            ++stats_.alptp_unsupported;  // GMAC header extension length unknown here
            alptp_synced_ = false;
            alptp_buf_.clear();
            return;
        }
        if (alptp_buf_.size() - off < hdr + length) break;
        ++stats_.alptp_packets;
        if (cb_.onAlp) cb_.onAlp(plp, sid, ByteView(p + hdr, length));
        off += hdr + length;
    }
    if (off) alptp_buf_.erase(alptp_buf_.begin(), alptp_buf_.begin() + static_cast<std::ptrdiff_t>(off));
}

}  // namespace atsc3::stltp
