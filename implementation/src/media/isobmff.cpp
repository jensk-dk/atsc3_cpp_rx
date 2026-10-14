// ISOBMFF helpers (spec 5.10.3, 5.17.3/5.17.4): box walking per ISO/IEC 14496-12 4.2 (size / largesize / size 0 /
// uuid), initialization-segment inspection (ftyp, mvhd, tkhd, hdlr, mdhd, stsd, trex: 14496-12 8.2-8.8; mmpu:
// ISO/IEC 23008-1 MPU box), movie-fragment inspection (mfhd, tfhd, tfdt, trun: 14496-12 8.8.5-8.8.12) and the
// MSE-oriented rewrites of MMT MPU metadata / movie fragments of spec 5.10.3: hint track and traf removal, ftyp/styp replacement, trun data_offset repair, tfdt rewrite.
#include "atsc3/media/isobmff.hpp"
#include "media/isobmff_internal.hpp"

namespace atsc3::media {

/// Upper bound on samples per trun honoured by the parser (on air: 94 for 2 s of AAC); larger counts are clamped.
constexpr std::uint32_t kMaxTrunSamples = 1u << 16;
/// Upper bound on the bytes buildFragment synthesises for samples the caller did not supply (zero-fill).
constexpr std::uint64_t kMaxZeroFill = 64ull << 20;

using detail::absoluteOffset;
using detail::rd16;
using detail::rd32;
using detail::rd64;

namespace {

std::string hexOf(ByteView v) {
    static const char* const kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (std::uint8_t b : v) {
        s += kDigits[b >> 4];
        s += kDigits[b & 0xf];
    }
    return s;
}
bool printable(ByteView v) noexcept {
    for (std::uint8_t b : v)
        if (b < 0x20 || b > 0x7e) return false;
    return true;
}
void append(std::vector<std::uint8_t>& out, ByteView v) { out.insert(out.end(), v.begin(), v.end()); }
void put4cc(std::vector<std::uint8_t>& out, std::string_view type) {
    for (std::size_t i = 0; i < 4; ++i) out.push_back(i < type.size() ? static_cast<std::uint8_t>(type[i]) : 0x20);
}
/// The raw bytes of a child found by boxes(parent_payload).
ByteView rawBytes(ByteView parent_payload, const Box& child) noexcept { return parent_payload.subview(child.offset, child.size); }

/// 14496-12 8.4.2.3: packed ISO 639-2/T code, three 5-bit letters + 0x60; valid only without the pad bit and with
/// letters in a..z.
std::string unpackLanguage(std::uint16_t code, bool& valid) {
    valid = (code & 0x8000) == 0;
    std::string s(3, '\0');
    for (int i = 0; i < 3; ++i) {
        const char c = static_cast<char>(((code >> (10 - 5 * i)) & 0x1f) + 0x60);
        if (c < 'a' || c > 'z') valid = false;
        s[static_cast<std::size_t>(i)] = c;
    }
    return valid ? s : "und";
}

}  // namespace

// ---- box walking -------------------------------------------------------------------------------------------------

std::vector<Box> boxes(ByteView data) {
    std::vector<Box> out;
    const std::size_t n = data.size();
    std::size_t pos = 0;
    while (pos + 8 <= n) {
        Box b;
        b.offset = pos;
        std::uint64_t size = rd32(data, pos);
        b.type = detail::fourcc(data, pos + 4);
        b.header = 8;
        if (size == 1) {  // largesize
            if (pos + 16 > n) break;
            size = rd64(data, pos + 8);
            b.header = 16;
        } else if (size == 0) {  // extends to the end of the range
            size = n - pos;
        }
        if (b.type == "uuid") {  // usertype follows the (large)size header
            if (pos + b.header + 16 > n) break;
            b.uuid = hexOf(data.subview(pos + b.header, 16));
            b.header += 16;
        }
        if (size < b.header || size > n - pos) break;  // never trust the header
        b.size = static_cast<std::size_t>(size);
        b.payload = data.subview(pos + b.header, b.size - b.header);
        pos += b.size;
        out.push_back(std::move(b));
    }
    return out;
}

std::optional<Box> findBox(ByteView data, std::string_view type) {
    for (Box& b : boxes(data))
        if (b.type == type) return std::move(b);
    return std::nullopt;
}

std::optional<Box> findPath(ByteView data, std::initializer_list<std::string_view> path) {
    std::optional<Box> cur;
    ByteView range = data;
    for (std::string_view t : path) {
        cur = findBox(range, t);
        if (!cur) return std::nullopt;
        range = cur->payload;
    }
    return cur;
}

// ---- TrackInfo / InitSegmentInfo ---------------------------------------------------------------------------------

std::string TrackInfo::kind() const {
    if (handler == "vide") return "video";
    if (handler == "soun") return "audio";
    if (handler == "subt" || handler == "text" || handler == "sbtl" || handler == "clcp") return "subtitles";
    return "data";
}
std::string TrackInfo::containerMime() const {
    const std::string k = kind();
    return k == "video" ? "video/mp4" : k == "audio" ? "audio/mp4" : "application/mp4";
}
std::string TrackInfo::mimeType() const {
    return codecs.empty() ? containerMime() : containerMime() + "; codecs=\"" + codecs + "\"";
}

const TrackInfo* InitSegmentInfo::mediaTrack() const noexcept {
    const TrackInfo* found = nullptr;
    for (const TrackInfo& t : tracks) {
        if (t.isHint()) continue;
        if (found) return nullptr;
        found = &t;
    }
    return found;
}
const TrackInfo* InitSegmentInfo::track(std::uint32_t track_id) const noexcept {
    for (const TrackInfo& t : tracks)
        if (t.track_id == track_id) return &t;
    return nullptr;
}

// ---- initialization segment --------------------------------------------------------------------------------------

namespace {

void parseFtyp(const Box& b, InitSegmentInfo& out) {
    // 14496-12 4.3: major_brand(4) minor_version(4) compatible_brands[]
    const ByteView p = b.payload;
    if (p.size() < 4) return;
    out.major_brand = detail::fourcc(p, 0);
    for (std::size_t o = 8; o + 4 <= p.size(); o += 4) out.compatible_brands.push_back(detail::fourcc(p, o));
}

void parseMmpu(const Box& b, InitSegmentInfo& out) {
    // ISO/IEC 23008-1 MPUBox (FullBox): is_complete(1) reserved(7) | mpu_sequence_number(32) | asset_id_scheme(32) |
    // asset_id_length(32) | asset_id_value[asset_id_length]
    const ByteView p = b.payload;
    out.has_mmpu = true;
    if (p.size() < 17) {
        out.warnings.push_back("truncated mmpu");
        return;
    }
    out.mmpu_sequence_number = rd32(p, 5);
    const std::uint32_t len = rd32(p, 13);
    const ByteView id = p.subview(17, len);
    if (id.size() < len) out.warnings.push_back("truncated mmpu asset_id");
    out.mmpu_asset_id = printable(id) ? std::string(reinterpret_cast<const char*>(id.data()), id.size()) : hexOf(id);
}

struct TrexDefaults {
    std::uint32_t track_id = 0, duration = 0, size = 0, flags = 0;
};

detail::EntryClass classFromHandler(const std::string& handler) noexcept {
    if (handler == "vide") return detail::EntryClass::Visual;
    if (handler == "soun") return detail::EntryClass::Audio;
    if (handler == "subt" || handler == "text" || handler == "sbtl" || handler == "clcp") return detail::EntryClass::Subtitle;
    return detail::EntryClass::Unknown;
}

void parseSampleEntry(const Box& entry, TrackInfo& t) {
    t.sample_entry = entry.type;
    detail::EntryClass cls = detail::entryClass(entry.type);
    if (cls == detail::EntryClass::Unknown) cls = classFromHandler(t.handler);
    const ByteView p = entry.payload;
    if (cls == detail::EntryClass::Visual && p.size() >= 28) {
        // VisualSampleEntry (14496-12 12.1.3): 8 + pre_defined(2) reserved(2) pre_defined(12) width(2) height(2)
        t.width = rd16(p, 24);
        t.height = rd16(p, 26);
    } else if (cls == detail::EntryClass::Audio && p.size() >= 28) {
        const detail::AudioFields a = detail::audioFields(entry, detail::audioChildrenOffset(entry));
        t.channels = a.channels;
        t.sample_rate = a.sample_rate;
    }
    if (entry.type == "encv" || entry.type == "enca" || entry.type == "enct" || entry.type == "encs") {
        t.encrypted = true;
        const std::vector<Box> children = detail::sampleEntryChildren(entry, cls);
        if (const Box* sinf = detail::child(children, "sinf")) {
            if (auto frma = findBox(sinf->payload, "frma"); frma && frma->payload.size() >= 4)
                t.original_format = detail::fourcc(frma->payload, 0);
        }
    }
    if (t.handler.empty()) {
        t.handler = cls == detail::EntryClass::Visual ? "vide" : cls == detail::EntryClass::Audio ? "soun" : cls == detail::EntryClass::Subtitle ? "subt" : "";
    }
    std::string warning;
    t.codecs = codecsFromSampleEntry(entry, &warning);
    if (!warning.empty()) t.warnings.push_back(warning);
}

void parseTrak(const Box& trak, TrackInfo& t) {
    if (auto tkhd = findBox(trak.payload, "tkhd")) {
        // 14496-12 8.3.2: v0 creation(4) modification(4) track_ID(4); v1 creation(8) modification(8) track_ID(4)
        t.track_id = rd32(tkhd->payload, fullBoxVersion(*tkhd) == 1 ? 20 : 12);
    } else {
        t.warnings.push_back("missing tkhd");
    }
    const std::optional<Box> mdia = findBox(trak.payload, "mdia");
    if (!mdia) {
        t.warnings.push_back("missing mdia");
        return;
    }
    if (auto hdlr = findBox(mdia->payload, "hdlr"); hdlr && hdlr->payload.size() >= 12) {
        t.handler = detail::fourcc(hdlr->payload, 8);  // FullBox(4) pre_defined(4) handler_type(4)
    } else {
        t.warnings.push_back("missing hdlr");
    }
    if (auto mdhd = findBox(mdia->payload, "mdhd")) {
        // 14496-12 8.4.2: v0 creation(4) modification(4) timescale(4) duration(4) language(2);
        // v1 creation(8) modification(8) timescale(4) duration(8) language(2)
        const bool v1 = fullBoxVersion(*mdhd) == 1;
        t.timescale = rd32(mdhd->payload, v1 ? 20 : 12);
        const std::uint16_t lang = rd16(mdhd->payload, v1 ? 32 : 20);
        bool valid = false;
        t.language = unpackLanguage(lang, valid);
        if (!valid) {
            const std::uint8_t raw[2] = {static_cast<std::uint8_t>(lang >> 8), static_cast<std::uint8_t>(lang)};
            t.warnings.push_back("invalid mdhd language 0x" + hexOf(ByteView(raw, 2)));
        }
    } else {
        t.warnings.push_back("missing mdhd");
    }
    const std::optional<Box> stsd = findPath(mdia->payload, {"minf", "stbl", "stsd"});
    if (!stsd || stsd->payload.size() < 8) {
        t.warnings.push_back("missing stsd");
        return;
    }
    // 14496-12 8.5.2: FullBox(4) entry_count(4) then the sample entries; only the first one is used (entry_count is 1
    // on air; a second entry would need stsc / sample_description_index logic fragmented broadcast never uses).
    const std::vector<Box> entries = boxes(stsd->payload.subview(8));
    if (entries.empty() || rd32(stsd->payload, 4) == 0) {
        t.warnings.push_back("missing stsd");
        return;
    }
    parseSampleEntry(entries.front(), t);
}

void parseMoov(const Box& moov, InitSegmentInfo& out) {
    std::vector<TrexDefaults> trex;
    for (const Box& c : boxes(moov.payload)) {
        if (c.type == "mvhd") {
            // 14496-12 8.2.2: v0 creation(4) modification(4) timescale(4); v1 creation(8) modification(8) timescale(4)
            out.movie_timescale = rd32(c.payload, fullBoxVersion(c) == 1 ? 20 : 12);
        } else if (c.type == "trak") {
            TrackInfo t;
            parseTrak(c, t);
            out.tracks.push_back(std::move(t));
        } else if (c.type == "mvex") {
            for (const Box& g : boxes(c.payload)) {
                // 14496-12 8.8.3 trex: FullBox(4) track_ID(4) default_sample_description_index(4)
                // default_sample_duration(4) default_sample_size(4) default_sample_flags(4)
                if (g.type == "trex" && g.payload.size() >= 24)
                    trex.push_back({rd32(g.payload, 4), rd32(g.payload, 12), rd32(g.payload, 16), rd32(g.payload, 20)});
            }
        }
    }
    for (TrackInfo& t : out.tracks) {
        for (const TrexDefaults& d : trex) {
            if (d.track_id != t.track_id) continue;
            t.trex_default_sample_duration = d.duration;
            t.trex_default_sample_size = d.size;
            t.trex_default_sample_flags = d.flags;
            break;
        }
    }
}

}  // namespace

bool parseInitSegment(ByteView data, InitSegmentInfo& out, std::string* error) {
    out = InitSegmentInfo{};
    if (error) error->clear();
    const std::vector<Box> top = boxes(data);
    const Box* moov = nullptr;
    for (const Box& b : top) {
        out.top_level.push_back(b.type);
        if (b.type == "ftyp") parseFtyp(b, out);
        else if (b.type == "mmpu") parseMmpu(b, out);
        else if (b.type == "moov" && !moov) moov = &b;
    }
    if (!moov) {
        if (error) *error = "no moov box";
        return false;
    }
    parseMoov(*moov, out);
    if (out.tracks.empty()) out.warnings.push_back("moov without trak");
    return true;
}

// ---- movie fragment ----------------------------------------------------------------------------------------------

namespace {

struct TfhdDefaults {
    std::optional<std::uint32_t> duration, size, flags;
};

void parseTfhd(const Box& tfhd, TrackFragmentInfo& t, TfhdDefaults& d, std::vector<std::string>& warnings) {
    // 14496-12 8.8.7: FullBox(4) track_ID(4) [base_data_offset(8)] [sample_description_index(4)]
    // [default_sample_duration(4)] [default_sample_size(4)] [default_sample_flags(4)]; flags 0x010000
    // duration-is-empty, 0x020000 default-base-is-moof.
    const ByteView p = tfhd.payload;
    const std::uint32_t flags = fullBoxFlags(tfhd);
    t.tfhd_flags = flags;
    t.track_id = rd32(p, 4);
    t.default_base_is_moof = (flags & 0x020000) != 0;
    std::size_t off = 8;
    if (flags & 0x1) {
        t.base_data_offset = rd64(p, off);
        off += 8;
    }
    if (flags & 0x2) off += 4;  // sample_description_index
    if (flags & 0x8) {
        d.duration = rd32(p, off);
        off += 4;
    }
    if (flags & 0x10) {
        d.size = rd32(p, off);
        off += 4;
    }
    if (flags & 0x20) {
        d.flags = rd32(p, off);
        off += 4;
    }
    if (off > p.size()) warnings.push_back("truncated tfhd of track " + std::to_string(t.track_id));
}

void parseTrun(const Box& trun, ByteView root, TrackFragmentInfo& t, const TfhdDefaults& d, std::vector<std::string>& warnings) {
    // 14496-12 8.8.8: FullBox(4) sample_count(4) [data_offset(s32)] [first_sample_flags(4)] then per sample
    // [duration(4)] [size(4)] [flags(4)] [composition_time_offset(u32 v0 / s32 v1)] per the tr_flags 0x100..0x800.
    const ByteView p = trun.payload;
    const std::uint8_t version = fullBoxVersion(trun);
    const std::uint32_t flags = fullBoxFlags(trun);
    std::uint32_t count = rd32(p, 4);
    std::size_t off = 8;
    if (flags & 0x1) {
        if (t.trun_count == 0) {
            t.data_offset = static_cast<std::int32_t>(rd32(p, off));
            t.data_offset_field_offset = absoluteOffset(trun, root) + trun.header + off;
        }
        off += 4;
    }
    std::optional<std::uint32_t> first_flags;
    if (flags & 0x4) {
        first_flags = rd32(p, off);
        off += 4;
    }
    const std::size_t per_sample = 4 * ((flags & 0x100 ? 1u : 0u) + (flags & 0x200 ? 1u : 0u) + (flags & 0x400 ? 1u : 0u) + (flags & 0x800 ? 1u : 0u));
    if (off > p.size() || (per_sample && (p.size() - off) / per_sample < count)) {
        warnings.push_back("truncated trun of track " + std::to_string(t.track_id));
        count = per_sample && off <= p.size() ? static_cast<std::uint32_t>((p.size() - off) / per_sample) : 0;
    }
    // Without per-sample fields nothing in the box bounds sample_count: a corrupt value must not drive the allocation
    // (one flipped bit asked for 2^32 entries on a 12-byte trun). No movie fragment on air comes near the cap.
    if (count > kMaxTrunSamples) {
        warnings.push_back("implausible trun sample_count " + std::to_string(count) + " of track " + std::to_string(t.track_id));
        count = kMaxTrunSamples;
    }
    ++t.trun_count;
    t.samples.reserve(t.samples.size() + count);
    for (std::uint32_t i = 0; i < count; ++i) {
        TrunSample s;
        if (flags & 0x100) {
            s.duration = rd32(p, off);
            off += 4;
        } else {
            s.duration = d.duration.value_or(0);
        }
        if (flags & 0x200) {
            s.size = rd32(p, off);
            off += 4;
        } else {
            s.size = d.size.value_or(0);
        }
        if (flags & 0x400) {
            s.flags = rd32(p, off);
            off += 4;
        } else {
            s.flags = (i == 0 && first_flags) ? *first_flags : d.flags.value_or(0);
        }
        if (flags & 0x800) {
            const std::uint32_t raw = rd32(p, off);
            s.composition_offset = version == 0 ? static_cast<std::int64_t>(raw) : static_cast<std::int64_t>(static_cast<std::int32_t>(raw));
            off += 4;
        }
        t.total_duration += s.duration;
        t.total_size += s.size;
        t.samples.push_back(s);
    }
}

void parseTraf(const Box& traf, ByteView root, MoofInfo& out) {
    TrackFragmentInfo t;
    t.traf_offset = absoluteOffset(traf, root);
    t.traf_size = traf.size;
    TfhdDefaults d;
    bool tfhd_seen = false;
    for (const Box& c : boxes(traf.payload)) {
        if (c.type == "tfhd") {
            parseTfhd(c, t, d, out.warnings);
            tfhd_seen = true;
        } else if (c.type == "tfdt") {
            // 14496-12 8.8.12: FullBox(4) baseMediaDecodeTime(u32 v0 / u64 v1)
            t.tfdt_version = fullBoxVersion(c);
            t.tfdt = t.tfdt_version == 1 ? rd64(c.payload, 4) : static_cast<std::uint64_t>(rd32(c.payload, 4));
            t.tfdt_field_offset = absoluteOffset(c, root) + c.header + 4;
        } else if (c.type == "trun") {
            parseTrun(c, root, t, d, out.warnings);
        }
    }
    if (!tfhd_seen) out.warnings.push_back("traf without tfhd");
    out.trafs.push_back(std::move(t));
}

}  // namespace

const TrackFragmentInfo* MoofInfo::traf(std::uint32_t track_id) const noexcept {
    for (const TrackFragmentInfo& t : trafs)
        if (t.track_id == track_id) return &t;
    return nullptr;
}

bool parseMoof(ByteView data, MoofInfo& out, std::string* error) {
    out = MoofInfo{};
    if (error) error->clear();
    const std::optional<Box> moof = findBox(data, "moof");
    if (!moof) {
        if (error) *error = "no moof box";
        return false;
    }
    out.moof_offset = moof->offset;
    out.moof_size = moof->size;
    for (const Box& c : boxes(moof->payload)) {
        if (c.type == "mfhd") out.sequence_number = rd32(c.payload, 4);  // 14496-12 8.8.5: FullBox(4) sequence_number(4)
        else if (c.type == "traf") parseTraf(c, data, out);
    }
    // An mdat header right after the moof (the FT=1 DU carries only the 8/16-byte header, spec 5.10.3).
    const std::size_t end = moof->offset + moof->size;
    if (end + 8 <= data.size() && detail::fourcc(data, end + 4) == "mdat") {
        const std::uint32_t s = rd32(data, end);
        if (s == 1) {
            if (end + 16 <= data.size()) {
                out.mdat_size = rd64(data, end + 8);
                out.mdat_header_size = 16;
            }
        } else {
            out.mdat_size = s;
            out.mdat_header_size = 8;
        }
    }
    return true;
}

// ---- writers -----------------------------------------------------------------------------------------------------

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    putU32(out, static_cast<std::uint32_t>(v >> 32));
    putU32(out, static_cast<std::uint32_t>(v));
}
void putBox(std::vector<std::uint8_t>& out, std::string_view type, ByteView payload) {
    const std::uint64_t size = 8 + static_cast<std::uint64_t>(payload.size());
    if (size > 0xffffffffull) {
        putU32(out, 1);
        put4cc(out, type);
        putU64(out, size + 8);
    } else {
        putU32(out, static_cast<std::uint32_t>(size));
        put4cc(out, type);
    }
    append(out, payload);
}
std::vector<std::uint8_t> ftypBox(std::string_view major, std::uint32_t minor, const std::vector<std::string>& compatible) {
    std::vector<std::uint8_t> payload, out;
    put4cc(payload, major);
    putU32(payload, minor);
    for (const std::string& c : compatible) put4cc(payload, c);
    putBox(out, "ftyp", ByteView(payload.data(), payload.size()));
    return out;
}
std::vector<std::uint8_t> stypBox() {
    std::vector<std::uint8_t> payload, out;
    put4cc(payload, "msdh");
    putU32(payload, 0);
    put4cc(payload, "msdh");
    put4cc(payload, "msix");
    putBox(out, "styp", ByteView(payload.data(), payload.size()));
    return out;
}

