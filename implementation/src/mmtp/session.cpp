#include "atsc3/mmtp/session.hpp"

#include "atsc3/io/inet.hpp"

namespace atsc3::mmtp {

Session::Session(SessionCallbacks cb) : cb_(std::move(cb)) {}

void Session::reset() {
    pids_.clear();
    pending_.clear();
    complete_.reset();
    subsets_.clear();
    atsc3_.clear();
    message_ids_.clear();
    assemblers_.clear();
    recent_pts_.clear();
    anchor_.reset();
    stats_ = SessionStats{};
}

void Session::setMpuAssembly(bool on) {
    if (mpu_enabled_ == on) return;
    mpu_enabled_ = on;
    if (!on) assemblers_.clear();
}

const MpuAssembler* Session::assembler(std::uint16_t packet_id) const {
    const auto it = assemblers_.find(packet_id);
    return it == assemblers_.end() ? nullptr : it->second.get();
}

std::vector<std::uint16_t> Session::assemblerIds() const {
    std::vector<std::uint16_t> v;
    for (const auto& [k, a] : assemblers_) v.push_back(k);
    return v;
}

void Session::flushMpus() {
    for (auto& [k, a] : assemblers_) a->flush();
}

void Session::resetMpuTimeline() {
    assemblers_.clear();
    recent_pts_.clear();
    anchor_.reset();
}

MpuAssembler& Session::assemblerFor(std::uint16_t packet_id) {
    auto it = assemblers_.find(packet_id);
    if (it != assemblers_.end()) return *it->second;
    MpuAssemblerCallbacks cb;
    cb.onInit = [this](std::shared_ptr<const MpuInitSegment> i) {
        if (cb_.onMpuInit) cb_.onMpuInit(std::move(i));
    };
    cb.onFragment = [this](std::shared_ptr<const MpuFragment> f) {
        if (cb_.onMpuFragment) cb_.onMpuFragment(std::move(f));
    };
    auto a = std::make_unique<MpuAssembler>(packet_id, std::move(cb), mpu_cfg_);
    if (anchor_) a->setAnchor(*anchor_);
    const auto pts = recent_pts_.find(packet_id);
    if (pts != recent_pts_.end())
        for (const MpuTimestamp& t : pts->second) a->setPresentationTime(t.mpu_sequence_number, t.ntp);
    it = assemblers_.emplace(packet_id, std::move(a)).first;
    return *it->second;
}

void Session::noteTimestamps(const MpTable& t) {
    for (const Asset& a : t.assets) {
        const auto pid = a.packetId();
        if (!pid || a.mpu_timestamps.empty()) continue;
        std::vector<MpuTimestamp>& recent = recent_pts_[*pid];
        for (const MpuTimestamp& ts : a.mpu_timestamps) {
            if (!anchor_) {
                anchor_ = ts.ntp;
                for (auto& [k, asm_] : assemblers_) asm_->setAnchor(ts.ntp);
            }
            bool known = false;
            for (const MpuTimestamp& r : recent)
                if (r.mpu_sequence_number == ts.mpu_sequence_number) known = true;
            if (!known) {
                recent.push_back(ts);
                if (recent.size() > 16) recent.erase(recent.begin());
            }
            if (mpu_enabled_) assemblerFor(*pid).setPresentationTime(ts.mpu_sequence_number, ts.ntp);
        }
    }
}

std::vector<PacketIdStats> Session::packetIds() const {
    std::vector<PacketIdStats> v;
    for (const auto& [k, s] : pids_) v.push_back(s);
    return v;
}

std::vector<MpTable> Session::subsetTables() const {
    std::vector<MpTable> v;
    for (const auto& [k, t] : subsets_) v.push_back(t);
    return v;
}

std::vector<Atsc3Message> Session::atsc3Messages() const {
    std::vector<Atsc3Message> v;
    for (const auto& [k, m] : atsc3_) v.push_back(m);
    return v;
}

std::vector<std::uint16_t> Session::messageIdsSeen() const {
    std::vector<std::uint16_t> v;
    for (const auto& [k, n] : message_ids_) v.push_back(k);
    return v;
}

void Session::onDatagram(const ip::UdpDatagram& d) {
    PacketHeader h;
    if (parsePacket(d.payload, h) != ParseStatus::Ok) {
        ++stats_.bad_header;
        return;
    }
    ++stats_.packets;
    PacketIdStats& s = pids_[h.packet_id];
    s.packet_id = h.packet_id;
    ++s.packets;
    s.bytes += d.payload.size();
    s.last_ns = d.time.t_ns;
    if (s.last_sequence && h.packet_sequence_number != *s.last_sequence + 1) ++s.sequence_gaps;
    s.last_sequence = h.packet_sequence_number;
    switch (h.type) {
        case PayloadType::Mpu: {
            ++s.mpu_packets;
            MpuPayloadHeader m;
            const ParseStatus st = parseMpuPayload(h.payload, m);
            if (st == ParseStatus::LengthMismatch) ++s.length_mismatch;
            if (st != ParseStatus::TooShort) {
                if (!s.last_mpu_sequence || *s.last_mpu_sequence != m.mpu_sequence_number) ++s.mpus;
                s.last_mpu_sequence = m.mpu_sequence_number;
                if (mpu_enabled_ && st == ParseStatus::Ok) assemblerFor(h.packet_id).onPacket(h, m, d.time.t_ns);
            }
            break;
        }
        case PayloadType::Signaling:
            ++s.signaling_packets;
            handleSignaling(h, h.payload);
            break;
        default:
            ++s.other_packets;
            break;
    }
}

void Session::handleSignaling(const PacketHeader& h, ByteView payload) {
    SignalingPayloadHeader sp;
    if (parseSignalingPayload(payload, sp) != ParseStatus::Ok) return;
    std::vector<std::uint8_t> assembled;
    ByteView whole;
    switch (sp.fragmentation_indicator) {
        case 0:
            whole = sp.data;
            break;
        case 1:
            ++stats_.fragments;
            pending_[h.packet_id].assign(sp.data.begin(), sp.data.end());
            return;
        case 2: {
            ++stats_.fragments;
            auto it = pending_.find(h.packet_id);
            if (it == pending_.end()) {
                ++stats_.fragment_drops;
                return;
            }
            it->second.insert(it->second.end(), sp.data.begin(), sp.data.end());
            return;
        }
        default: {
            ++stats_.fragments;
            auto it = pending_.find(h.packet_id);
            if (it == pending_.end()) {
                ++stats_.fragment_drops;
                return;
            }
            assembled = std::move(it->second);
            pending_.erase(it);
            assembled.insert(assembled.end(), sp.data.begin(), sp.data.end());
            whole = ByteView(assembled.data(), assembled.size());
            break;
        }
    }
    if (sp.aggregation) {
        std::size_t off = 0;
        const std::size_t lw = sp.long_length ? 4 : 2;
        while (off + lw <= whole.size()) {
            const std::size_t len = sp.long_length ? io::be32(whole.data() + off) : io::be16(whole.data() + off);
            off += lw;
            if (off + len > whole.size()) break;
            handleMessage(h.packet_id, whole.subview(off, len));
            off += len;
        }
    } else {
        handleMessage(h.packet_id, whole);
    }
}

void Session::handleMessage(std::uint16_t packet_id, ByteView msg) {
    Message m;
    if (!parseMessage(msg, m)) return;
    ++stats_.messages;
    ++message_ids_[m.message_id];
    if (cb_.onMessage) cb_.onMessage(packet_id, m);
    if (m.mp_table) {
        ++stats_.mp_tables;
        const MpTable& t = *m.mp_table;
        noteTimestamps(t);
        bool changed = false;
        if (t.complete()) {
            if (!complete_ || complete_->version != t.version || complete_->assets.size() != t.assets.size()) changed = true;
            MpTable merged = t;
            if (complete_) {  // keep subset-provided timestamps
                for (Asset& a : merged.assets)
                    for (const Asset& old : complete_->assets)
                        if (old.asset_id == a.asset_id && a.mpu_timestamps.empty()) a.mpu_timestamps = old.mpu_timestamps;
            }
            complete_ = std::move(merged);
        } else {
            auto it = subsets_.find(t.table_id);
            if (it == subsets_.end() || it->second.version != t.version) changed = true;
            subsets_[t.table_id] = t;
            if (complete_) {
                for (const Asset& sa : t.assets)
                    for (Asset& a : complete_->assets)
                        if (a.asset_id == sa.asset_id && !sa.mpu_timestamps.empty()) a.mpu_timestamps = sa.mpu_timestamps;
            }
        }
        if (changed && cb_.onMpTable) cb_.onMpTable(t);
    }
    if (m.atsc3) {
        ++stats_.atsc3_messages;
        std::uint64_t hsh = 1469598103934665603ull;
        for (const char c : m.atsc3->uri) hsh = (hsh ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
        const std::uint64_t key = (static_cast<std::uint64_t>(m.atsc3->content_type) << 48) ^ (hsh >> 16);
        auto it = atsc3_.find(key);
        const bool is_new = it == atsc3_.end() || it->second.content_version != m.atsc3->content_version || it->second.content != m.atsc3->content;
        atsc3_[key] = *m.atsc3;
        if (is_new && cb_.onAtsc3Message) cb_.onAtsc3Message(packet_id, *m.atsc3);
    }
}

}  // namespace atsc3::mmtp
