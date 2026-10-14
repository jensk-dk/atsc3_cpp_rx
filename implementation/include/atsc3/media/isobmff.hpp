#pragma once
// ISOBMFF helpers for the media paths (spec 5.10.3, 5.17.3/5.17.4): box walking, initialization-segment inspection
// with RFC 6381 codecs strings (ISO/IEC 14496-15 Annex E for HEVC/AVC, 14496-3 for AAC, ETSI TS 103 190-2 Annex E for
// AC-4), movie-fragment inspection, and the rewrites an MSE consumer needs for MMT MPUs: an init without the mmpu box
// and the MMT hint track, a fragment without the hint traf with its trun data_offset repaired and its tfdt rewritten.
// Everything is a pure function of its input bytes; nothing throws; malformed input stops the walk at the first bad
// box header and the parsers return what they could read.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::media {

/// One box as found in a byte range. `size` is the whole box (header included), `header` 8 or 16 (largesize) or
/// 8 + 16 for uuid boxes; `payload` the bytes after the header. A size-0 box extends to the end of the range.
struct Box {
    std::string type;          // 4 characters ("uuid" for extended-type boxes; `uuid` then holds the 16 bytes)
    std::string uuid;          // hex, uuid boxes only
    std::size_t offset = 0;    // from the start of the range given to boxes()
    std::size_t size = 0;
    std::size_t header = 8;
    ByteView payload;
};
/// Boxes of a range (top level of a file, or the payload of a container). Stops at the first malformed header.
std::vector<Box> boxes(ByteView data);
/// First box of `type` in a range (direct children only).
std::optional<Box> findBox(ByteView data, std::string_view type);
/// First box along a path of direct children, e.g. {"moov", "trak", "mdia", "mdhd"}.
std::optional<Box> findPath(ByteView data, std::initializer_list<std::string_view> path);
/// FullBox version and flags (payload must have >= 4 bytes).
inline std::uint8_t fullBoxVersion(const Box& b) noexcept { return b.payload.size() >= 4 ? b.payload[0] : 0; }
inline std::uint32_t fullBoxFlags(const Box& b) noexcept {
    return b.payload.size() >= 4 ? (static_cast<std::uint32_t>(b.payload[1]) << 16) | (static_cast<std::uint32_t>(b.payload[2]) << 8) | b.payload[3] : 0;
}

/// One track of an initialization segment.
struct TrackInfo {
    std::uint32_t track_id = 0;
    std::string handler;               // hdlr handler_type: "vide", "soun", "subt", "text", "sbtl", "hint", "meta", ...
    std::uint32_t timescale = 0;       // mdhd
    std::string language = "und";      // mdhd, ISO 639-2/T; "und" when the packed value is invalid
    std::string sample_entry;          // stsd first entry 4CC ("hev1", "mp4a", "ac-4", "stpp", "encv", ...)
    std::string original_format;       // sinf/frma 4CC for encv/enca, else empty
    bool encrypted = false;            // encv/enca
    std::string codecs;                // RFC 6381 string, empty when not derivable (then `warnings` says why)
    std::optional<std::uint16_t> width, height;        // VisualSampleEntry
    std::optional<std::uint16_t> channels;             // AudioSampleEntry
    std::optional<std::uint32_t> sample_rate;          // AudioSampleEntry (integer part)
    std::optional<std::uint32_t> trex_default_sample_duration, trex_default_sample_size, trex_default_sample_flags;
    std::vector<std::string> warnings;                 // e.g. "missing hvcC", "unknown sample entry xyz"
    bool isHint() const noexcept { return handler == "hint"; }
    /// "video" | "audio" | "subtitles" | "data" from the handler (subt/text/sbtl/clcp -> subtitles).
    std::string kind() const;
    /// MSE container type for the kind: "video/mp4", "audio/mp4", "application/mp4".
    std::string containerMime() const;
    /// Full MSE type: container + "; codecs=\"...\"" (container only when codecs is empty).
    std::string mimeType() const;
};