// ---- cleanInitSegment ----------------------------------------------------------------------------------------------

namespace {

std::uint32_t trakId(const Box& trak) noexcept {
    const std::optional<Box> tkhd = findBox(trak.payload, "tkhd");
    return tkhd ? rd32(tkhd->payload, fullBoxVersion(*tkhd) == 1 ? 20 : 12) : 0;
}

ByteView view(const std::vector<std::uint8_t>& v) noexcept { return ByteView(v.data(), v.size()); }

/// moov without the hint traks (identified by their position in moov order, which is how parseInitSegment lists the
/// tracks), their trex and every trep; mvex and moov sizes recomputed. `kept_traks` receives the number of traks left.
std::vector<std::uint8_t> rebuildMoov(const Box& moov, const std::set<std::uint32_t>& hint_ids, const std::set<std::size_t>& hint_positions,
                                      bool drop_hints, std::vector<std::string>& dropped, std::size_t& kept_traks) {
    std::vector<std::uint8_t> payload;
    std::size_t trak_index = 0;
    kept_traks = 0;
    for (const Box& c : boxes(moov.payload)) {
        if (c.type == "trak") {
            const std::size_t pos = trak_index++;
            if (drop_hints && hint_positions.count(pos)) {
                dropped.push_back("trak#" + std::to_string(trakId(c)));
                continue;
            }
            ++kept_traks;
        }
        if (drop_hints && c.type == "mvex") {
            std::vector<std::uint8_t> mvex;
            for (const Box& g : boxes(c.payload)) {
                const std::uint32_t id = rd32(g.payload, 4);  // trex / trep: FullBox(4) track_ID(4)
                if (g.type == "trex" && hint_ids.count(id)) {
                    dropped.push_back("trex#" + std::to_string(id));
                    continue;
                }
                if (g.type == "trep") {
                    dropped.push_back("trep#" + std::to_string(id));
                    continue;
                }
                append(mvex, rawBytes(c.payload, g));
            }
            putBox(payload, "mvex", view(mvex));
            continue;
        }
        append(payload, rawBytes(moov.payload, c));
    }
    std::vector<std::uint8_t> out;
    putBox(out, "moov", view(payload));
    return out;
}

}  // namespace

