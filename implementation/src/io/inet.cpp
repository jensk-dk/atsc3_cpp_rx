#include "atsc3/io/inet.hpp"

#include <cstdio>

namespace atsc3::io {

std::string ipv4ToString(std::uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (a >> 24) & 0xFFu, (a >> 16) & 0xFFu, (a >> 8) & 0xFFu, a & 0xFFu);
    return buf;
}

bool parseIpv4(std::string_view text, std::uint32_t& out) {
    std::uint32_t acc = 0;
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(text[i] - '0');
            if (++digits > 3 || v > 255) return false;
            ++i;
        }
        acc = (acc << 8) | v;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = acc;
    return true;
}

bool parseIpv4Udp(ByteView ip, Ipv4Udp& out) noexcept {
    out = Ipv4Udp{};
    if (ip.size() < 20 || (ip[0] >> 4) != 4) return false;
    const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > ip.size()) return false;
    out.total_len = be16(ip.data() + 2);
    if (out.total_len < ihl) return false;
    const std::size_t total = (out.total_len <= ip.size()) ? out.total_len : ip.size();
    const std::uint16_t frag = be16(ip.data() + 6);
    out.fragment = (frag & 0x3FFFu) != 0;
    out.protocol = ip[9];
    out.src = be32(ip.data() + 12);
    out.dst = be32(ip.data() + 16);
    if (out.protocol == 17 && !out.fragment && total >= ihl + 8) {
        const std::uint8_t* u = ip.data() + ihl;
        out.sport = be16(u);
        out.dport = be16(u + 2);
        const std::uint16_t ulen = be16(u + 4);
        std::size_t plen = total - ihl - 8;
        if (ulen >= 8 && static_cast<std::size_t>(ulen) - 8 < plen) plen = static_cast<std::size_t>(ulen) - 8;
        out.udp = true;
        out.payload = ByteView(u + 8, plen);
    }
    return true;
}

bool parseRtp(ByteView d, Rtp& out) noexcept {
    out = Rtp{};
    if (d.size() < 12) return false;
    out.version = static_cast<std::uint8_t>(d[0] >> 6);
    out.padding = (d[0] & 0x20) != 0;
    out.extension = (d[0] & 0x10) != 0;
    out.cc = static_cast<std::uint8_t>(d[0] & 0x0F);
    out.marker = (d[1] & 0x80) != 0;
    out.pt = static_cast<std::uint8_t>(d[1] & 0x7F);
    out.seq = be16(d.data() + 2);
    out.timestamp = be32(d.data() + 4);
    out.ssrc = be32(d.data() + 8);
    std::size_t off = 12 + static_cast<std::size_t>(out.cc) * 4;
    if (off > d.size()) return false;
    if (out.extension) {
        if (off + 4 > d.size()) return false;
        const std::size_t ext_len = static_cast<std::size_t>(be16(d.data() + off + 2)) * 4;
        if (off + 4 + ext_len > d.size()) return false;
        out.ext = ByteView(d.data() + off + 4, ext_len);
        off += 4 + ext_len;
    }
    std::size_t end = d.size();
    if (out.padding && end > off) {
        const std::size_t pad = d[end - 1];
        if (pad == 0 || pad > end - off) return false;
        end -= pad;
    }
    out.payload = ByteView(d.data() + off, end - off);
    return true;
}

}  // namespace atsc3::io
