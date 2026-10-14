#include "atsc3/io/inet.hpp"
#include "atsc3/link/signaling.hpp"

namespace atsc3::link {

bool parseLmt(ByteView t, Lmt& out) noexcept {
    out = Lmt{};
    if (t.empty()) return false;
    const std::size_t num_plps = static_cast<std::size_t>(t[0] >> 2) + 1;
    std::size_t o = 1;
    for (std::size_t i = 0; i < num_plps; ++i) {
        if (o + 2 > t.size()) return false;
        LmtPlp plp;
        plp.plp_id = static_cast<std::uint8_t>(t[o] >> 2);
        const std::size_t n = t[o + 1];
        o += 2;
        for (std::size_t j = 0; j < n; ++j) {
            if (o + 13 > t.size()) return false;
            LmtMulticast m;
            m.src_ip = io::be32(t.data() + o);
            m.dst_ip = io::be32(t.data() + o + 4);
            m.src_port = io::be16(t.data() + o + 8);
            m.dst_port = io::be16(t.data() + o + 10);
            const std::uint8_t flags = t[o + 12];
            o += 13;
            if (flags & 0x80) {
                if (o >= t.size()) return false;
                m.sid = t[o++];
            }
            if (flags & 0x40) {
                if (o >= t.size()) return false;
                m.context_id = t[o++];
            }
            plp.multicasts.push_back(m);
        }
        out.plps.push_back(std::move(plp));
    }
    return true;
}

bool parseRdt(ByteView t, Rdt& out) noexcept {
    out = Rdt{};
    if (t.size() < 4) return false;
    out.plp_id = static_cast<std::uint8_t>(t[0] >> 2);
    // PLP_ID(6) max_CID(8) adaptation_mode(2) context_config(2) reserved(6) num_context(8)
    out.max_cid = static_cast<std::uint8_t>(((t[0] & 0x03) << 6) | (t[1] >> 2));
    out.adaptation_mode = static_cast<std::uint8_t>(t[1] & 0x03);
    out.context_config = static_cast<std::uint8_t>(t[2] >> 6);
    const std::size_t n = t[3];
    std::size_t o = 4;
    for (std::size_t i = 0; i < n; ++i) {
        if (o + 2 > t.size()) return false;
        RdtContext c;
        c.context_id = t[o];
        c.profile = t[o + 1];
        c.context_config = out.context_config;
        o += 2;
        if (out.context_config != 0) {
            if (o >= t.size()) return false;
            const std::size_t len = t[o++];
            if (o + len > t.size()) return false;
            c.chain.assign(t.data() + o, t.data() + o + len);
            o += len;
        }
        out.contexts.push_back(std::move(c));
    }
    return true;
}

void PlpMap::applyLmt(std::uint8_t carrier_plp, const Lmt& lmt) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->carrier == carrier_plp) it = entries_.erase(it);
        else ++it;
    }
    for (const LmtPlp& p : lmt.plps) {
        for (const LmtMulticast& m : p.multicasts) {
            Keyed k;
            k.dst_ip = m.dst_ip;
            k.dst_port = m.dst_port;
            k.carrier = carrier_plp;
            k.entry.plp_id = p.plp_id;
            k.entry.src_ip = m.src_ip;
            k.entry.src_port = m.src_port;
            k.entry.sid = m.sid;
            k.entry.context_id = m.context_id;
            k.entry.origin = io::LmtOrigin::Broadcast;
            entries_.push_back(k);
        }
    }
    carriers_[carrier_plp] = lmt.version;
}

void PlpMap::applySidecar(const std::vector<io::LmtEntry>& entries) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->carrier == 0xFF) it = entries_.erase(it);
        else ++it;
    }
    for (const io::LmtEntry& e : entries) {
        Keyed k;
        k.dst_ip = e.dst_ip;
        k.dst_port = e.dst_port;
        k.carrier = 0xFF;
        k.entry.plp_id = e.plp_id;
        k.entry.src_ip = e.src_ip;
        k.entry.src_port = e.src_port;
        k.entry.origin = io::LmtOrigin::Sidecar;
        entries_.push_back(k);
    }
}