bool cleanInitSegment(ByteView data, const CleanInitOptions& opts, CleanInitResult& out, std::string* error) {
    out = CleanInitResult{};
    if (!parseInitSegment(data, out.info, error)) return false;
    std::set<std::uint32_t> hint_ids;
    std::set<std::size_t> hint_positions;
    std::size_t media_tracks = 0;
    for (std::size_t i = 0; i < out.info.tracks.size(); ++i) {
        const TrackInfo& t = out.info.tracks[i];
        if (t.isHint()) {
            hint_ids.insert(t.track_id);
            hint_positions.insert(i);
            if (!out.hint_track_id) {
                out.hint_track_id = t.track_id;
                out.hint_sample_size = t.trex_default_sample_size;
            }
            if (!opts.drop_hint_tracks) ++media_tracks;
        } else {
            ++media_tracks;
        }
    }
    if (media_tracks == 0) {
        if (error) *error = "no media track";
        return false;
    }
    std::vector<std::uint8_t>& o = out.bytes;
    if (opts.rewrite_ftyp) {
        const std::vector<std::uint8_t> ftyp = ftypBox("iso6", 0, {"iso6", "isom", "dash", "cmfc"});
        append(o, view(ftyp));
    }
    bool moov_done = false;
    std::size_t kept_traks = 0;
    for (const Box& b : boxes(data)) {
        if (b.type == "ftyp") {
            if (!opts.rewrite_ftyp) append(o, rawBytes(data, b));
        } else if (b.type == "moov" && !moov_done) {
            const std::vector<std::uint8_t> moov = rebuildMoov(b, hint_ids, hint_positions, opts.drop_hint_tracks, out.dropped, kept_traks);
            append(o, view(moov));
            moov_done = true;
        } else if (opts.keep_only_ftyp_moov) {
            out.dropped.push_back(b.type);
        } else {
            append(o, rawBytes(data, b));
        }
    }
    if (!moov_done || kept_traks == 0) {
        if (error) *error = "no media track in the output";
        out.bytes.clear();
        return false;
    }
    return true;
}