struct InitSegmentInfo {
    std::vector<std::string> top_level;        // box types in order
    std::string major_brand;
    std::vector<std::string> compatible_brands;
    std::uint32_t movie_timescale = 0;         // mvhd
    std::vector<TrackInfo> tracks;             // in moov order
    bool has_mmpu = false;
    std::optional<std::uint32_t> mmpu_sequence_number;
    std::string mmpu_asset_id;                 // printable asset_id_value (hex when not printable)
    std::vector<std::string> warnings;
    /// The single non-hint track (nullptr when there is none or more than one).
    const TrackInfo* mediaTrack() const noexcept;
    const TrackInfo* track(std::uint32_t track_id) const noexcept;
};
/// Inspects ftyp/moov (and mmpu when present). False only when no moov box is found.
bool parseInitSegment(ByteView data, InitSegmentInfo& out, std::string* error = nullptr);

/// Codecs string from a sample entry box (the stsd entry: e.g. a Box with type "hev1"). Empty when not derivable;
/// `warning` receives the reason. Exposed for tests and for the ROUTE path (codecs from an init vs the MPD).
std::string codecsFromSampleEntry(const Box& entry, std::string* warning = nullptr);
/// True when two codecs strings denote the same codec configuration (numeric comparison of the HEVC fields, case-
/// insensitive elsewhere): "hev1.2.4.L120.B0" == "hev1.2.4.L120.b0", and "hvc1.2.4.L120.B0.0.0" == "hvc1.2.4.L120.B0".
bool sameCodecs(std::string_view a, std::string_view b) noexcept;

/// One sample of a trun, with tfhd defaults applied.
struct TrunSample {
    std::uint32_t duration = 0, size = 0, flags = 0;
    std::int64_t composition_offset = 0;
};
struct TrackFragmentInfo {
    std::uint32_t track_id = 0;
    std::uint32_t tfhd_flags = 0;
    bool default_base_is_moof = false;
    std::optional<std::uint64_t> base_data_offset;
    std::optional<std::uint64_t> tfdt;         // baseMediaDecodeTime
    std::uint8_t tfdt_version = 0;
    std::size_t tfdt_field_offset = 0;         // byte offset (within the moof range) of the baseMediaDecodeTime field, 0 when absent
    std::optional<std::int32_t> data_offset;   // first trun's data_offset
    std::size_t data_offset_field_offset = 0;  // byte offset (within the moof range) of that field, 0 when absent
    std::uint32_t trun_count = 0;
    std::vector<TrunSample> samples;           // all truns concatenated
    std::uint64_t total_duration = 0, total_size = 0;
    std::size_t traf_offset = 0, traf_size = 0;   // the traf box within the moof range
};
struct MoofInfo {
    std::uint32_t sequence_number = 0;         // mfhd
    std::size_t moof_offset = 0, moof_size = 0;
    std::vector<TrackFragmentInfo> trafs;
    std::optional<std::uint64_t> mdat_size;    // announced by an mdat header following the moof in the same range
    std::size_t mdat_header_size = 0;          // 8 or 16, 0 when no mdat header follows
    std::vector<std::string> warnings;
    const TrackFragmentInfo* traf(std::uint32_t track_id) const noexcept;
};
/// Parses the first moof of a range (and the mdat header that follows it, if any). False when there is no moof.
bool parseMoof(ByteView data, MoofInfo& out, std::string* error = nullptr);

// ---- Player-oriented views (spec 5.19.2): decoder configuration per track, samples of a fragment --------------------

/// What a decoder needs for one track of an initialization segment, in addition to the TrackInfo facts: the
/// codec-specific "extradata" as players and libavcodec take it (hvcC/avcC/dac4 payload, the AudioSpecificConfig
/// from the esds), the stpp namespace, and the trex defaults that fragments without explicit sample fields rely on.
struct DecoderConfig {
    TrackInfo track;
    std::string original_entry;            // the sample entry 4CC a decoder should be keyed by ("hev1" for encv/hev1)
    std::vector<std::uint8_t> extradata;
    std::string extradata_box;             // "hvcC", "avcC", "esds" (then extradata is the AudioSpecificConfig), "dac4", ""
    std::string xml_namespace;             // stpp XMLSubtitleSampleEntry namespace(s), space separated
};
/// Decoder configurations of every non-hint track of an initialization segment. False when there is no moov.
bool decoderConfigs(ByteView init, std::vector<DecoderConfig>& out, std::string* error = nullptr);

