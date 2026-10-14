// RFC 6381 codecs strings from ISOBMFF sample entries (spec 5.17.3): ISO/IEC 14496-15 Annex E.3 (HEVC) / E.2 (AVC),
// ISO/IEC 14496-3 1.6 AudioSpecificConfig via the 14496-14 esds (AAC), ETSI TS 103 190-2 Annex E (AC-4 dac4),
// ISO/IEC 23008-3 20.6 (MPEG-H mhaC), ETSI TS 102 366 Annex F (ac-3/ec-3), W3C TTML Profile Registry + A/343 (stpp),
// plus the VP9 (vpcC) and AV1 (av1C) strings for test content. The sample-entry layouts are in isobmff_internal.hpp.
#include <cctype>
#include <cstdio>
#include <cstring>

#include "atsc3/media/isobmff.hpp"
#include "media/isobmff_internal.hpp"

namespace atsc3::media {
namespace detail {

EntryClass entryClass(std::string_view f) noexcept {
    static const char* const kVisual[] = {"hvc1", "hev1", "hvt1", "dvh1", "dvhe", "lhv1", "lhe1", "avc1", "avc2", "avc3",
                                          "avc4", "dvav", "dva1", "vp09", "vp08", "av01", "mp4v", "encv", "resv"};
    static const char* const kAudio[] = {"mp4a", "ac-4", "ec-3", "ac-3", "Opus", "fLaC", "mhm1", "mhm2", "mha1", "mha2",
                                         "dtsc", "dtsh", "dtsl", "dtse", "dtsx", "mlpa", "enca", ".mp3", "alac", "samr", "sawb"};
    static const char* const kSubtitle[] = {"stpp", "wvtt", "tx3g", "enct", "mett", "metx", "stxt", "sbtt"};
    for (const char* t : kVisual)
        if (f == t) return EntryClass::Visual;
    for (const char* t : kAudio)
        if (f == t) return EntryClass::Audio;
    for (const char* t : kSubtitle)
        if (f == t) return EntryClass::Subtitle;
    return EntryClass::Unknown;
}

namespace {

/// A box header is plausible at `o` when it fits, its type is printable ASCII and its size is sane (14496-12 4.2).
bool plausibleBoxAt(ByteView p, std::size_t o) noexcept {
    if (o + 8 > p.size()) return false;
    for (std::size_t i = 4; i < 8; ++i)
        if (p[o + i] < 0x20 || p[o + i] > 0x7e) return false;
    const std::uint32_t size = rd32(p, o);
    if (size == 0) return true;
    if (size == 1) return o + 16 <= p.size();
    return size >= 8 && size <= p.size() - o;
}

/// Length of a NUL-terminated string at `o` including the terminator (whole remainder when unterminated).
std::size_t cstrLen(ByteView p, std::size_t o) noexcept {
    std::size_t i = o;
    while (i < p.size() && p[i] != 0) ++i;
    return (i < p.size() ? i + 1 : i) - o;
}

}  // namespace

std::size_t audioChildrenOffset(const Box& entry) noexcept {
    // AudioSampleEntry (14496-12 12.2.3): reserved(6) data_reference_index(2) | version(2) revision(2) vendor(4)
    // channelcount(2) samplesize(2) pre_defined(2) reserved(2) samplerate(4) = 28 bytes. QuickTime sound sample
    // description v1 adds 4 u32 (44 bytes), v2 adds 36 bytes (64 bytes); 14496-12:2015 AudioSampleEntryV1 keeps 28.
    const ByteView p = entry.payload;
    const std::uint16_t version = rd16(p, 8);
    if (version == 1) return plausibleBoxAt(p, 28) ? 28 : plausibleBoxAt(p, 44) ? 44 : 28;
    if (version == 2) return plausibleBoxAt(p, 64) ? 64 : plausibleBoxAt(p, 28) ? 28 : 64;
    return 28;
}

AudioFields audioFields(const Box& entry, std::size_t children_offset) noexcept {
    AudioFields a;
    const ByteView p = entry.payload;
    a.version = rd16(p, 8);
    a.channels = rd16(p, 16);
    a.sample_rate = rd32(p, 24) >> 16;  // 16.16 fixed point, integer part
    if (a.version == 2 && children_offset == 64 && p.size() >= 44) {
        // QuickTime v2: sizeOfStructOnly(4) audioSampleRate(double, 8) numAudioChannels(4) ... at +28
        const std::uint64_t bits = rd64(p, 32);
        double rate = 0;
        std::memcpy(&rate, &bits, sizeof rate);
        if (rate > 0 && rate < 4294967295.0) a.sample_rate = static_cast<std::uint32_t>(rate);
        const std::uint32_t ch = rd32(p, 40);
        if (ch <= 0xffff) a.channels = static_cast<std::uint16_t>(ch);
    }
    return a;
}

std::vector<Box> sampleEntryChildren(const Box& entry, EntryClass cls) {
    const ByteView p = entry.payload;
    std::size_t off = 8;  // SampleEntry: reserved(6) data_reference_index(2)
    switch (cls) {
        case EntryClass::Visual: off = 78; break;  // VisualSampleEntry (14496-12 12.1.3)
        case EntryClass::Audio: off = audioChildrenOffset(entry); break;
        case EntryClass::Subtitle:
            if (entry.type == "stpp" || entry.type == "metx") {
                // XMLSubtitleSampleEntry (14496-30 7.2.1): namespace, schema_location, auxiliary_mime_types
                for (int i = 0; i < 3 && off < p.size(); ++i) off += cstrLen(p, off);
            } else if (entry.type == "mett" || entry.type == "stxt" || entry.type == "sbtt") {
                for (int i = 0; i < 2 && off < p.size(); ++i) off += cstrLen(p, off);  // content_encoding, mime_format
            } else if (entry.type == "tx3g") {
                off = 8 + 30;  // TextSampleEntry fixed part (3GPP TS 26.245)
            }
            break;
        case EntryClass::Unknown: break;
    }
    if (off > p.size()) return {};
    return boxes(p.subview(off));
}

const Box* child(const std::vector<Box>& children, std::string_view type) noexcept {
    for (const Box& b : children)
        if (b.type == type) return &b;
    return nullptr;
}

}  // namespace detail

namespace {

using detail::rd16;
using detail::rd32;

std::string hex2(std::uint32_t v, bool upper) {
    char buf[8];
    if (upper) std::snprintf(buf, sizeof buf, "%02X", v & 0xffu);
    else std::snprintf(buf, sizeof buf, "%02x", v & 0xffu);
    return buf;
}
std::string hexNoLeadingZeros(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", v);
    return buf;
}

/// MSB-first bit reader; reads past the end return 0 and set `overrun`.
struct BitReader {
    ByteView d;
    std::size_t pos = 0;  // in bits
    bool overrun = false;
    std::uint32_t u(unsigned n) noexcept {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            const std::size_t byte = pos >> 3;
            if (byte >= d.size()) {
                overrun = true;
                return 0;
            }
            const unsigned shift = 7u - static_cast<unsigned>(pos & 7);
            v = (v << 1) | ((static_cast<std::uint32_t>(d[byte]) >> shift) & 1u);
            ++pos;
        }
        return v;
    }
    std::size_t bitsLeft() const noexcept { return pos >= d.size() * 8 ? 0 : d.size() * 8 - pos; }
    void byteAlign() noexcept { pos = (pos + 7) & ~static_cast<std::size_t>(7); }
};

// ---- HEVC: 14496-15 8.3.3.1.2 HEVCDecoderConfigurationRecord, string per Annex E.3 ------------------------------
std::string hevcCodecs(const std::string& fourcc, ByteView hvcc, std::string* warning) {
    if (hvcc.size() < 23) {
        if (warning) *warning = "truncated hvcC";
        return {};
    }
    const std::uint8_t b1 = hvcc[1];
    const unsigned profile_space = b1 >> 6;
    const bool tier = (b1 & 0x20) != 0;
    const unsigned profile_idc = b1 & 0x1f;
    const std::uint32_t compat = rd32(hvcc, 2);
    std::uint32_t reversed = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (compat & (1u << i)) reversed |= 1u << (31 - i);
    const std::uint8_t level = hvcc[12];
    std::string s = fourcc + ".";
    if (profile_space) s += static_cast<char>('A' + static_cast<char>(profile_space - 1));
    s += std::to_string(profile_idc) + "." + hexNoLeadingZeros(reversed) + "." + (tier ? "H" : "L") + std::to_string(level);
    std::size_t last = 0;  // constraint bytes up to the last non-zero one
    for (std::size_t i = 0; i < 6; ++i)
        if (hvcc[6 + i] != 0) last = i + 1;
    for (std::size_t i = 0; i < last; ++i) s += "." + hex2(hvcc[6 + i], true);
    return s;
}

// ---- AVC: 14496-15 5.3.3.1.2 AVCDecoderConfigurationRecord, string per Annex E.2 (avc1.PPCCLL) ------------------
std::string avcCodecs(const std::string& fourcc, ByteView avcc, std::string* warning) {
    if (avcc.size() < 4) {
        if (warning) *warning = "truncated avcC";
        return {};
    }
    return fourcc + "." + hex2(avcc[1], true) + hex2(avcc[2], true) + hex2(avcc[3], true);
}

// ---- AAC: 14496-14 esds -> 14496-1 8.3.3 expandable descriptors -> 14496-3 1.6.2.1 AudioSpecificConfig ----------
struct Descriptor {
    std::uint8_t tag = 0;
    ByteView body;
    std::size_t next = 0;  // offset after this descriptor
};
bool readDescriptor(ByteView d, std::size_t off, Descriptor& out) noexcept {
    if (off + 2 > d.size()) return false;
    out.tag = d[off];
    std::size_t len = 0, i = off + 1;
    for (int n = 0; n < 4; ++n) {  // 14496-1 8.3.3: 7 bits per byte, MSB = continuation
        if (i >= d.size()) return false;
        const std::uint8_t b = d[i++];
        len = (len << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    if (i + len > d.size()) len = d.size() - i;  // tolerate a truncated last descriptor
    out.body = d.subview(i, len);
    out.next = i + len;
    return true;
}
const Descriptor* findDescriptor(const std::vector<Descriptor>& v, std::uint8_t tag) noexcept {
    for (const Descriptor& d : v)
        if (d.tag == tag) return &d;
    return nullptr;
}
std::vector<Descriptor> descriptorsIn(ByteView d) {
    std::vector<Descriptor> v;
    std::size_t off = 0;
    Descriptor desc;
    while (off < d.size() && readDescriptor(d, off, desc)) {
        v.push_back(desc);
        off = desc.next;
    }
    return v;
}
std::uint32_t readAot(BitReader& br) noexcept {  // GetAudioObjectType(): 5 bits, escape 31 -> 32 + 6 bits
    std::uint32_t aot = br.u(5);
    if (aot == 31) aot = 32 + br.u(6);
    return aot;
}
void skipSamplingFrequency(BitReader& br) noexcept {
    if (br.u(4) == 15) br.u(24);
}
/// 14496-3 1.6.2.1 AudioSpecificConfig: object type with explicit (hierarchical) or backward-compatible SBR/PS.
std::string aacObjectType(ByteView asc) {
    BitReader br{asc};
    std::uint32_t aot = readAot(br);
    skipSamplingFrequency(br);
    const std::uint32_t chcfg = br.u(4);
    if (br.overrun) return {};
    if (aot == 5 || aot == 29) return std::to_string(aot);  // explicit hierarchical signalling
    const bool ga = aot == 1 || aot == 2 || aot == 3 || aot == 4 || aot == 6 || aot == 7 || aot == 17 || aot == 19 ||
                    aot == 20 || aot == 21 || aot == 22 || aot == 23;
    if (!ga || chcfg == 0) return std::to_string(aot);  // PCE inline or non-GA: do not try to skip the config
    // GASpecificConfig (14496-3 4.4.1)
    br.u(1);                      // frameLengthFlag
    if (br.u(1)) br.u(14);        // dependsOnCoreCoder -> coreCoderDelay
    const std::uint32_t ext = br.u(1);
    if (aot == 6 || aot == 20) br.u(3);  // layerNr
    if (ext) {
        if (aot == 22) {
            br.u(5);   // numOfSubFrame
            br.u(11);  // layer_length
        }
        if (aot == 17 || aot == 19 || aot == 20 || aot == 23) br.u(3);  // resilience flags
        br.u(1);  // extensionFlag3
    }
    if (br.overrun) return std::to_string(aot);
    bool sbr = false, ps = false;
    if (br.bitsLeft() >= 16 && br.u(11) == 0x2b7) {  // backward-compatible extension
        const std::uint32_t ext_aot = readAot(br);
        if (ext_aot == 5) {
            sbr = br.u(1) != 0;
            if (sbr) {
                skipSamplingFrequency(br);
                if (br.bitsLeft() >= 12 && br.u(11) == 0x548) ps = br.u(1) != 0;
            }
        }
    }
    if (br.overrun) return std::to_string(aot);
    if (ps) return "29";
    if (sbr) return "5";
    return std::to_string(aot);
}
std::string aacCodecs(ByteView esds_payload, std::string* warning) {
    if (esds_payload.size() < 4) {
        if (warning) *warning = "truncated esds";
        return {};
    }
    const auto top = descriptorsIn(esds_payload.subview(4));  // after the FullBox version/flags
    const Descriptor* es = findDescriptor(top, 0x03);
    std::vector<Descriptor> inner;
    if (es) {
        // ES_Descriptor: ES_ID(2) flags(1): streamDependenceFlag(0x80) dependsOn_ES_ID(2); URL_Flag(0x40)
        // URLlength(1) + URLstring; OCRstreamFlag(0x20) OCR_ES_Id(2); then the nested descriptors.
        std::size_t off = 3;
        if (es->body.size() < 3) {
            if (warning) *warning = "truncated ES_Descriptor";
            return {};
        }
        const std::uint8_t flags = es->body[2];
        if (flags & 0x80) off += 2;
        if (flags & 0x40) off += 1 + static_cast<std::size_t>(off < es->body.size() ? es->body[off] : 0);
        if (flags & 0x20) off += 2;
        inner = descriptorsIn(es->body.subview(off));
    } else {
        inner = top;  // tolerate an esds that starts directly with the DecoderConfigDescriptor
    }
    const Descriptor* dcd = findDescriptor(inner, 0x04);
    if (!dcd || dcd->body.size() < 13) {
        if (warning) *warning = "missing DecoderConfigDescriptor";
        return {};
    }
    const std::uint8_t oti = dcd->body[0];
    if (oti != 0x40) return "mp4a." + hex2(oti, true);
    const auto dsi = descriptorsIn(dcd->body.subview(13));
    const Descriptor* info = findDescriptor(dsi, 0x05);
    if (!info || info->body.size() < 2) {
        if (warning) *warning = "missing DecoderSpecificInfo";
        return {};
    }
    const std::string aot = aacObjectType(info->body);
    if (aot.empty()) {
        if (warning) *warning = "truncated AudioSpecificConfig";
        return {};
    }
    return "mp4a.40." + aot;
}

/// The nested descriptor list of an esds payload (after the ES_Descriptor header fields, or the top level when the
/// esds starts with the DecoderConfigDescriptor directly).
std::vector<Descriptor> esdsInner(ByteView esds_payload) {
    if (esds_payload.size() < 4) return {};
    const auto top = descriptorsIn(esds_payload.subview(4));
    const Descriptor* es = findDescriptor(top, 0x03);
    if (!es) return top;
    if (es->body.size() < 3) return {};
    std::size_t off = 3;
    const std::uint8_t flags = es->body[2];
    if (flags & 0x80) off += 2;
    if (flags & 0x40) off += 1 + static_cast<std::size_t>(off < es->body.size() ? es->body[off] : 0);
    if (flags & 0x20) off += 2;
    return descriptorsIn(es->body.subview(off));
}
}  // namespace

namespace detail {
ByteView esdsDecoderSpecificInfo(ByteView esds_payload) noexcept {
    const auto inner = esdsInner(esds_payload);
    const Descriptor* dcd = findDescriptor(inner, 0x04);
    if (!dcd || dcd->body.size() < 13) return {};
    const auto dsi = descriptorsIn(dcd->body.subview(13));
    const Descriptor* info = findDescriptor(dsi, 0x05);
    return info ? info->body : ByteView{};
}
}  // namespace detail

namespace {
// ---- AC-4: ETSI TS 103 190-2 Annex E.5 ac4_dsi_v1 (dac4), string per Annex E.13 "ac-4.XX.YY.ZZ" -------------------
std::string ac4Codecs(ByteView dac4, std::string* warning) {
    BitReader br{dac4};
    const std::uint32_t dsi_version = br.u(3);
    const std::uint32_t bitstream_version = br.u(7);
    br.u(1);  // fs_index
    br.u(4);  // frame_rate_index
    const std::uint32_t n_presentations = br.u(9);
    if (dsi_version >= 1) {  // ac4_dsi_v1; the TS 103 190-1 ac4_dsi (version 0) goes straight to the presentations
        if (bitstream_version > 1 && br.u(1)) {  // b_program_id
            br.u(16);                            // short_program_id
            if (br.u(1)) {                       // b_uuid -> program_uuid (128 bits)
                for (int i = 0; i < 4; ++i) br.u(32);
            }
        }
        br.u(2);   // ac4_bitrate_dsi: bitrate_mode
        br.u(32);  // bitrate
        br.u(32);  // bitrate_precision
        br.byteAlign();
    }
    if (br.overrun || n_presentations == 0) {
        if (warning) *warning = br.overrun ? "truncated dac4" : "dac4 without presentations";
        return {};
    }
    // Presentation 0 (A/342-2 5.2.2 orders presentations by preference): presentation_version(8) pres_bytes(8)
    // [+16 when 255], then the presentation dsi whose first fields are presentation_config(_v1)(5) and, unless
    // the config is 6 (EMDF only), mdcompat(3).
    const std::uint32_t presentation_version = br.u(8);
    std::uint32_t pres_bytes = br.u(8);
    if (pres_bytes == 255) pres_bytes += br.u(16);
    const std::uint32_t config = br.u(5);
    if (br.overrun) {
        if (warning) *warning = "truncated dac4";
        return {};
    }
    if (config == 6) {
        if (warning) *warning = "missing mdcompat";
        return {};
    }
    const std::uint32_t mdcompat = br.u(3);
    if (br.overrun) {
        if (warning) *warning = "truncated dac4";
        return {};
    }
    return "ac-4." + hex2(bitstream_version, false) + "." + hex2(presentation_version, false) + "." + hex2(mdcompat, false);
}

// ---- MPEG-H: ISO/IEC 23008-3 20.6 MHADecoderConfigurationRecord (mhaC), string "mhm1.0xPL" (DASH-IF, A/342-3) ----
std::string mpeghCodecs(const std::string& fourcc, ByteView mhac, std::string* warning) {
    if (mhac.size() < 2) {
        if (warning) *warning = "truncated mhaC";
        return {};
    }
    return fourcc + ".0x" + hex2(mhac[1], true);
}

// ---- VP9: vpcC (VP Codec ISO Media File Format Binding) "vp09.PP.LL.DD" -------------------------------------------
std::string vp9Codecs(const std::string& fourcc, ByteView vpcc, std::string* warning) {
    if (vpcc.size() < 7) {  // FullBox(4) profile(1) level(1) bitDepth(4)+chroma(3)+fullRange(1)
        if (warning) *warning = "truncated vpcC";
        return {};
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s.%02u.%02u.%02u", fourcc.c_str(), static_cast<unsigned>(vpcc[4]), static_cast<unsigned>(vpcc[5]), static_cast<unsigned>(vpcc[6] >> 4));
    return buf;
}

// ---- AV1: av1C (AV1 Codec ISO Media File Format Binding 2.3) "av01.P.LLT.DD" ------------------------------------
std::string av1Codecs(const std::string& fourcc, ByteView av1c, std::string* warning) {
    if (av1c.size() < 4) {
        if (warning) *warning = "truncated av1C";
        return {};
    }
    const unsigned profile = av1c[1] >> 5, level = av1c[1] & 0x1f;
    const bool tier = (av1c[2] & 0x80) != 0, high = (av1c[2] & 0x40) != 0, twelve = (av1c[2] & 0x20) != 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s.%u.%02u%c.%02u", fourcc.c_str(), profile, level, tier ? 'H' : 'M', high ? (twelve ? 12u : 10u) : 8u);
    return buf;
}

std::string codecsFor(const std::string& fourcc, const std::vector<Box>& children, std::string* warning) {
    using detail::child;
    auto need = [&](const char* box) -> const Box* {
        const Box* b = child(children, box);
        if (!b && warning) *warning = std::string("missing ") + box;
        return b;
    };
    if (fourcc == "hvc1" || fourcc == "hev1" || fourcc == "hvt1" || fourcc == "dvh1" || fourcc == "dvhe" || fourcc == "lhv1" ||
        fourcc == "lhe1") {
        const Box* b = need("hvcC");
        return b ? hevcCodecs(fourcc, b->payload, warning) : std::string{};
    }
    if (fourcc == "avc1" || fourcc == "avc2" || fourcc == "avc3" || fourcc == "avc4" || fourcc == "dvav" || fourcc == "dva1") {
        const Box* b = need("avcC");
        return b ? avcCodecs(fourcc, b->payload, warning) : std::string{};
    }
    if (fourcc == "mp4a") {
        const Box* b = child(children, "esds");
        if (!b) {  // QuickTime wraps the esds in a 'wave' child: wave{frma, mp4a?, esds, terminator}
            if (const Box* wave = child(children, "wave")) {
                if (auto e = findBox(wave->payload, "esds")) return aacCodecs(e->payload, warning);
            }
            if (warning) *warning = "missing esds";
            return {};
        }
        return aacCodecs(b->payload, warning);
    }
    if (fourcc == "ac-4") {
        const Box* b = need("dac4");
        return b ? ac4Codecs(b->payload, warning) : std::string{};
    }
    if (fourcc == "ec-3" || fourcc == "ac-3" || fourcc == "wvtt" || fourcc == "tx3g" || fourcc == "mlpa" || fourcc == "dtsc" ||
        fourcc == "dtsh" || fourcc == "dtsl" || fourcc == "dtse" || fourcc == "dtsx") {
        return fourcc;  // registered as the bare 4CC (ETSI TS 102 366 Annex F, 14496-30, ...)
    }
    if (fourcc == "Opus") return "opus";
    if (fourcc == "fLaC") return "flac";
    if (fourcc == "mhm1" || fourcc == "mhm2" || fourcc == "mha1" || fourcc == "mha2") {
        const Box* b = need("mhaC");
        return b ? mpeghCodecs(fourcc, b->payload, warning) : std::string{};
    }
    if (fourcc == "stpp") return "stpp.ttml.im1t";  // profile not derivable from the init: ATSC default (A/343 5)
    if (fourcc == "vp09" || fourcc == "vp08") {
        const Box* b = need("vpcC");
        return b ? vp9Codecs(fourcc, b->payload, warning) : std::string{};
    }
    if (fourcc == "av01") {
        const Box* b = need("av1C");
        return b ? av1Codecs(fourcc, b->payload, warning) : std::string{};
    }
    if (warning) *warning = "unknown sample entry " + fourcc;
    return {};
}

}  // namespace

std::string codecsFromSampleEntry(const Box& entry, std::string* warning) {
    if (warning) warning->clear();
    const detail::EntryClass cls = detail::entryClass(entry.type);
    const std::vector<Box> children = detail::sampleEntryChildren(entry, cls);
    std::string fourcc = entry.type;
    if (entry.type == "encv" || entry.type == "enca" || entry.type == "enct" || entry.type == "encs") {
        // CENC (23001-7): the original format is in sinf/frma; the codec config box stays a direct child.
        const Box* sinf = detail::child(children, "sinf");
        const std::optional<Box> frma = sinf ? findBox(sinf->payload, "frma") : std::nullopt;
        if (!frma || frma->payload.size() < 4) {
            if (warning) *warning = "missing sinf/frma";
            return {};
        }
        fourcc = detail::fourcc(frma->payload, 0);
    }
    return codecsFor(fourcc, children, warning);
}

namespace {

std::vector<std::string> splitDots(std::string_view s) {
    std::vector<std::string> v;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = s.find('.', start);
        v.emplace_back(s.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return v;
}
bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
bool parseNumber(std::string_view s, int base, std::uint64_t& out) noexcept {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return false;
        if (d >= base) return false;
        v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    }
    out = v;
    return true;
}
/// Two fields are equal when they match case-insensitively or denote the same number (hex when either says so).
bool sameField(std::string_view a, std::string_view b) noexcept {
    if (iequals(a, b)) return true;
    auto strip0x = [](std::string_view s, bool& hex) {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            hex = true;
            return s.substr(2);
        }
        return s;
    };
    bool hex = false;
    const std::string_view fa = strip0x(a, hex), fb = strip0x(b, hex);
    for (char c : fa)
        if (std::isxdigit(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c))) hex = true;
    for (char c : fb)
        if (std::isxdigit(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c))) hex = true;
    std::uint64_t va = 0, vb = 0;
    return parseNumber(fa, hex ? 16 : 10, va) && parseNumber(fb, hex ? 16 : 10, vb) && va == vb;
}
bool isHevc(std::string_view fourcc) noexcept {
    return iequals(fourcc, "hvc1") || iequals(fourcc, "hev1") || iequals(fourcc, "hvt1") || iequals(fourcc, "dvh1") ||
           iequals(fourcc, "dvhe") || iequals(fourcc, "lhv1") || iequals(fourcc, "lhe1");
}

}  // namespace

