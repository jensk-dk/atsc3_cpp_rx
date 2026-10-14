#pragma once
// Private helpers shared by isobmff.cpp and codecs.cpp: bounds-checked big-endian reads, the sample-entry layouts of
// ISO/IEC 14496-12 12.1.3 (VisualSampleEntry) / 12.2.3 (AudioSampleEntry, plus the QuickTime v1/v2 variants) /
// 14496-30 (stpp, wvtt), and the 4CC classes the codecs deriver dispatches on.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "atsc3/media/isobmff.hpp"

namespace atsc3::media::detail {

inline std::uint16_t rd16(ByteView v, std::size_t o) noexcept {
    if (o + 2 > v.size()) return 0;
    return static_cast<std::uint16_t>((static_cast<unsigned>(v[o]) << 8) | v[o + 1]);
}
inline std::uint32_t rd32(ByteView v, std::size_t o) noexcept {
    if (o + 4 > v.size()) return 0;
    return (static_cast<std::uint32_t>(v[o]) << 24) | (static_cast<std::uint32_t>(v[o + 1]) << 16) |
           (static_cast<std::uint32_t>(v[o + 2]) << 8) | v[o + 3];
}
inline std::uint64_t rd64(ByteView v, std::size_t o) noexcept {
    if (o + 8 > v.size()) return 0;
    return (static_cast<std::uint64_t>(rd32(v, o)) << 32) | rd32(v, o + 4);
}
/// 4 bytes as a string (as-is; callers decide what to do with non-printable types).
inline std::string fourcc(ByteView v, std::size_t o) {
    if (o + 4 > v.size()) return {};
    return std::string(reinterpret_cast<const char*>(v.data() + o), 4);
}
/// Absolute offset of a box within the range its ancestors were walked from (the payload view aliases that memory).
inline std::size_t absoluteOffset(const Box& b, ByteView root) noexcept {
    return static_cast<std::size_t>(b.payload.data() - root.data()) - b.header;
}

enum class EntryClass { Visual, Audio, Subtitle, Unknown };

/// Class of a sample entry 4CC (14496-15 HEVC/AVC, 14496-14 mp4a/mp4v, A/342 ac-4, mhm1, 14496-30 stpp/wvtt, CENC enc*).
EntryClass entryClass(std::string_view fourcc) noexcept;

/// The audio fields of an AudioSampleEntry (version 0/1 header, or the QuickTime version 2 double / u32).
struct AudioFields {
    std::uint16_t version = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
};

/// Child boxes of a sample entry: after the 78-byte VisualSampleEntry, the 28/44/64-byte AudioSampleEntry (resolved
/// by probing for a plausible box header), the three NUL-terminated strings of an stpp entry, or 8 bytes otherwise.
std::vector<Box> sampleEntryChildren(const Box& entry, EntryClass cls);
AudioFields audioFields(const Box& entry, std::size_t children_offset) noexcept;
std::size_t audioChildrenOffset(const Box& entry) noexcept;

/// First child of `type` among `children`.
const Box* child(const std::vector<Box>& children, std::string_view type) noexcept;

/// The DecoderSpecificInfo (AudioSpecificConfig for AAC) of an esds payload; empty when absent (codecs.cpp).
ByteView esdsDecoderSpecificInfo(ByteView esds_payload) noexcept;

}  // namespace atsc3::media::detail