/// One sample of a movie fragment, with its byte range in the fragment and timing in the track timescale.
struct SampleRef {
    std::size_t offset = 0, size = 0;      // into the range given to fragmentSamples()
    std::uint64_t dts = 0;                 // baseMediaDecodeTime + preceding durations
    std::int64_t composition_offset = 0;
    std::uint32_t duration = 0;
    bool sync = false;                     // !sample_is_non_sync_sample
};
struct FragmentSamples {
    std::uint32_t track_id = 0;
    std::optional<std::uint64_t> tfdt;
    std::vector<SampleRef> samples;
    std::uint64_t duration = 0;            // sum of the sample durations
    std::size_t truncated = 0;             // samples whose bytes lie beyond the range (counted, not returned)
};
/// Samples of the first moof's traf for `track_id` (any traf when 0), with offsets resolved through the tfhd base
/// (default-base-is-moof or base_data_offset) and the first trun's data_offset; data of later truns is taken to
/// follow contiguously. False when there is no moof or no matching traf.
bool fragmentSamples(ByteView data, std::uint32_t track_id, FragmentSamples& out, std::string* error = nullptr);

// ---- MSE-oriented rewrites -------------------------------------------------------------------------------------

struct CleanInitOptions {
    bool rewrite_ftyp = true;                  // -> ftyp(iso6, 0, [iso6, isom, dash, cmfc])
    bool drop_hint_tracks = true;              // hdlr 'hint' trak + its trex + any trep
    bool keep_only_ftyp_moov = true;           // drop mmpu, evti, free and every other top-level box
};
struct CleanInitResult {
    std::vector<std::uint8_t> bytes;
    InitSegmentInfo info;                      // of the INPUT (all tracks, mmpu fields)
    std::optional<std::uint32_t> hint_track_id;
    std::optional<std::uint32_t> hint_sample_size;    // the hint trex default_sample_size (34 on air)
    std::vector<std::string> dropped;          // "mmpu", "trak#2", "trex#2", "trep#1", "evti", ...
};
/// Rebuilds an MPU metadata DU (ftyp mmpu moov ...) as an MSE initialization segment. False when there is no moov or
/// no media track remains.
bool cleanInitSegment(ByteView data, const CleanInitOptions& opts, CleanInitResult& out, std::string* error = nullptr);

struct BuildFragmentOptions {
    std::set<std::uint32_t> drop_track_ids;    // trafs to remove (the hint track)
    std::optional<std::uint32_t> media_track_id;   // the traf whose data_offset is repaired (default: the only remaining traf)
    std::optional<std::uint64_t> tfdt;         // rewrite baseMediaDecodeTime (version 1 field is widened when needed)
    bool prepend_styp = true;                  // styp(msdh, 0, [msdh, msix])
};
struct BuildFragmentResult {
    std::vector<std::uint8_t> bytes;           // [styp] moof' mdat'
    MoofInfo moof;                             // of the INPUT moof
    std::size_t moof_size = 0;                 // of the output moof'
    std::uint64_t mdat_payload = 0;            // bytes of samples written
    std::vector<std::string> dropped;
};
/// Builds an MSE media segment from a received moof (+ optional mdat header in `moof_du`) and the media samples in
/// sample order (one entry per trun sample; the vector may be shorter than the trun's sample_count, in which case the
/// missing tail is zero-filled with the trun sizes). trun.data_offset is set to size(moof') + 8.
bool buildFragment(ByteView moof_du, const std::vector<ByteView>& samples, const BuildFragmentOptions& opts, BuildFragmentResult& out,
                   std::string* error = nullptr);

// ---- small writers used by the rewrites and by tests ----------------------------------------------------------
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v);
void putU64(std::vector<std::uint8_t>& out, std::uint64_t v);
/// Appends a box: size + type + payload.
void putBox(std::vector<std::uint8_t>& out, std::string_view type, ByteView payload);
std::vector<std::uint8_t> ftypBox(std::string_view major, std::uint32_t minor, const std::vector<std::string>& compatible);
std::vector<std::uint8_t> stypBox();   // styp(msdh, 0, [msdh, msix])

}  // namespace atsc3::media