bool sameCodecs(std::string_view a, std::string_view b) noexcept {
    const std::vector<std::string> fa = splitDots(a), fb = splitDots(b);
    if (fa.empty() || fb.empty() || !iequals(fa[0], fb[0])) return false;
    if (isHevc(fa[0])) {
        // 14496-15 Annex E.3: profile (space letter + idc), compat flags (hex), tier+level, constraint bytes (hex,
        // trailing zero bytes optional).
        if (fa.size() < 4 || fb.size() < 4) {  // incomplete strings: plain field comparison
            if (fa.size() != fb.size()) return false;
            for (std::size_t i = 1; i < fa.size(); ++i)
                if (!sameField(fa[i], fb[i])) return false;
            return true;
        }
        auto letterNumber = [](std::string_view s, std::string_view& letter, std::string_view& number) {
            std::size_t i = 0;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            letter = s.substr(0, i);
            number = s.substr(i);
        };
        std::string_view la, na, lb, nb;
        letterNumber(fa[1], la, na);
        letterNumber(fb[1], lb, nb);
        std::uint64_t va = 0, vb = 0;
        if (!iequals(la, lb) || !parseNumber(na, 10, va) || !parseNumber(nb, 10, vb) || va != vb) return false;
        if (!parseNumber(fa[2], 16, va) || !parseNumber(fb[2], 16, vb) || va != vb) return false;
        letterNumber(fa[3], la, na);
        letterNumber(fb[3], lb, nb);
        if (!iequals(la, lb) || !parseNumber(na, 10, va) || !parseNumber(nb, 10, vb) || va != vb) return false;
        std::size_t ea = fa.size(), eb = fb.size();
        auto zero = [](const std::string& s) {
            std::uint64_t v = 1;
            return parseNumber(s, 16, v) && v == 0;
        };
        while (ea > 4 && zero(fa[ea - 1])) --ea;
        while (eb > 4 && zero(fb[eb - 1])) --eb;
        if (ea != eb) return false;
        for (std::size_t i = 4; i < ea; ++i)
            if (!parseNumber(fa[i], 16, va) || !parseNumber(fb[i], 16, vb) || va != vb) return false;
        return true;
    }
    if (fa.size() != fb.size()) return false;
    for (std::size_t i = 1; i < fa.size(); ++i) {
        if (sameField(fa[i], fb[i])) continue;
        // The on-air typo of the IMSC1 text profile (A/343 5): "imt1" for "im1t".
        if (iequals(fa[0], "stpp") && ((iequals(fa[i], "imt1") && iequals(fb[i], "im1t")) || (iequals(fa[i], "im1t") && iequals(fb[i], "imt1"))))
            continue;
        return false;
    }
    return true;
}

}  // namespace atsc3::media
