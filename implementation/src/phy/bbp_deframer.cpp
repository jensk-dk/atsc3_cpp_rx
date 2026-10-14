#include "atsc3/alp/header.hpp"
#include "atsc3/phy/bbp.hpp"

namespace atsc3::phy {

BbpStatus parseBbpHeader(ByteView b, BbpHeader& h) noexcept {
    h = BbpHeader{};
    if (b.empty()) return BbpStatus::Invalid;
    h.mode = (b[0] & 0x80) != 0;
    h.pointer = static_cast<std::uint16_t>(b[0] & 0x7F);
    std::size_t off = 1;
    if (h.mode) {
        if (b.size() < 2) return BbpStatus::Invalid;
        h.pointer = static_cast<std::uint16_t>(h.pointer | (static_cast<std::uint16_t>(b[1] >> 2) << 7));
        h.ofi = static_cast<std::uint8_t>(b[1] & 0x03);
        off = 2;
        if (h.ofi == 1) {  // short extension mode: EXT_TYPE(3) EXT_LEN(5)
            if (b.size() < 3) return BbpStatus::Invalid;
            h.ext_type = static_cast<std::uint8_t>(b[2] >> 5);
            h.ext_len = static_cast<std::uint16_t>(b[2] & 0x1F);
            off = 3;
        } else if (h.ofi == 2) {  // long: EXT_TYPE(3) EXT_LEN_LSB(5) EXT_LEN_MSB(8)
            if (b.size() < 4) return BbpStatus::Invalid;
            h.ext_type = static_cast<std::uint8_t>(b[2] >> 5);
            h.ext_len = static_cast<std::uint16_t>((b[2] & 0x1F) | (static_cast<std::uint16_t>(b[3]) << 5));
            off = 4;
        } else if (h.ofi == 3) {  // mixed: NUM_EXT(3) EXT_LEN_LSB(5) EXT_LEN_MSB(8); N headers inside the field
            if (b.size() < 4) return BbpStatus::Invalid;
            h.num_ext = static_cast<std::uint8_t>(b[2] >> 5);
            h.ext_len = static_cast<std::uint16_t>((b[2] & 0x1F) | (static_cast<std::uint16_t>(b[3]) << 5));
            off = 4;
            if (h.ext_len >= 2) h.ext_type = static_cast<std::uint8_t>(b[4] >> 5);
        }
        if (b.size() < off + h.ext_len) return BbpStatus::Invalid;
        if ((h.ofi == 1 || h.ofi == 2) && h.ext_type == 0 && h.ext_len >= 2) {
            h.counter = static_cast<std::uint16_t>((static_cast<unsigned>(b[off]) << 8) | b[off + 1]);
        }
        off += h.ext_len;
    } else if (h.pointer >= 128) {
        return BbpStatus::Invalid;  // cannot happen with 7 bits, kept for clarity
    }
    h.header_length = off;
    if (h.pointer != kPointerNoAlpStart && h.pointer > b.size() - off) return BbpStatus::Invalid;
    return BbpStatus::Ok;
}

BbpDeframer::BbpDeframer(AlpSink sink) : sink_(std::move(sink)) {}

void BbpDeframer::reset() { plps_.clear(); }
void BbpDeframer::resetPlp(std::uint8_t plp) { plps_.erase(plp); }

void BbpDeframer::flush(PlpState& s) {
    if (s.have_pending) ++stats_.flushed;
    s.pending.clear();
    s.have_pending = false;
}

// Parses consecutive ALP packets from [data, data+len); the tail that does not complete becomes pending.
void BbpDeframer::emitFromBuffer(std::uint8_t plp, PlpState& s, const std::uint8_t* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        alp::Header h;
        const alp::ParseStatus st = alp::parseHeader(ByteView(data + off, len - off), h);
        if (st == alp::ParseStatus::Invalid) {
            ++stats_.alp_invalid;
            return;  // drop the rest of this payload; the next pointer resynchronises
        }
        if (st == alp::ParseStatus::NeedMore || h.totalLength() > len - off) {
            s.pending.assign(data + off, data + len);
            s.have_pending = true;
            return;
        }
        ++stats_.alp;
        sink_(plp, ByteView(data + off, h.totalLength()));
        off += h.totalLength();
    }
}

void BbpDeframer::feed(std::uint8_t plp, ByteView bbp, bool error) {
    ++stats_.bbp;
    PlpState& s = plps_[plp];
    if (error) {
        ++stats_.bbp_error;
        flush(s);
        s.need_resync = true;
        return;
    }
    BbpHeader h;
    if (parseBbpHeader(bbp, h) != BbpStatus::Ok) {
        ++stats_.bbp_invalid;
        flush(s);
        s.need_resync = true;
        return;
    }
    if (h.counter) {
        if (s.counter && static_cast<std::uint16_t>(*s.counter + 1) != *h.counter) {
            ++stats_.counter_jumps;
            flush(s);
        }
        s.counter = h.counter;
    }
    const ByteView payload = bbp.subview(h.header_length);
    if (h.pointer == kPointerNoAlpStart) {
        ++stats_.pointer_no_start;
        if (s.have_pending) {
            s.pending.insert(s.pending.end(), payload.begin(), payload.end());
            // a continued packet may complete exactly at the end of this BBP
            alp::Header ah;
            if (alp::parseHeader(ByteView(s.pending.data(), s.pending.size()), ah) == alp::ParseStatus::Ok &&
                ah.totalLength() == s.pending.size()) {
                ++stats_.alp;
                sink_(plp, ByteView(s.pending.data(), s.pending.size()));
                s.pending.clear();
                s.have_pending = false;
            }
        }
        return;
    }
    if (h.pointer == 0) ++stats_.pointer_zero;
    s.need_resync = false;
    // Bytes before the pointer complete the pending packet.
    if (h.pointer > 0) {
        if (s.have_pending) {
            s.pending.insert(s.pending.end(), payload.begin(), payload.begin() + h.pointer);
            alp::Header ah;
            if (alp::parseHeader(ByteView(s.pending.data(), s.pending.size()), ah) == alp::ParseStatus::Ok &&
                ah.totalLength() == s.pending.size()) {
                ++stats_.alp;
                sink_(plp, ByteView(s.pending.data(), s.pending.size()));
            } else {
                ++stats_.pending_mismatch;
            }
            s.pending.clear();
            s.have_pending = false;
        } else {
            ++stats_.orphan_prefix;
        }
    } else if (s.have_pending) {
        ++stats_.pending_mismatch;  // a new packet starts although the previous one was incomplete
        s.pending.clear();
        s.have_pending = false;
    }
    emitFromBuffer(plp, s, payload.data() + h.pointer, payload.size() - h.pointer);
}

}  // namespace atsc3::phy
