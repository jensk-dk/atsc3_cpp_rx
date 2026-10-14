#include "link_strip.hpp"

#include <string>

#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"

namespace atsc3::pcap {

namespace {
bool ipv4At(ByteView frame, std::size_t off, ByteView& ipv4) noexcept {
    if (frame.size() < off + 20 || (frame[off] >> 4) != 4) return false;
    ipv4 = frame.subview(off);
    return true;
}
}  // namespace

bool stripToIpv4(std::uint16_t lt, ByteView frame, ByteView& ipv4) noexcept {
    switch (lt) {
        case linktype::kEthernet: {
            std::size_t off = 14;
            if (frame.size() < off) return false;
            std::uint16_t et = io::be16(frame.data() + 12);
            while ((et == 0x8100 || et == 0x88A8 || et == 0x9100) && frame.size() >= off + 4) {
                et = io::be16(frame.data() + off + 2);
                off += 4;
            }
            if (et != 0x0800) return false;
            return ipv4At(frame, off, ipv4);
        }
        case linktype::kLinuxSll: {
            if (frame.size() < 16 || io::be16(frame.data() + 14) != 0x0800) return false;
            return ipv4At(frame, 16, ipv4);
        }
        case linktype::kLinuxSll2: {
            if (frame.size() < 20 || io::be16(frame.data()) != 0x0800) return false;
            return ipv4At(frame, 20, ipv4);
        }
        case linktype::kRawBsd:
        case linktype::kRawIp:
        case linktype::kIpv4:
            return ipv4At(frame, 0, ipv4);
        default:
            return false;
    }
}

bool extractMarkerText(ByteView ipv4, std::string& text) {
    io::Ipv4Udp h;
    if (!io::parseIpv4Udp(ipv4, h) || h.protocol != 1) return false;
    const std::size_t ihl = static_cast<std::size_t>(ipv4[0] & 0x0F) * 4;
    if (ipv4.size() < ihl + 8) return false;
    const ByteView body = ipv4.subview(ihl + 8);
    std::string s;
    for (std::uint8_t c : body) {
        if (c == 0) break;
        if (c < 0x20 && c != '\n') return false;
        if (c > 0x7E) return false;
        s.push_back(static_cast<char>(c));
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.find('=') == std::string::npos) return false;
    text = s;
    return true;
}

}  // namespace atsc3::pcap
