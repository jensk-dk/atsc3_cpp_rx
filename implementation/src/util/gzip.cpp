#include "atsc3/util/gzip.hpp"

#include <zlib.h>

namespace atsc3::util {

bool isGzip(ByteView d) noexcept { return d.size() >= 2 && d[0] == 0x1F && d[1] == 0x8B; }

namespace {
std::optional<std::vector<std::uint8_t>> inflateWith(ByteView data, int window_bits, std::size_t max_out) {
    z_stream zs{};
    if (inflateInit2(&zs, window_bits) != Z_OK) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.resize(data.size() * 4 + 1024);
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    std::size_t produced = 0;
    int rc = Z_OK;
    for (;;) {
        if (produced == out.size()) {
            if (out.size() >= max_out) {
                inflateEnd(&zs);
                return std::nullopt;
            }
            out.resize(std::min(max_out, out.size() * 2));
        }
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);
        rc = inflate(&zs, Z_NO_FLUSH);
        produced = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return std::nullopt;
        }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && zs.avail_out != 0) {
            inflateEnd(&zs);
            return std::nullopt;  // truncated input
        }
    }
    inflateEnd(&zs);
    out.resize(produced);
    return out;
}
}  // namespace

std::optional<std::vector<std::uint8_t>> gunzip(ByteView data, std::size_t max_out) {
    if (data.empty()) return std::nullopt;
    if (isGzip(data)) {
        if (auto r = inflateWith(data, 15 + 16, max_out)) return r;
    }
    if (auto r = inflateWith(data, -15, max_out)) return r;  // raw deflate
    return inflateWith(data, 15, max_out);                    // zlib wrapper
}

std::vector<std::uint8_t> gzipCompress(ByteView data) {
    z_stream zs{};
    std::vector<std::uint8_t> out;
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return out;
    out.resize(deflateBound(&zs, static_cast<uLong>(data.size())) + 32);
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const std::size_t n = out.size() - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(n);
    return out;
}

}  // namespace atsc3::util
