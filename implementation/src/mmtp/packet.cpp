#include "atsc3/mmtp/packet.hpp"

#include "atsc3/io/inet.hpp"

namespace atsc3::mmtp {

const char* toString(PayloadType t) noexcept {
    switch (t) {
        case PayloadType::Mpu: return "MPU";
        case PayloadType::Gfd: return "GFD";
        case PayloadType::Signaling: return "signaling";
        case PayloadType::Repair: return "repair";
        default: return "other";
    }
}

ParseStatus parsePacket(ByteView d, PacketHeader& h) noexcept {
    h = PacketHeader{};
    if (d.size() < 12) return ParseStatus::TooShort;
    h.version = static_cast<std::uint8_t>(d[0] >> 6);
    if (h.version > 1) return ParseStatus::BadVersion;
    h.packet_counter_flag = (d[0] & 0x20) != 0;
    h.fec_type = static_cast<std::uint8_t>((d[0] >> 3) & 0x03);
    h.extension_flag = (d[0] & 0x04) != 0;
    h.flag_a = (d[0] & 0x02) != 0;
    h.flag_b = (d[0] & 0x01) != 0;
    h.raw_type = static_cast<std::uint8_t>(d[1] & 0x3F);
    h.type = h.raw_type <= 3 ? static_cast<PayloadType>(h.raw_type) : PayloadType::Other;
    h.packet_id = io::be16(d.data() + 2);
    h.timestamp = io::be32(d.data() + 4);
    h.packet_sequence_number = io::be32(d.data() + 8);
    std::size_t off = 12;
    if (h.packet_counter_flag) {
        if (d.size() < off + 4) return ParseStatus::TooShort;
        h.packet_counter = io::be32(d.data() + off);
        off += 4;
    }
    if (h.version == 1) {
        if (d.size() < off + 2) return ParseStatus::TooShort;
        h.qos = io::be16(d.data() + off);
        off += 2;
    }
    if (h.extension_flag) {
        if (d.size() < off + 4) return ParseStatus::TooShort;
        const std::size_t len = io::be16(d.data() + off + 2);
        if (d.size() < off + 4 + len) return ParseStatus::TooShort;
        h.extension = d.subview(off + 4, len);
        off += 4 + len;
    }
    h.header_bytes = off;
    h.payload = d.subview(off);
    return ParseStatus::Ok;
}

ParseStatus parseMpuPayload(ByteView p, MpuPayloadHeader& m) noexcept {
    m = MpuPayloadHeader{};
    if (p.size() < 8) return ParseStatus::TooShort;
    m.length = io::be16(p.data());
    m.fragment_type = static_cast<std::uint8_t>(p[2] >> 4);
    m.timed = (p[2] & 0x08) != 0;
    m.fragmentation_indicator = static_cast<std::uint8_t>((p[2] >> 1) & 0x03);
    m.aggregation = (p[2] & 0x01) != 0;
    m.fragment_counter = p[3];
    m.mpu_sequence_number = io::be32(p.data() + 4);
    m.data = p.subview(8);
    if (m.length != p.size() - 2) return ParseStatus::LengthMismatch;
    return ParseStatus::Ok;
}

ParseStatus parseSignalingPayload(ByteView p, SignalingPayloadHeader& s) noexcept {
    s = SignalingPayloadHeader{};
    if (p.size() < 2) return ParseStatus::TooShort;
    s.fragmentation_indicator = static_cast<std::uint8_t>(p[0] >> 6);
    s.long_length = (p[0] & 0x02) != 0;
    s.aggregation = (p[0] & 0x01) != 0;
    s.fragment_counter = p[1];
    s.data = p.subview(2);
    return ParseStatus::Ok;
}

}  // namespace atsc3::mmtp
