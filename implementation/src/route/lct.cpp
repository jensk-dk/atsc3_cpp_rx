#include "atsc3/route/lct.hpp"

#include "atsc3/io/inet.hpp"

namespace atsc3::route {

LctStatus parseLct(ByteView d, LctPacket& out) noexcept {
    out = LctPacket{};
    LctHeader& h = out.header;
    if (d.size() < 4) return LctStatus::TooShort;
    h.version = static_cast<std::uint8_t>(d[0] >> 4);
    if (h.version != 1) return LctStatus::BadVersion;
    h.c = static_cast<std::uint8_t>((d[0] >> 2) & 0x03);
    h.psi = static_cast<std::uint8_t>(d[0] & 0x03);
    h.s = static_cast<std::uint8_t>(d[1] >> 7);
    h.o = static_cast<std::uint8_t>((d[1] >> 5) & 0x03);
    h.h = static_cast<std::uint8_t>((d[1] >> 4) & 0x01);
    h.close_session = (d[1] & 0x02) != 0;
    h.close_object = (d[1] & 0x01) != 0;
    h.hdr_len_words = d[2];
    h.codepoint = d[3];
    const std::size_t hdr_len = static_cast<std::size_t>(h.hdr_len_words) * 4;
    if (hdr_len < 4 || hdr_len > d.size()) return LctStatus::BadHeaderLength;
    std::size_t off = 4;
    const std::size_t cci_len = static_cast<std::size_t>(h.c + 1) * 4;
    const std::size_t tsi_len = static_cast<std::size_t>(h.s) * 4 + static_cast<std::size_t>(h.h) * 2;
    const std::size_t toi_len = static_cast<std::size_t>(h.o) * 4 + static_cast<std::size_t>(h.h) * 2;
    if (off + cci_len + tsi_len + toi_len > hdr_len) return LctStatus::BadHeaderLength;
    auto rd = [&](std::size_t n) {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n && i < 8; ++i) v = (v << 8) | d[off + i];
        off += n;
        return v;
    };
    h.cci = rd(cci_len);
    h.tsi = rd(tsi_len);
    h.toi = rd(toi_len);
    while (off + 4 <= hdr_len) {
        const std::uint8_t het_v = d[off];
        std::size_t ext_len;
        std::size_t data_off;
        if (het_v >= 128) {
            ext_len = 4;
            data_off = off + 1;
        } else {
            const std::size_t hel = d[off + 1];
            if (hel == 0) return LctStatus::BadHeaderLength;
            ext_len = hel * 4;
            data_off = off + 2;
        }
        if (off + ext_len > hdr_len) return LctStatus::BadHeaderLength;
        LctExtension e;
        e.het = het_v;
        e.data = ByteView(d.data() + data_off, off + ext_len - data_off);
        switch (het_v) {
            case het::kTol24:
                h.transfer_length = (static_cast<std::uint64_t>(e.data[0]) << 16) | (static_cast<std::uint64_t>(e.data[1]) << 8) | e.data[2];
                break;
            case het::kTol48:
                if (e.data.size() >= 6) {  // A/331 A.3.8.1: HET, HEL=2, 48-bit Transfer Length
                    std::uint64_t v = 0;
                    for (std::size_t i = 0; i < 6; ++i) v = (v << 8) | e.data[i];
                    h.transfer_length = v;
                }
                break;
            case het::kFti:
                if (e.data.size() >= 14) {  // RFC 5775/5445: TL(48) reserved/instance(16) ESL(16) MSBL(32)
                    FecOti f;
                    for (std::size_t i = 0; i < 6; ++i) f.transfer_length = (f.transfer_length << 8) | e.data[i];
                    f.instance_or_reserved = io::be16(e.data.data() + 6);
                    f.encoding_symbol_length = io::be16(e.data.data() + 8);
                    f.max_source_block_length = io::be32(e.data.data() + 10);
                    h.fti = f;
                    if (!h.transfer_length) h.transfer_length = f.transfer_length;
                }
                break;
            case het::kRoutePresentationTime:
                if (e.data.size() >= 10) h.presentation_time_ntp = (static_cast<std::uint64_t>(io::be32(e.data.data() + 2)) << 32) | io::be32(e.data.data() + 6);
                break;
            case het::kTime: {
                if (e.data.size() >= 2) {
                    const std::uint16_t use = io::be16(e.data.data());
                    std::size_t p = 2;
                    std::uint64_t sct = 0;
                    bool have = false;
                    if ((use & 0x8000) && p + 4 <= e.data.size()) {  // SCT-High
                        sct = static_cast<std::uint64_t>(io::be32(e.data.data() + p)) << 32;
                        p += 4;
                        have = true;
                    }
                    if ((use & 0x4000) && p + 4 <= e.data.size()) {  // SCT-Low
                        sct |= io::be32(e.data.data() + p);
                        have = true;
                    }
                    if (have) h.sender_current_time_ntp = sct;
                }
                break;
            }
            case het::kFdt:
                h.fdt_instance_id = io::be32(d.data() + off) & 0x000FFFFF;  // V(4) FDT Instance ID(20) in the 32-bit word
                break;
            case het::kCenc:
                h.content_encoding_ext = true;
                break;
            default:
                break;
        }
        h.extensions.push_back(e);
        off += ext_len;
    }
    h.header_bytes = hdr_len;
    if (d.size() == hdr_len) {
        out.dataless = true;
        return LctStatus::Ok;
    }
    if (d.size() < hdr_len + 4) return LctStatus::TooShort;
    out.start_offset = io::be32(d.data() + hdr_len);
    out.payload = d.subview(hdr_len + 4);
    return LctStatus::Ok;
}

}  // namespace atsc3::route
