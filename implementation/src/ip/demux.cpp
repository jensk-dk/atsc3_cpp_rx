#include "atsc3/ip/demux.hpp"

#include <algorithm>

#include "atsc3/io/inet.hpp"

namespace atsc3::ip {

namespace {
std::uint16_t ones16(const std::uint8_t* p, std::size_t len, std::uint32_t seed = 0) {
    std::uint32_t sum = seed;
    for (std::size_t i = 0; i + 1 < len; i += 2) sum += static_cast<std::uint32_t>((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += static_cast<std::uint32_t>(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(sum);
}
}  // namespace

Demux::Demux(DemuxConfig cfg) : cfg_(cfg) {}

void Demux::reset() {
    fragments_.clear();
    unrouted_.clear();
}

SubscriptionId Demux::subscribe(FlowKey key, FlowHandler handler) {
    const SubscriptionId id = next_id_++;
    subs_.push_back(Subscriber{id, key, false, false, std::make_shared<FlowHandler>(std::move(handler))});
    return id;
}

SubscriptionId Demux::tapAll(FlowHandler handler) {
    const SubscriptionId id = next_id_++;
    subs_.push_back(Subscriber{id, FlowKey{}, true, false, std::make_shared<FlowHandler>(std::move(handler))});
    return id;
}

void Demux::unsubscribe(SubscriptionId id) {
    for (Subscriber& s : subs_) {
        if (s.id == id) s.dead = true;
    }
    if (delivering_ > 0) {
        pending_erase_ = true;
        return;
    }
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [](const Subscriber& s) { return s.dead; }), subs_.end());
}

std::vector<UnroutedFlow> Demux::unroutedFlows() const {
    std::vector<UnroutedFlow> v;
    v.reserve(unrouted_.size());
    for (const auto& [k, f] : unrouted_) v.push_back(f);
    std::sort(v.begin(), v.end(), [](const UnroutedFlow& a, const UnroutedFlow& b) { return a.packets > b.packets; });
    return v;
}

void Demux::expireFragments(std::int64_t now_ns) {
    for (auto it = fragments_.begin(); it != fragments_.end();) {
        if (now_ns - it->second.first_ns > cfg_.fragment_timeout_ns) {
            ++stats_.fragments_dropped;
            it = fragments_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Demux::reassemble(std::uint8_t plp, ByteView ip, io::UnitTime time, std::optional<std::uint8_t> sid,
                       std::vector<std::uint8_t>& out) {
    const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0x0F) * 4;
    const std::uint16_t total = io::be16(ip.data() + 2);
    const std::uint16_t id = io::be16(ip.data() + 4);
    const std::uint16_t frag = io::be16(ip.data() + 6);
    const bool mf = (frag & 0x2000) != 0;
    const std::uint32_t offset = static_cast<std::uint32_t>(frag & 0x1FFF) * 8;
    const std::uint64_t key = (static_cast<std::uint64_t>(io::be32(ip.data() + 12)) << 32) ^ io::be32(ip.data() + 16) ^
                              (static_cast<std::uint64_t>(id) << 8) ^ ip[9];
    expireFragments(time.t_ns);
    FragmentBuffer& fb = fragments_[key];
    if (fb.data.empty()) {
        fb.first_ns = time.t_ns;
        fb.plp = plp;
        fb.sid = sid;
    }
    const std::size_t body_len = std::min<std::size_t>(total, ip.size()) - ihl;
    const std::uint32_t end = offset + static_cast<std::uint32_t>(body_len);
    if (end > cfg_.fragment_max_bytes) {
        ++stats_.fragments_dropped;
        fragments_.erase(key);
        return false;
    }
    if (offset == 0) {
        // keep the first fragment's header so the reassembled packet is a normal IPv4 packet
        std::vector<std::uint8_t> hdr(ip.begin(), ip.begin() + static_cast<std::ptrdiff_t>(ihl));
        if (fb.data.size() < ihl) fb.data.resize(ihl);
        std::copy(hdr.begin(), hdr.end(), fb.data.begin());
    }
    if (fb.data.size() < ihl + end) fb.data.resize(ihl + end);
    std::copy(ip.begin() + static_cast<std::ptrdiff_t>(ihl), ip.begin() + static_cast<std::ptrdiff_t>(ihl + body_len),
              fb.data.begin() + static_cast<std::ptrdiff_t>(ihl + offset));
    fb.ranges.emplace_back(offset, end);
    if (!mf) fb.total = end;
    if (!fb.total) return false;
    // contiguous from 0 to total?
    std::sort(fb.ranges.begin(), fb.ranges.end());
    std::uint32_t covered = 0;
    for (const auto& [b, e] : fb.ranges) {
        if (b > covered) return false;
        covered = std::max(covered, e);
    }
    if (covered < *fb.total) return false;
    out = std::move(fb.data);
    out.resize(ihl + *fb.total);
    // fix total length and clear fragment fields
    const std::uint16_t new_total = static_cast<std::uint16_t>(out.size());
    out[2] = static_cast<std::uint8_t>(new_total >> 8);
    out[3] = static_cast<std::uint8_t>(new_total & 0xFF);
    out[6] = static_cast<std::uint8_t>(out[6] & 0xC0);
    out[7] = 0;
    fragments_.erase(key);
    ++stats_.reassembled;
    return true;
}

void Demux::onIpv4(std::uint8_t plp, ByteView ip, io::UnitTime time, std::optional<std::uint8_t> sid) {
    if (ip.size() < 20 || (ip[0] >> 4) != 4) {
        if (!ip.empty() && (ip[0] >> 4) == 6) ++stats_.ipv6_or_other;
        else ++stats_.bad_ipv4;
        return;
    }
    ++stats_.ipv4;
    if (!fragments_.empty()) expireFragments(time.t_ns);
    const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0x0F) * 4;
    const std::uint16_t total = io::be16(ip.data() + 2);
    if (ihl < 20 || ihl > ip.size() || total < ihl) {
        ++stats_.bad_ipv4;
        return;
    }
    if (cfg_.verify_ip_checksum && ones16(ip.data(), ihl) != 0xFFFF) {
        ++stats_.ip_checksum_bad;
        return;
    }
    const std::uint16_t frag = io::be16(ip.data() + 6);
    std::vector<std::uint8_t> reassembled;
    if ((frag & 0x3FFF) != 0) {
        ++stats_.fragments;
        if (!reassemble(plp, ip, time, sid, reassembled)) return;
        ip = ByteView(reassembled.data(), reassembled.size());
    }
    io::Ipv4Udp h;
    if (!io::parseIpv4Udp(ip, h)) {
        ++stats_.bad_ipv4;
        return;
    }
    if (h.protocol != 17) {
        ++stats_.non_udp;
        return;
    }
    if (!h.udp) {
        ++stats_.udp_bad_length;
        return;
    }
    const std::uint8_t* udp = ip.data() + ihl;
    const std::uint16_t ulen = io::be16(udp + 4);
    const std::size_t avail = std::min<std::size_t>(h.total_len, ip.size()) - ihl;
    if (ulen < 8 || ulen > avail) {
        ++stats_.udp_bad_length;
        return;
    }
    const std::uint16_t csum = io::be16(udp + 6);
    if (csum != 0) {
        std::uint8_t pseudo[12];
        for (int i = 0; i < 8; ++i) pseudo[i] = ip[12 + static_cast<std::size_t>(i)];
        pseudo[8] = 0;
        pseudo[9] = 17;
        pseudo[10] = static_cast<std::uint8_t>(ulen >> 8);
        pseudo[11] = static_cast<std::uint8_t>(ulen & 0xFF);
        const std::uint16_t ps = ones16(pseudo, 12);
        if (ones16(udp, ulen, ps) != 0xFFFF) {
            ++stats_.udp_checksum_bad;
            if (cfg_.strict_udp_checksum) return;
        }
    }
    ++stats_.udp;
    UdpDatagram d;
    d.plp = plp;
    d.sid = sid;
    d.time = time;
    d.src_ip = h.src;
    d.dst_ip = h.dst;
    d.src_port = h.sport;
    d.dst_port = h.dport;
    d.payload = ByteView(udp + 8, static_cast<std::size_t>(ulen) - 8);
    deliver(d);
}

void Demux::deliver(const UdpDatagram& d) {
    bool routed = false;
    bool have_exact = false;
    for (const Subscriber& s : subs_) {
        if (s.tap || s.dead) continue;
        if (s.key.dst_ip == d.dst_ip && s.key.dst_port == d.dst_port && s.key.src_ip != 0 && s.key.src_ip == d.src_ip) have_exact = true;
    }
    // Handlers may subscribe (vector may reallocate) or unsubscribe (deferred erase) while we iterate:
    // index-based loop over the subscribers present at entry, holding a reference to the handler.
    ++delivering_;
    const std::size_t n = subs_.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (subs_[i].dead) continue;
        const Subscriber& s = subs_[i];
        bool match = s.tap;
        if (!match && s.key.dst_ip == d.dst_ip && s.key.dst_port == d.dst_port) {
            if (s.key.src_ip != 0) match = s.key.src_ip == d.src_ip;
            else match = !have_exact;  // wildcard entries yield to an exact source subscriber
        }
        if (!match) continue;
        if (!s.tap) routed = true;
        const std::shared_ptr<FlowHandler> h = s.handler;
        (*h)(d);
    }
    if (--delivering_ == 0 && pending_erase_) {
        pending_erase_ = false;
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [](const Subscriber& s) { return s.dead; }), subs_.end());
    }
    if (routed) {
        ++stats_.routed;
        return;
    }
    ++stats_.unrouted;
    const std::uint64_t key = ((static_cast<std::uint64_t>(d.dst_ip) << 32) | (static_cast<std::uint64_t>(d.dst_port) << 8)) ^ d.src_ip;
    auto it = unrouted_.find(key);
    if (it == unrouted_.end()) {
        if (unrouted_.size() >= cfg_.max_unrouted_flows) return;
        it = unrouted_.emplace(key, UnroutedFlow{d.dst_ip, d.src_ip, d.dst_port, 0, 0, d.plp}).first;
    }
    ++it->second.packets;
    it->second.bytes += d.payload.size();
}

}  // namespace atsc3::ip
