#pragma once
// LCT header (RFC 5651) with ROUTE extension headers (A/331 A.3.6–A.3.8, RFC 5775 EXT_FTI).
#include <cstdint>
#include <optional>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::route {

namespace het {
constexpr std::uint8_t kNop = 0;
constexpr std::uint8_t kAuth = 1;
constexpr std::uint8_t kTime = 2;
constexpr std::uint8_t kFti = 64;
constexpr std::uint8_t kRoutePresentationTime = 66;
constexpr std::uint8_t kTol48 = 67;
constexpr std::uint8_t kFdt = 192;
constexpr std::uint8_t kCenc = 193;
constexpr std::uint8_t kTol24 = 194;
}  // namespace het

struct LctExtension {
    std::uint8_t het = 0;
    ByteView data;  // bytes after HET (and HEL for variable-length extensions)
};

struct FecOti {
    std::uint64_t transfer_length = 0;  // 48 bits
    std::uint16_t instance_or_reserved = 0;
    std::uint16_t encoding_symbol_length = 0;
    std::uint32_t max_source_block_length = 0;
};

struct LctHeader {
    std::uint8_t version = 0, c = 0, psi = 0, s = 0, o = 0, h = 0;
    bool close_session = false;  // A
    bool close_object = false;   // B
    std::uint8_t hdr_len_words = 0;
    std::uint8_t codepoint = 0;
    std::uint64_t cci = 0;
    std::uint64_t tsi = 0;
    std::uint64_t toi = 0;
    std::size_t header_bytes = 0;
    std::vector<LctExtension> extensions;
    // decoded extensions
    std::optional<std::uint64_t> transfer_length;          // EXT_TOL (24/48) or EXT_FTI
    std::optional<FecOti> fti;                             // EXT_FTI
    std::optional<std::uint64_t> presentation_time_ntp;    // EXT_ROUTE_PRESENTATION_TIME (NTP64)
    std::optional<std::uint64_t> sender_current_time_ntp;  // EXT_TIME SCT (NTP64, low word 0 if only high)
    std::optional<std::uint32_t> fdt_instance_id;          // EXT_FDT
    bool content_encoding_ext = false;                     // EXT_CENC present
};

struct LctPacket {
    LctHeader header;
    bool dataless = false;          // header only (close session / flags)
    std::uint32_t start_offset = 0; // FEC payload id for source flows (Compact No-Code)
    ByteView payload;               // after the FEC payload id
};

enum class LctStatus { Ok, TooShort, BadVersion, BadHeaderLength };

/// Parses an LCT packet (UDP payload). Source flows only: the 32-bit start_offset follows the header.
LctStatus parseLct(ByteView udp_payload, LctPacket& out) noexcept;

}  // namespace atsc3::route
