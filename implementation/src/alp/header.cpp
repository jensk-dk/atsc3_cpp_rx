#include "atsc3/alp/header.hpp"

namespace atsc3::alp {

const char* toString(PacketType t) noexcept {
    switch (t) {
        case PacketType::Ipv4: return "IPv4";
        case PacketType::CompressedIp: return "compressed-IP";
        case PacketType::Signaling: return "signaling";
        case PacketType::TypeExtension: return "type-extension";
        case PacketType::MpegTs: return "MPEG-2 TS";
        default: return "reserved";
    }
}

ParseStatus parseHeader(ByteView d, Header& h) noexcept {
    h = Header{};
    if (d.empty()) return ParseStatus::NeedMore;
    h.type = static_cast<PacketType>(d[0] >> 5);

    if (h.type == PacketType::MpegTs) {  // A/330 Table 5.17
        h.layout = Layout::MpegTs;
        h.num_ts = static_cast<std::uint8_t>((d[0] >> 1) & 0x0F);
        if (h.num_ts == 0) h.num_ts = 16;
        h.ahf = (d[0] & 0x01) != 0;
        h.header_length = h.ahf ? 2 : 1;
        if (d.size() < h.header_length) return ParseStatus::NeedMore;
        if (h.ahf) {
            h.hdm = (d[1] & 0x80) != 0;
            h.dnp = static_cast<std::uint8_t>(d[1] & 0x7F);
        }
        h.payload_length = h.hdm ? 187u + static_cast<std::size_t>(h.num_ts - 1) * 184u
                                 : static_cast<std::size_t>(h.num_ts) * 187u;
        return ParseStatus::Ok;
    }

    if (d.size() < 2) return ParseStatus::NeedMore;
    const bool pc = (d[0] & 0x10) != 0;
    const bool hs = (d[0] & 0x08) != 0;  // HM (pc=0) or S/C (pc=1)
    std::size_t length = (static_cast<std::size_t>(d[0] & 0x07) << 8) | d[1];
    std::size_t off = 2;
    bool sif = false, hef = false;

    if (!pc) {
        h.layout = Layout::Single;
        h.header_mode = hs;
        if (hs) {  // Table 5.4
            if (d.size() < 3) return ParseStatus::NeedMore;
            length |= static_cast<std::size_t>(d[2] >> 3) << 11;
            sif = (d[2] & 0x02) != 0;
            hef = (d[2] & 0x01) != 0;
            off = 3;
        }
    } else if (!hs) {  // Table 5.5 segmentation
        h.layout = Layout::Segment;
        if (d.size() < 3) return ParseStatus::NeedMore;
        h.segment_sn = static_cast<std::uint8_t>(d[2] >> 3);
        h.last_segment = (d[2] & 0x04) != 0;
        sif = (d[2] & 0x02) != 0;
        hef = (d[2] & 0x01) != 0;
        off = 3;
        if (h.segment_sn == 0 && h.last_segment) return ParseStatus::Invalid;
    } else {  // Table 5.6 concatenation
        h.layout = Layout::Concatenation;
        if (d.size() < 3) return ParseStatus::NeedMore;
        length |= static_cast<std::size_t>(d[2] >> 4) << 11;
        h.component_count = static_cast<std::uint8_t>((d[2] >> 1) & 0x07);
        sif = (d[2] & 0x01) != 0;
        off = 3;
        const std::size_t n = static_cast<std::size_t>(h.component_count) + 1;  // explicit lengths
        const std::size_t nbytes = (n * 12 + 7) / 8;                             // includes 4 stuffing bits when n is odd
        if (d.size() < off + nbytes) return ParseStatus::NeedMore;
        std::size_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bit = i * 12;
            const std::size_t byte = off + bit / 8;
            std::uint16_t v;
            if ((bit % 8) == 0) {
                v = static_cast<std::uint16_t>((static_cast<unsigned>(d[byte]) << 4) | (d[byte + 1] >> 4));
            } else {
                v = static_cast<std::uint16_t>(((static_cast<unsigned>(d[byte]) & 0x0F) << 8) | d[byte + 1]);
            }
            if (v == 0) return ParseStatus::Invalid;
            h.component_lengths[i] = v;
            sum += v;
        }
        off += nbytes;
        if (sum >= length) return ParseStatus::Invalid;  // the implied last component must be > 0
        h.component_lengths[n] = static_cast<std::uint16_t>(length - sum);
    }

    if (h.type == PacketType::Signaling) {  // Table 5.11
        if (d.size() < off + 5) return ParseStatus::NeedMore;
        h.signaling.type = d[off];
        h.signaling.type_extension = static_cast<std::uint16_t>((static_cast<unsigned>(d[off + 1]) << 8) | d[off + 2]);
        h.signaling.version = d[off + 3];
        h.signaling.format = static_cast<std::uint8_t>(d[off + 4] >> 6);
        h.signaling.encoding = static_cast<std::uint8_t>((d[off + 4] >> 4) & 0x03);
        off += 5;
    } else if (h.type == PacketType::TypeExtension) {  // Table 5.15
        if (d.size() < off + 2) return ParseStatus::NeedMore;
        h.extended_type = static_cast<std::uint16_t>((static_cast<unsigned>(d[off]) << 8) | d[off + 1]);
        off += 2;
    }
    if (sif) {  // Table 5.8
        if (d.size() < off + 1) return ParseStatus::NeedMore;
        h.sid = d[off];
        off += 1;
    }
    if (hef) {  // Table 5.9
        if (d.size() < off + 2) return ParseStatus::NeedMore;
        const std::size_t ext_len = static_cast<std::size_t>(d[off + 1]) + 1;
        if (d.size() < off + 2 + ext_len) return ParseStatus::NeedMore;
        HeaderExtension ext;
        ext.type = d[off];
        ext.bytes = ByteView(d.data() + off + 2, ext_len);
        h.extension = ext;
        off += 2 + ext_len;
    }
    h.header_length = off;
    h.payload_length = length;
    return ParseStatus::Ok;
}

}  // namespace atsc3::alp