// ---- buildFragment -------------------------------------------------------------------------------------------------

namespace {

std::uint32_t trafTrackId(const Box& traf) noexcept {
    const std::optional<Box> tfhd = findBox(traf.payload, "tfhd");
    return tfhd ? rd32(tfhd->payload, 4) : 0;
}

/// A trun data_offset field in the rebuilt traf payload and the index of its first sample in the traf.
struct TrunPatch {
    std::size_t field_offset;  // within the traf payload being built
    std::size_t first_sample;
};

std::vector<std::uint8_t> tfdtBox(std::uint8_t version, ByteView flags, std::uint64_t value) {
    std::vector<std::uint8_t> payload, out;
    const bool wide = version == 1 || value > 0xffffffffull;  // widen a version-0 box when the value needs 64 bits
    payload.push_back(wide ? 1 : 0);
    for (std::size_t i = 0; i < 3; ++i) payload.push_back(i < flags.size() ? flags[i] : 0);
    if (wide) putU64(payload, value);
    else putU32(payload, static_cast<std::uint32_t>(value));
    putBox(out, "tfdt", view(payload));
    return out;
}

/// The media traf: tfdt rewritten on request, every trun given a data_offset field (patched later).
std::vector<std::uint8_t> rebuildMediaTraf(const Box& traf, const BuildFragmentOptions& opts, std::vector<TrunPatch>& patches) {
    std::vector<std::uint8_t> tp;
    const std::vector<Box> children = boxes(traf.payload);
    bool has_tfdt = false;
    for (const Box& c : children) has_tfdt = has_tfdt || c.type == "tfdt";
    std::size_t sample_cursor = 0;
    for (const Box& c : children) {
        if (c.type == "tfdt" && opts.tfdt) {
            const std::vector<std::uint8_t> box = tfdtBox(fullBoxVersion(c), c.payload.subview(1, 3), *opts.tfdt);
            append(tp, view(box));
            continue;
        }
        if (c.type == "trun") {
            const std::uint32_t flags = fullBoxFlags(c);
            const std::uint32_t count = rd32(c.payload, 4);
            if ((flags & 0x1) && c.payload.size() >= 12) {
                patches.push_back({tp.size() + c.header + 8, sample_cursor});
                append(tp, rawBytes(traf.payload, c));
            } else {  // insert the data_offset field after sample_count and set tr_flag 0x000001 (also repairs a
                      // truncated trun that claims the field without carrying it)
                std::vector<std::uint8_t> payload;
                payload.push_back(fullBoxVersion(c));
                const std::uint32_t nf = flags | 0x1;
                payload.push_back(static_cast<std::uint8_t>(nf >> 16));
                payload.push_back(static_cast<std::uint8_t>(nf >> 8));
                payload.push_back(static_cast<std::uint8_t>(nf));
                putU32(payload, count);
                putU32(payload, 0);
                append(payload, c.payload.subview((flags & 0x1) ? 12 : 8));
                patches.push_back({tp.size() + 8 + 8, sample_cursor});
                putBox(tp, "trun", view(payload));
            }
            sample_cursor += count;
            continue;
        }
        append(tp, rawBytes(traf.payload, c));
        if (c.type == "tfhd" && !has_tfdt && opts.tfdt) {  // no tfdt on the wire: add a version-1 one after the tfhd
            const std::vector<std::uint8_t> box = tfdtBox(1, ByteView(), *opts.tfdt);
            append(tp, view(box));
        }
    }
    return tp;
}

}  // namespace

