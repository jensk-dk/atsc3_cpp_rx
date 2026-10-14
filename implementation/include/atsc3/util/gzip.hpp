#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::util {

/// True when the bytes start with the gzip magic 1F 8B.
bool isGzip(ByteView data) noexcept;

/// Inflates gzip (RFC 1952) or, failing that, raw deflate (RFC 1951) data. nullopt on error or when
/// the output would exceed max_out.
std::optional<std::vector<std::uint8_t>> gunzip(ByteView data, std::size_t max_out = 16u << 20);

/// Deflates into gzip format (used by tools and tests).
std::vector<std::uint8_t> gzipCompress(ByteView data);

}  // namespace atsc3::util
