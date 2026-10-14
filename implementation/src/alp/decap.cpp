#include "atsc3/alp/decap.hpp"

#include <cstring>

namespace atsc3::alp {

namespace {
constexpr std::size_t kTsPacket = 188;
constexpr std::size_t kTsNoSync = 187;
constexpr std::size_t kTsPayload = 184;
}  // namespace

Decap::Decap(Callbacks cb) : cb_(std::move(cb)) {}

void Decap::reset() {
    reassembly_.clear();
}

void Decap::setSidFilter(std::uint8_t plp, std::set<std::uint8_t> sids) {
    if (sids.empty()) sid_filter_.erase(plp);
    else sid_filter_[plp] = std::move(sids);
}

void Decap::event(EventType t, std::uint8_t plp, std::string detail) {
    if (cb_.onEvent) cb_.onEvent(Event{t, plp, std::move(detail)});
}

void Decap::feed(std::uint8_t plp, ByteView alp) {
    ++stats_.packets;
    Header h;
    const ParseStatus st = parseHeader(alp, h);
    if (st != ParseStatus::Ok) {
        ++stats_.invalid;
        event(st == ParseStatus::NeedMore ? EventType::Truncated : EventType::InvalidHeader, plp,
              st == ParseStatus::NeedMore ? "header incomplete" : "invalid header");
        return;
    }
    if (h.totalLength() > alp.size()) {
        ++stats_.truncated;
        event(EventType::Truncated, plp, "payload shorter than declared length");
        return;
    }
    if (h.extension && h.extension->type >= 0xF0) {
        ++stats_.private_extension;
        event(EventType::PrivateExtension, plp, "extension type " + std::to_string(h.extension->type));
    }
    if (h.payload_length == 0 && h.layout != Layout::MpegTs) {  // forbidden by A/330 but seen on air
        ++stats_.zero_length;
        event(EventType::ZeroLength, plp, "zero-length ALP packet");
        return;
    }
    if (h.sid) {
        const auto it = sid_filter_.find(plp);
        if (it != sid_filter_.end() && !it->second.count(*h.sid)) {
            ++stats_.sid_filtered;
            return;
        }
    }
    const ByteView payload = alp.subview(h.header_length, h.payload_length);
    switch (h.layout) {
        case Layout::Single:
        case Layout::MpegTs:
            dispatch(plp, h, payload);
            break;
        case Layout::Concatenation: {
            ++stats_.concatenated;
            std::size_t off = 0;
            const std::size_t n = static_cast<std::size_t>(h.component_count) + 2;
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t len = h.component_lengths[i];
                ++stats_.components;
                dispatch(plp, h, payload.subview(off, len));
                off += len;
            }
            break;
        }
        case Layout::Segment: {
            ++stats_.segments;
            const Key key = (static_cast<Key>(plp) << 16) | (static_cast<Key>(h.type) << 8) |
                            (h.sid ? static_cast<Key>(*h.sid) + 1u : 0u);
            auto it = reassembly_.find(key);
            if (h.segment_sn == 0) {
                if (it != reassembly_.end()) {
                    ++stats_.reassembly_drops;
                    event(EventType::ReassemblyDrop, plp, "new first segment while reassembling");
                    reassembly_.erase(it);
                }
                Reassembly r;
                r.data.assign(payload.begin(), payload.end());
                r.next_sn = 1;
                r.first = h;
                it = reassembly_.emplace(key, std::move(r)).first;
            } else {
                if (it == reassembly_.end() || it->second.next_sn != h.segment_sn) {
                    ++stats_.reassembly_drops;
                    event(EventType::ReassemblyDrop, plp,
                          it == reassembly_.end() ? "segment without first segment" : "segment sequence gap");
                    if (it != reassembly_.end()) reassembly_.erase(it);
                    return;
                }
                it->second.data.insert(it->second.data.end(), payload.begin(), payload.end());
                it->second.next_sn = static_cast<std::uint8_t>(h.segment_sn + 1);
            }
            if (it->second.data.size() > max_reassembly_) {
                ++stats_.reassembly_drops;
                event(EventType::ReassemblyDrop, plp, "reassembly exceeds limit");
                reassembly_.erase(it);
                return;
            }
            if (h.last_segment) {
                ++stats_.reassembled;
                Reassembly done = std::move(it->second);
                reassembly_.erase(it);
                dispatch(plp, done.first, ByteView(done.data.data(), done.data.size()));
            }
            break;
        }
    }
}

void Decap::dispatch(std::uint8_t plp, const Header& h, ByteView payload) {
    switch (h.type) {
        case PacketType::Ipv4:
            ++stats_.ipv4;
            if (cb_.onIpv4) cb_.onIpv4(plp, payload, h.sid);
            break;
        case PacketType::CompressedIp:
            ++stats_.compressed_ip;
            if (cb_.onCompressedIp) cb_.onCompressedIp(plp, payload, h.sid);
            break;
        case PacketType::Signaling:
            ++stats_.signaling;
            if (cb_.onSignaling) cb_.onSignaling(plp, h.signaling, payload);
            break;
        case PacketType::TypeExtension:
            ++stats_.type_extension;
            if (cb_.onTypeExtension) cb_.onTypeExtension(plp, h.extended_type, payload);
            break;
        case PacketType::MpegTs:
            ++stats_.ts;
            deliverTs(plp, h, payload);
            break;
        default:
            ++stats_.reserved_type;
            event(EventType::ReservedType, plp, std::string("packet_type ") + toString(h.type));
            break;
    }
}

// A/330 §5.4: sync byte removal (always), null packet deletion (DNP), TS header deletion (HDM).
void Decap::deliverTs(std::uint8_t plp, const Header& h, ByteView payload) {
    ts_buf_.clear();
    std::size_t deleted_nulls = 0;
    if (h.ahf) deleted_nulls = (h.dnp == 0) ? (h.hdm ? 0u : 128u) : h.dnp;
    for (std::size_t i = 0; i < deleted_nulls; ++i) {
        const std::uint8_t null_hdr[4] = {0x47, 0x1F, 0xFF, 0x10};
        ts_buf_.insert(ts_buf_.end(), null_hdr, null_hdr + 4);
        ts_buf_.insert(ts_buf_.end(), kTsPayload, 0xFF);
    }
    if (!h.hdm) {
        for (std::size_t i = 0; i < h.num_ts; ++i) {
            ts_buf_.push_back(0x47);
            const ByteView pkt = payload.subview(i * kTsNoSync, kTsNoSync);
            ts_buf_.insert(ts_buf_.end(), pkt.begin(), pkt.end());
        }
    } else {
        // first packet carries the 3-byte header; the following ones only 184 bytes of payload,
        // their header is the first header with the continuity counter incremented.
        std::uint8_t hdr[3] = {payload[0], payload[1], payload[2]};
        for (std::size_t i = 0; i < h.num_ts; ++i) {
            ts_buf_.push_back(0x47);
            ts_buf_.insert(ts_buf_.end(), hdr, hdr + 3);
            const std::size_t off = (i == 0) ? 3 : kTsNoSync + (i - 1) * kTsPayload;
            const ByteView body = payload.subview(off, kTsPayload);
            ts_buf_.insert(ts_buf_.end(), body.begin(), body.end());
            hdr[2] = static_cast<std::uint8_t>((hdr[2] & 0xF0) | ((hdr[2] + 1) & 0x0F));
        }
    }
    stats_.ts_packets += ts_buf_.size() / kTsPacket;
    if (cb_.onTs) cb_.onTs(plp, ByteView(ts_buf_.data(), ts_buf_.size()));
}

}  // namespace atsc3::alp
