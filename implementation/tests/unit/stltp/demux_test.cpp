#include <catch2/catch_test_macros.hpp>

#include "atsc3/io/inet.hpp"
#include "atsc3/stltp/demux.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
struct Collector {
    std::vector<std::pair<std::uint8_t, Bytes>> bbps;
    std::vector<std::pair<std::uint8_t, Bytes>> alps;
    std::size_t preambles = 0, tmps = 0;
    stltp::Callbacks callbacks() {
        stltp::Callbacks cb;
        cb.onBbp = [this](std::uint8_t plp, ByteView b) { bbps.emplace_back(plp, Bytes(b.begin(), b.end())); };
        cb.onAlp = [this](std::uint8_t plp, std::uint8_t, ByteView b) { alps.emplace_back(plp, Bytes(b.begin(), b.end())); };
        cb.onPreamble = [this](ByteView) { ++preambles; };
        cb.onTmp = [this](ByteView) { ++tmps; };
        return cb;
    }
};
void feedAll(stltp::Demux& d, const std::vector<Bytes>& tunnel) {
    for (const Bytes& t : tunnel) d.feedTunnelPacket(ByteView(t.data(), t.size()));
}
}  // namespace

TEST_CASE("BBPs straddling tunnel packets are reassembled per PLP", "[stltp]") {
    Collector c;
    stltp::Demux d(c.callbacks());
    std::uint16_t inner_seq = 7, outer_seq = 0x3880;
    std::vector<Bytes> inner;
    Bytes bbp0 = basebandPacket(0, Bytes(5000, 0xA5), 5916);
    Bytes bbp1 = basebandPacket(8191, Bytes(3000, 0x5A), 5916);
    Bytes bbp2 = basebandPacket(3, Bytes(10, 0x11), 100);
    for (const Bytes& p : bppsPackets(2, bbp0, inner_seq, 1)) inner.push_back(p);
    // Preamble and T&M in between
    inner.push_back(ipv4Udp(addr(1, 2, 3, 4), addr(239, 0, 51, 48), 1, 30064, rtp(77, true, 1, 1, 0, Bytes(80, 1))));
    inner.push_back(ipv4Udp(addr(1, 2, 3, 4), addr(239, 0, 51, 48), 1, 30065, rtp(76, true, 1, 1, 0, Bytes(60, 2))));
    for (const Bytes& p : bppsPackets(1, bbp1, inner_seq, 1)) inner.push_back(p);
    for (const Bytes& p : bppsPackets(0, bbp2, inner_seq, 1)) inner.push_back(p);
    const auto tunnel = stltpTunnel(inner, outer_seq, 1360, 0);
    REQUIRE(tunnel.size() > 5);
    feedAll(d, tunnel);
    REQUIRE(c.bbps.size() == 3);
    CHECK(c.bbps[0].first == 2);
    CHECK(c.bbps[0].second == bbp0);
    CHECK(c.bbps[1].first == 1);
    CHECK(c.bbps[1].second == bbp1);
    CHECK(c.bbps[2].first == 0);
    CHECK(c.bbps[2].second == bbp2);
    CHECK(c.preambles == 1);
    CHECK(c.tmps == 1);
    CHECK(d.stats().outer_seq_gaps == 0);
    CHECK(d.stats().protocol_version == 0);
    CHECK(d.stats().inner_packets == inner.size());
}

TEST_CASE("an outer sequence gap drops the partial BBP and resyncs on the next marker", "[stltp]") {
    Collector c;
    stltp::Demux d(c.callbacks());
    std::uint16_t inner_seq = 0, outer_seq = 10;
    std::vector<Bytes> inner;
    for (int i = 0; i < 3; ++i) {
        Bytes bbp = basebandPacket(0, Bytes(4000, static_cast<std::uint8_t>(i)), 5916);
        for (const Bytes& p : bppsPackets(0, bbp, inner_seq, 1)) inner.push_back(p);
    }
    auto tunnel = stltpTunnel(inner, outer_seq, 1360, 1);
    REQUIRE(tunnel.size() >= 8);
    tunnel.erase(tunnel.begin() + 2);  // lose one tunnel packet inside the first BBP
    feedAll(d, tunnel);
    CHECK(d.stats().outer_seq_gaps == 1);
    CHECK(d.stats().protocol_version == 1);
    REQUIRE(c.bbps.size() >= 1);
    CHECK(c.bbps.back().second.size() == 5916);
    CHECK(c.bbps.back().second.back() == 2);  // the last BBP is intact (payload sits after the padding)
    CHECK(c.bbps.size() <= 2);
}

TEST_CASE("non-tunnel RTP payload types are counted, not parsed", "[stltp]") {
    Collector c;
    stltp::Demux d(c.callbacks());
    const Bytes other = rtp(33, false, 1, 0, 0, Bytes(100, 0x47));
    d.feedTunnelPacket(ByteView(other.data(), other.size()));
    CHECK(d.stats().outer_other_pt == 1);
    const Bytes junk(5, 0);
    d.feedTunnelPacket(ByteView(junk.data(), junk.size()));
    CHECK(d.stats().outer_not_rtp == 1);
    CHECK(c.bbps.empty());
}

TEST_CASE("ALPTP tunnel delivers ALP packets with PLP id", "[stltp][alptp]") {
    Collector c;
    stltp::Demux d(c.callbacks());
    Bytes stream;
    auto addAlp = [&](std::uint8_t plp, const Bytes& alp) {
        stream.push_back(static_cast<std::uint8_t>(alp.size() >> 8));
        stream.push_back(static_cast<std::uint8_t>(alp.size() & 0xFF));
        stream.push_back(0);                                   // alp_sid
        stream.push_back(static_cast<std::uint8_t>(plp << 2)); // plp_id(6) lls(1) lmt_rdt(1)
        stream.insert(stream.end(), {0, 0, 0, 0});             // rap/time_limit/.../reserved
        stream.insert(stream.end(), alp.begin(), alp.end());
    };
    const Bytes a1{0x05, 0x03, 0x45, 0x00, 0x00};
    const Bytes a2(700, 0x42);
    addAlp(1, a1);
    addAlp(5, a2);
    std::uint16_t seq = 1;
    std::vector<Bytes> tunnel;
    for (std::size_t off = 0; off < stream.size(); off += 300) {
        const std::size_t n = std::min<std::size_t>(300, stream.size() - off);
        const bool marker = off == 0 || (off < 8 + a1.size() && off + n > 8 + a1.size());
        const std::uint16_t po = off == 0 ? 0 : static_cast<std::uint16_t>(8 + a1.size() - off);
        Bytes chunk(stream.begin() + static_cast<std::ptrdiff_t>(off), stream.begin() + static_cast<std::ptrdiff_t>(off + n));
        tunnel.push_back(rtp(82, marker, seq++, 0, marker ? po : 0u, chunk));
    }
    feedAll(d, tunnel);
    REQUIRE(c.alps.size() == 2);
    CHECK(c.alps[0].first == 1);
    CHECK(c.alps[0].second == a1);
    CHECK(c.alps[1].first == 5);
    CHECK(c.alps[1].second == a2);
}