bool buildFragment(ByteView moof_du, const std::vector<ByteView>& samples, const BuildFragmentOptions& opts, BuildFragmentResult& out,
                   std::string* error) {
    out = BuildFragmentResult{};
    if (!parseMoof(moof_du, out.moof, error)) return false;
    const TrackFragmentInfo* media = nullptr;
    if (opts.media_track_id) {
        media = out.moof.traf(*opts.media_track_id);
    } else {
        for (const TrackFragmentInfo& t : out.moof.trafs) {
            if (opts.drop_track_ids.count(t.track_id)) continue;
            media = &t;
            break;
        }
    }
    if (!media) {
        if (error) *error = "no media traf";
        return false;
    }
    const std::optional<Box> moof = findBox(moof_du, "moof");

    // moof' payload: hint trafs dropped, the media traf rebuilt, everything else verbatim.
    std::vector<std::uint8_t> payload;
    std::vector<TrunPatch> patches;  // offsets within moof' once the traf position is known
    bool media_done = false;
    for (const Box& c : boxes(moof->payload)) {
        if (c.type == "traf") {
            const std::uint32_t id = trafTrackId(c);
            if (opts.drop_track_ids.count(id)) {
                out.dropped.push_back("traf#" + std::to_string(id));
                continue;
            }
            if (id == media->track_id && !media_done) {
                std::vector<TrunPatch> local;
                const std::vector<std::uint8_t> tp = rebuildMediaTraf(c, opts, local);
                const std::size_t traf_payload_start = 8 + payload.size() + 8;  // moof header + position + traf header
                for (const TrunPatch& p : local) patches.push_back({traf_payload_start + p.field_offset, p.first_sample});
                putBox(payload, "traf", view(tp));
                media_done = true;
                continue;
            }
        }
        append(payload, rawBytes(moof->payload, c));
    }
    std::vector<std::uint8_t> moof_bytes;
    putBox(moof_bytes, "moof", view(payload));
    out.moof_size = moof_bytes.size();

    // mdat': the samples in trun order, the missing tail zero-filled with the trun sizes.
    const std::size_t count = media->samples.size();
    std::vector<std::uint64_t> written(count, 0);
    std::uint64_t total = 0, synthesized = 0;
    for (std::size_t i = 0; i < count; ++i) {
        written[i] = i < samples.size() ? samples[i].size() : media->samples[i].size;
        total += written[i];
        if (i >= samples.size()) synthesized += written[i];
    }
    // The zero-fill comes from the received trun sizes, which a corrupt moof can set to anything: never synthesise more
    // than the announced mdat could hold, and never more than kMaxZeroFill.
    std::uint64_t zero_cap = kMaxZeroFill;
    if (out.moof.mdat_size && *out.moof.mdat_size > out.moof.mdat_header_size) zero_cap = std::min<std::uint64_t>(zero_cap, *out.moof.mdat_size - out.moof.mdat_header_size);
    if (synthesized > zero_cap) {
        if (error) *error = "implausible sample sizes: " + std::to_string(synthesized) + " bytes to zero-fill";
        return false;
    }
    const std::size_t mdat_header = (8 + total) > 0xffffffffull ? 16 : 8;
    for (const TrunPatch& p : patches) {
        std::uint64_t before = 0;
        for (std::size_t i = 0; i < p.first_sample && i < count; ++i) before += written[i];
        const std::uint64_t off = static_cast<std::uint64_t>(out.moof_size) + mdat_header + before;
        if (p.field_offset + 4 > moof_bytes.size() || off > 0x7fffffffull) continue;
        const std::uint32_t v = static_cast<std::uint32_t>(off);
        moof_bytes[p.field_offset] = static_cast<std::uint8_t>(v >> 24);
        moof_bytes[p.field_offset + 1] = static_cast<std::uint8_t>(v >> 16);
        moof_bytes[p.field_offset + 2] = static_cast<std::uint8_t>(v >> 8);
        moof_bytes[p.field_offset + 3] = static_cast<std::uint8_t>(v);
    }

    std::vector<std::uint8_t>& o = out.bytes;
    o.reserve((opts.prepend_styp ? 24 : 0) + moof_bytes.size() + mdat_header + static_cast<std::size_t>(total));
    if (opts.prepend_styp) {
        const std::vector<std::uint8_t> styp = stypBox();
        append(o, view(styp));
    }
    append(o, view(moof_bytes));
    if (mdat_header == 16) {
        putU32(o, 1);
        put4cc(o, "mdat");
        putU64(o, 16 + total);
    } else {
        putU32(o, static_cast<std::uint32_t>(8 + total));
        put4cc(o, "mdat");
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i < samples.size()) append(o, samples[i]);
        else o.insert(o.end(), static_cast<std::size_t>(written[i]), 0);
    }
    out.mdat_payload = total;
    return true;
}