std::optional<PlpMap::Entry> PlpMap::lookup(std::uint32_t dst_ip, std::uint16_t dst_port, std::uint32_t src_ip,
                                            std::uint16_t src_port) const {
    const Keyed* wildcard = nullptr;
    for (const Keyed& k : entries_) {
        if (k.dst_ip != dst_ip || k.dst_port != dst_port) continue;
        const bool src_ok = k.entry.src_ip == 0 || src_ip == 0 || k.entry.src_ip == src_ip;
        const bool port_ok = k.entry.src_port == 0 || src_port == 0 || k.entry.src_port == src_port;
        if (!src_ok || !port_ok) continue;
        if (k.entry.src_ip != 0 && src_ip != 0) return k.entry;  // exact source match
        if (!wildcard) wildcard = &k;
    }
    if (wildcard) return wildcard->entry;
    return std::nullopt;
}

std::vector<PlpMap::Entry> PlpMap::entries() const {
    std::vector<Entry> v;
    v.reserve(entries_.size());
    for (const Keyed& k : entries_) {
        Entry e = k.entry;
        e.dst_ip = k.dst_ip;
        e.dst_port = k.dst_port;
        v.push_back(e);
    }
    return v;
}

LinkSignaling::LinkSignaling(Callbacks cb) : cb_(std::move(cb)) {}

void LinkSignaling::reset() {
    map_.clear();
    lmts_.clear();
    rdts_.clear();
}

std::optional<Lmt> LinkSignaling::lmt(std::uint8_t plp) const {
    const auto it = lmts_.find(plp);
    if (it == lmts_.end()) return std::nullopt;
    return it->second;
}

std::optional<Rdt> LinkSignaling::rdt(std::uint8_t plp) const {
    const auto it = rdts_.find(plp);
    if (it == rdts_.end()) return std::nullopt;
    return it->second;
}

void LinkSignaling::onSignaling(std::uint8_t plp, const alp::SignalingInfo& info, ByteView table) {
    ++stats_.tables;
    if (info.type >= 0xF0) {
        ++stats_.user_private;
        if (cb_.onUserPrivate) cb_.onUserPrivate(plp, info, table);
        return;
    }
    if (info.format != 0 || info.encoding != 0) {  // LMT/RDT are binary, uncompressed (Tables 7.1, 7.3)
        ++stats_.unsupported;
        if (cb_.onUnparsed) cb_.onUnparsed(plp, info, table);
        return;
    }
    if (info.type == kSignalingLmt) {
        ++stats_.lmt;
        const auto it = lmts_.find(plp);
        if (it != lmts_.end() && it->second.version == info.version) return;  // repetition
        Lmt lmt;
        if (!parseLmt(table, lmt)) {
            ++stats_.invalid;
            return;
        }
        lmt.version = info.version;
        ++stats_.lmt_new;
        map_.applyLmt(plp, lmt);
        lmts_[plp] = lmt;
        if (cb_.onLmt) cb_.onLmt(plp, lmts_[plp]);
    } else if (info.type == kSignalingRdt) {
        ++stats_.rdt;
        const auto it = rdts_.find(plp);
        if (it != rdts_.end() && it->second.version == info.version) return;
        Rdt rdt;
        if (!parseRdt(table, rdt)) {
            ++stats_.invalid;
            return;
        }
        rdt.version = info.version;
        ++stats_.rdt_new;
        rdts_[plp] = rdt;
        if (cb_.onRdt) cb_.onRdt(plp, rdts_[plp]);
    } else {
        ++stats_.unsupported;
        if (cb_.onUnparsed) cb_.onUnparsed(plp, info, table);
    }
}

}  // namespace atsc3::link
