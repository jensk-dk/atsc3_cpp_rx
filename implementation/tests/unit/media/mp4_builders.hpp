#pragma once
// Test-only ISOBMFF builders for the media layer tests: boxes, sample entries, a minimal moov/moof, codec config
// boxes and an MSB-first bit writer for AudioSpecificConfig / ac4_dsi payloads. Fixture loading from tests/vectors/mmt.
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/media/isobmff.hpp"

namespace atsc3::test::mp4 {

using Bytes = std::vector<std::uint8_t>;

inline ByteView view(const Bytes& b) noexcept { return ByteView(b.data(), b.size()); }
inline std::string vectorPath(const std::string& name) { return std::string(ATSC3_VECTOR_DIR) + "/mmt/" + name; }
inline Bytes loadVector(const std::string& name) {
    std::ifstream f(vectorPath(name), std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
inline Bytes u8(std::uint8_t v) { return Bytes{v}; }
inline Bytes u16(std::uint16_t v) { return Bytes{static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)}; }
inline Bytes u32(std::uint32_t v) {
    Bytes b;
    media::putU32(b, v);
    return b;
}
inline Bytes u64(std::uint64_t v) {
    Bytes b;
    media::putU64(b, v);
    return b;
}
inline Bytes zeros(std::size_t n) { return Bytes(n, 0); }
inline Bytes ascii(std::string_view s) { return Bytes(s.begin(), s.end()); }
inline Bytes fourcc(std::string_view s) {
    Bytes b(4, 0x20);
    for (std::size_t i = 0; i < 4 && i < s.size(); ++i) b[i] = static_cast<std::uint8_t>(s[i]);
    return b;
}
inline Bytes box(std::string_view type, const Bytes& payload) {
    Bytes b;
    media::putBox(b, type, view(payload));
    return b;
}
inline Bytes fullBox(std::string_view type, std::uint8_t version, std::uint32_t flags, const Bytes& payload) {
    return box(type, cat({u8(version), u8(static_cast<std::uint8_t>(flags >> 16)), u8(static_cast<std::uint8_t>(flags >> 8)),
                          u8(static_cast<std::uint8_t>(flags)), payload}));
}
/// A box with the largesize header (14496-12 4.2).
inline Bytes largeBox(std::string_view type, const Bytes& payload) {
    return cat({u32(1), fourcc(type), u64(16 + payload.size()), payload});
}
/// A uuid box: size 'uuid' usertype(16) payload.
inline Bytes uuidBox(const Bytes& usertype, const Bytes& payload) {
    return cat({u32(static_cast<std::uint32_t>(24 + payload.size())), fourcc("uuid"), usertype, payload});
}

/// MSB-first bit writer (AudioSpecificConfig, ac4_dsi).
struct BitWriter {
    Bytes out;
    unsigned nbits = 0;
    void put(std::uint32_t v, unsigned n) {
        for (unsigned i = n; i-- > 0;) {
            if (nbits % 8 == 0) out.push_back(0);
            if ((v >> i) & 1u) out.back() = static_cast<std::uint8_t>(out.back() | (0x80u >> (nbits % 8)));
            ++nbits;
        }
    }
    void align() { nbits = (nbits + 7) & ~7u; }
};

// ---- sample entries (14496-12 12.1.3 / 12.2.3, 14496-30 7.2.1) ----------------------------------------------------

inline Bytes visualEntry(std::string_view type, std::uint16_t width, std::uint16_t height, const Bytes& children) {
    return box(type, cat({zeros(6), u16(1),                          // SampleEntry
                          u16(0), u16(0), zeros(12), u16(width), u16(height), u32(0x00480000), u32(0x00480000), u32(0), u16(1),
                          zeros(32), u16(0x0018), u16(0xffff),      // VisualSampleEntry
                          children}));
}
/// AudioSampleEntry; `qt_extra` appends the QuickTime v1 (16 bytes) / v2 (36 bytes) fields before the children.
inline Bytes audioEntry(std::string_view type, std::uint16_t version, std::uint16_t channels, std::uint32_t rate, const Bytes& qt_extra,
                        const Bytes& children) {
    return box(type, cat({zeros(6), u16(1), u16(version), u16(0), u32(0), u16(channels), u16(16), u16(0), u16(0), u32(rate << 16),
                          qt_extra, children}));
}
inline Bytes stppEntry(const Bytes& children = {}) {
    return box("stpp", cat({zeros(6), u16(1), ascii("http://www.w3.org/ns/ttml"), u8(0), u8(0), u8(0), children}));
}
inline Bytes plainEntry(std::string_view type, const Bytes& children = {}) { return box(type, cat({zeros(6), u16(1), children})); }

// ---- codec configuration boxes ------------------------------------------------------------------------------------

/// hvcC (14496-15 8.3.3.1.2) with the given PTL fields and no NAL arrays.
inline Bytes hvcC(unsigned profile_space, bool tier, unsigned profile_idc, std::uint32_t compat, const Bytes& constraints, std::uint8_t level) {
    const std::uint8_t b1 = static_cast<std::uint8_t>((profile_space << 6) | (tier ? 0x20u : 0u) | (profile_idc & 0x1fu));
    return box("hvcC", cat({u8(1), u8(b1), u32(compat), constraints, u8(level), u16(0xf000), u8(0xfc), u8(0xfd), u8(0xf8), u8(0xf8),
                            u16(0), u8(0x0f), u8(0)}));
}
inline Bytes avcC(std::uint8_t profile, std::uint8_t compat, std::uint8_t level) {
    return box("avcC", cat({u8(1), u8(profile), u8(compat), u8(level), u8(0xff), u8(0xe0), u8(0)}));
}
/// 14496-1 expandable size with `extra` leading 0x80 continuation bytes.
inline Bytes descriptorSize(std::size_t len, int extra = 0) {
    Bytes b;
    for (int i = 0; i < extra; ++i) b.push_back(0x80);
    b.push_back(static_cast<std::uint8_t>(len & 0x7f));
    return b;
}
/// esds with ES_Descriptor{DecoderConfigDescriptor{DecoderSpecificInfo}, SLConfigDescriptor}.
inline Bytes esds(std::uint8_t oti, const Bytes& dsi, bool with_dsi = true, int size_extra = 0) {
    Bytes dsi_desc = with_dsi ? cat({u8(0x05), descriptorSize(dsi.size(), size_extra), dsi}) : Bytes{};
    Bytes dcd_body = cat({u8(oti), u8(0x15), u8(0), u16(0), u32(128000), u32(128000), dsi_desc});
    Bytes dcd = cat({u8(0x04), descriptorSize(dcd_body.size(), size_extra), dcd_body});
    Bytes sl = cat({u8(0x06), descriptorSize(1, size_extra), u8(0x02)});
    Bytes es_body = cat({u16(0), u8(0), dcd, sl});
    Bytes es = cat({u8(0x03), descriptorSize(es_body.size(), size_extra), es_body});
    return fullBox("esds", 0, 0, es);
}
/// AudioSpecificConfig (14496-3 1.6.2.1) with optional backward-compatible SBR/PS signalling.
inline Bytes audioSpecificConfig(std::uint32_t aot, unsigned sfi, unsigned chcfg, bool sync_sbr = false, bool sbr = false, bool sync_ps = false,
                                 bool ps = false) {
    BitWriter w;
    auto putAot = [&](std::uint32_t a) {
        if (a >= 32) {
            w.put(31, 5);
            w.put(a - 32, 6);
        } else {
            w.put(a, 5);
        }
    };
    putAot(aot);
    w.put(sfi, 4);
    w.put(chcfg, 4);
    if (aot == 5 || aot == 29) {
        w.put(3, 4);  // extensionSamplingFrequencyIndex
        w.put(2, 5);  // core object type
    }
    if (aot == 2 || aot == 1 || aot == 3 || aot == 4) {
        w.put(0, 1);  // frameLengthFlag
        w.put(0, 1);  // dependsOnCoreCoder
        w.put(0, 1);  // extensionFlag
    }
    if (sync_sbr) {
        w.put(0x2b7, 11);
        w.put(5, 5);
        w.put(sbr ? 1 : 0, 1);
        if (sbr) {
            w.put(3, 4);
            if (sync_ps) {
                w.put(0x548, 11);
                w.put(ps ? 1 : 0, 1);
            }
        }
    }
    w.align();
    return w.out;
}
/// dac4 = ac4_dsi_v1 (TS 103 190-2 E.5) with one presentation.
inline Bytes dac4(unsigned bitstream_version, unsigned presentation_version, unsigned presentation_config, unsigned mdcompat, bool program_id = false) {
    BitWriter w;
    w.put(1, 3);                  // ac4_dsi_version
    w.put(bitstream_version, 7);
    w.put(1, 1);                  // fs_index 48 kHz
    w.put(3, 4);                  // frame_rate_index
    w.put(1, 9);                  // n_presentations
    if (bitstream_version > 1) {
        w.put(program_id ? 1 : 0, 1);
        if (program_id) {
            w.put(0x1234, 16);
            w.put(1, 1);  // b_uuid
            for (int i = 0; i < 4; ++i) w.put(0xdeadbeef, 32);
        }
    }
    w.put(2, 2);           // bitrate_mode
    w.put(0, 32);          // bitrate
    w.put(0xffffffff, 32); // bitrate_precision
    w.align();
    w.put(presentation_version, 8);
    w.put(2, 8);  // pres_bytes
    w.put(presentation_config, 5);
    if (presentation_config != 6) w.put(mdcompat, 3);
    w.align();
    w.put(0, 8);
    return box("dac4", w.out);
}

// ---- moov / trak / moof ---------------------------------------------------------------------------------------------

struct TrakSpec {
    std::uint32_t track_id = 1;
    std::string handler = "vide";
    std::uint32_t timescale = 90000;
    std::uint16_t language = 0x55c4;  // "und"
    Bytes stsd_entry;
    std::uint8_t version = 0;         // tkhd / mdhd version
};
inline Bytes trak(const TrakSpec& s) {
    const Bytes tkhd = s.version == 1 ? fullBox("tkhd", 1, 7, cat({u64(0), u64(0), u32(s.track_id), u32(0), u64(0), zeros(60)}))
                                      : fullBox("tkhd", 0, 7, cat({u32(0), u32(0), u32(s.track_id), u32(0), u32(0), zeros(60)}));
    const Bytes mdhd = s.version == 1 ? fullBox("mdhd", 1, 0, cat({u64(0), u64(0), u32(s.timescale), u64(0), u16(s.language), u16(0)}))
                                      : fullBox("mdhd", 0, 0, cat({u32(0), u32(0), u32(s.timescale), u32(0), u16(s.language), u16(0)}));
    const Bytes hdlr = fullBox("hdlr", 0, 0, cat({u32(0), fourcc(s.handler), zeros(12), ascii("test"), u8(0)}));
    const Bytes stsd = fullBox("stsd", 0, 0, cat({u32(1), s.stsd_entry}));
    const Bytes stbl = box("stbl", cat({stsd, fullBox("stts", 0, 0, u32(0)), fullBox("stsc", 0, 0, u32(0)), fullBox("stsz", 0, 0, cat({u32(0), u32(0)})),
                                        fullBox("stco", 0, 0, u32(0))}));
    const Bytes minf = box("minf", cat({fullBox("nmhd", 0, 0, {}), box("dinf", fullBox("dref", 0, 0, cat({u32(1), fullBox("url ", 0, 1, {})}))), stbl}));
    return box("trak", cat({tkhd, box("mdia", cat({mdhd, hdlr, minf}))}));
}
inline Bytes trex(std::uint32_t track_id, std::uint32_t duration, std::uint32_t size, std::uint32_t flags) {
    return fullBox("trex", 0, 0, cat({u32(track_id), u32(1), u32(duration), u32(size), u32(flags)}));
}
inline Bytes mvhd(std::uint32_t timescale, std::uint8_t version = 0) {
    return version == 1 ? fullBox("mvhd", 1, 0, cat({u64(0), u64(0), u32(timescale), u64(0), zeros(80)}))
                        : fullBox("mvhd", 0, 0, cat({u32(0), u32(0), u32(timescale), u32(0), zeros(80)}));
}
inline Bytes ftyp(std::string_view major, const std::vector<std::string>& compatible) { return media::ftypBox(major, 0, compatible); }
/// A whole init segment with one track and its trex.
inline Bytes initSegment(const TrakSpec& t, std::uint32_t movie_timescale = 90000) {
    return cat({ftyp("iso6", {"iso6", "dash"}), box("moov", cat({mvhd(movie_timescale), trak(t), box("mvex", trex(t.track_id, 0, 0, 0))}))});
}

struct TrunSampleSpec {
    std::uint32_t duration, size, flags;
    std::int32_t cto;
};
/// tfhd with the given flags and optional fields in the 14496-12 order.
inline Bytes tfhd(std::uint32_t track_id, std::uint32_t flags, std::uint64_t base_data_offset = 0, std::uint32_t sdi = 1, std::uint32_t def_duration = 0,
                  std::uint32_t def_size = 0, std::uint32_t def_flags = 0) {
    Bytes p = u32(track_id);
    if (flags & 0x1) p = cat({p, u64(base_data_offset)});
    if (flags & 0x2) p = cat({p, u32(sdi)});
    if (flags & 0x8) p = cat({p, u32(def_duration)});
    if (flags & 0x10) p = cat({p, u32(def_size)});
    if (flags & 0x20) p = cat({p, u32(def_flags)});
    return fullBox("tfhd", 0, flags, p);
}
inline Bytes tfdt(std::uint8_t version, std::uint64_t t) {
    return version == 1 ? fullBox("tfdt", 1, 0, u64(t)) : fullBox("tfdt", 0, 0, u32(static_cast<std::uint32_t>(t)));
}
inline Bytes trun(std::uint32_t flags, const std::vector<TrunSampleSpec>& samples, std::int32_t data_offset = 0, std::uint32_t first_flags = 0,
                  std::uint8_t version = 0) {
    Bytes p = u32(static_cast<std::uint32_t>(samples.size()));
    if (flags & 0x1) p = cat({p, u32(static_cast<std::uint32_t>(data_offset))});
    if (flags & 0x4) p = cat({p, u32(first_flags)});
    for (const TrunSampleSpec& s : samples) {
        if (flags & 0x100) p = cat({p, u32(s.duration)});
        if (flags & 0x200) p = cat({p, u32(s.size)});
        if (flags & 0x400) p = cat({p, u32(s.flags)});
        if (flags & 0x800) p = cat({p, u32(static_cast<std::uint32_t>(s.cto))});
    }
    return fullBox("trun", version, flags, p);
}
inline Bytes moof(std::uint32_t seq, const std::vector<Bytes>& trafs) {
    Bytes p = fullBox("mfhd", 0, 0, u32(seq));
    for (const Bytes& t : trafs) p = cat({p, t});
    return box("moof", p);
}
inline Bytes mdatHeader(std::uint64_t payload_size, bool large = false) {
    return large ? cat({u32(1), fourcc("mdat"), u64(16 + payload_size)}) : cat({u32(static_cast<std::uint32_t>(8 + payload_size)), fourcc("mdat")});
}

}  // namespace atsc3::test::mp4