// ---- Player-oriented views (spec 5.19.2) --------------------------------------------------------------------------

namespace {
/// Boxes of a sample entry (after its fixed fields), walking into sinf-less CENC entries the same way.
std::vector<Box> entryChildren(const Box& entry, const std::string& handler) {
    detail::EntryClass cls = detail::entryClass(entry.type);
    if (cls == detail::EntryClass::Unknown) cls = classFromHandler(handler);
    return detail::sampleEntryChildren(entry, cls);
}
/// The NUL-terminated namespace string of an stpp XMLSubtitleSampleEntry (14496-30 6.1.2): 8 reserved/index bytes,
/// then namespace, schema_location, auxiliary_mime_types.
std::string stppNamespace(const Box& entry) {
    const ByteView p = entry.payload;
    if (p.size() <= 8) return {};
    std::string ns;
    for (std::size_t i = 8; i < p.size() && p[i] != 0; ++i) ns.push_back(static_cast<char>(p[i]));
    return ns;
}
}  // namespace

bool decoderConfigs(ByteView init, std::vector<DecoderConfig>& out, std::string* error) {
    out.clear();
    InitSegmentInfo info;
    if (!parseInitSegment(init, info, error)) return false;
    const std::optional<Box> moov = findBox(init, "moov");
    if (!moov) {
        if (error) *error = "no moov";
        return false;
    }
    for (const Box& trak : boxes(moov->payload)) {
        if (trak.type != "trak") continue;
        TrackInfo probe;
        parseTrak(trak, probe);
        if (probe.isHint()) continue;
        DecoderConfig dc;
        const TrackInfo* known = info.track(probe.track_id);
        dc.track = known ? *known : probe;
        dc.original_entry = dc.track.encrypted && !dc.track.original_format.empty() ? dc.track.original_format : dc.track.sample_entry;
        const std::optional<Box> mdia = findBox(trak.payload, "mdia");
        const std::optional<Box> stsd = mdia ? findPath(mdia->payload, {"minf", "stbl", "stsd"}) : std::nullopt;
        if (stsd && stsd->payload.size() >= 8) {
            const std::vector<Box> entries = boxes(stsd->payload.subview(8));
            if (!entries.empty()) {
                const Box& entry = entries.front();
                const std::vector<Box> children = entryChildren(entry, dc.track.handler);
                auto take = [&](std::string_view box) {
                    if (const Box* b = detail::child(children, box)) {
                        dc.extradata.assign(b->payload.begin(), b->payload.end());
                        dc.extradata_box = std::string(box);
                        return true;
                    }
                    return false;
                };
                const std::string key = dc.original_entry;
                if (key == "hev1" || key == "hvc1" || key == "dvhe" || key == "dvh1") take("hvcC");
                else if (key == "avc1" || key == "avc3") take("avcC");
                else if (key == "ac-4") take("dac4");
                else if (key == "mp4a") {
                    const Box* esds = detail::child(children, "esds");
                    if (!esds) {
                        if (const Box* wave = detail::child(children, "wave")) {
                            if (auto e = findBox(wave->payload, "esds")) {
                                const ByteView dsi = detail::esdsDecoderSpecificInfo(e->payload);
                                dc.extradata.assign(dsi.begin(), dsi.end());
                                dc.extradata_box = "esds";
                            }
                        }
                    } else {
                        const ByteView dsi = detail::esdsDecoderSpecificInfo(esds->payload);
                        dc.extradata.assign(dsi.begin(), dsi.end());
                        dc.extradata_box = "esds";
                    }
                } else if (key == "stpp") {
                    dc.xml_namespace = stppNamespace(entry);
                } else {
                    take("hvcC") || take("avcC") || take("dac4") || take("dec3") || take("dac3") || take("dOps") || take("av1C") || take("vpcC");
                }
            }
        }
        out.push_back(std::move(dc));
    }
    return true;
}

bool fragmentSamples(ByteView data, std::uint32_t track_id, FragmentSamples& out, std::string* error) {
    out = FragmentSamples{};
    MoofInfo moof;
    if (!parseMoof(data, moof, error)) return false;
    const TrackFragmentInfo* traf = nullptr;
    for (const TrackFragmentInfo& t : moof.trafs) {
        if (track_id == 0 || t.track_id == track_id) {
            traf = &t;
            break;
        }
    }
    if (!traf) {
        if (error) *error = "no traf for track " + std::to_string(track_id);
        return false;
    }
    out.track_id = traf->track_id;
    out.tfdt = traf->tfdt;
    // 14496-12 8.8.7: base = base_data_offset when present, else the moof start (default-base-is-moof, and the first
    // traf without either flag); the first trun's data_offset is relative to that base. Without a data_offset the
    // samples start at the mdat payload.
    const std::uint64_t base = traf->base_data_offset ? *traf->base_data_offset : static_cast<std::uint64_t>(moof.moof_offset);
    std::uint64_t pos = traf->data_offset ? static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + *traf->data_offset)
                                          : static_cast<std::uint64_t>(moof.moof_offset + moof.moof_size + moof.mdat_header_size);
    std::uint64_t dts = traf->tfdt.value_or(0);
    out.samples.reserve(traf->samples.size());
    for (const TrunSample& ts : traf->samples) {
        SampleRef r;
        r.offset = static_cast<std::size_t>(pos);
        r.size = ts.size;
        r.dts = dts;
        r.composition_offset = ts.composition_offset;
        r.duration = ts.duration;
        r.sync = (ts.flags & 0x00010000u) == 0;   // sample_is_non_sync_sample
        if (pos + ts.size <= data.size()) out.samples.push_back(r);
        else ++out.truncated;
        pos += ts.size;
        dts += ts.duration;
        out.duration += ts.duration;
    }
    return true;
}

}  // namespace atsc3::media
