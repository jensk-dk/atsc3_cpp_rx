#include <catch2/catch_test_macros.hpp>

#include "atsc3/alp/decap.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
struct Collect {
    std::vector<std::pair<std::uint8_t, Bytes>> ipv4;
    std::vector<std::optional<std::uint8_t>> sids;
    std::vector<Bytes> rohc, ts;
    std::vector<alp::SignalingInfo> signaling;
    std::vector<Bytes> tables;
    std::vector<alp::Event> events;
    alp::Callbacks cb() {
        alp::Callbacks c;
        c.onIpv4 = [this](std::uint8_t plp, ByteView b, std::optional<std::uint8_t> sid) {
            ipv4.emplace_back(plp, Bytes(b.begin(), b.end()));
            sids.push_back(sid);
        };
        c.onCompressedIp = [this](std::uint8_t, ByteView b, std::optional<std::uint8_t>) { rohc.emplace_back(b.begin(), b.end()); };
        c.onSignaling = [this](std::uint8_t, const alp::SignalingInfo& i, ByteView t) {
            signaling.push_back(i);
            tables.emplace_back(t.begin(), t.end());
        };
        c.onTs = [this](std::uint8_t, ByteView b) { ts.emplace_back(b.begin(), b.end()); };
        c.onEvent = [this](const alp::Event& e) { events.push_back(e); };
        return c;
    }
};
void feed(alp::Decap& d, std::uint8_t plp, const Bytes& b) { d.feed(plp, ByteView(b.data(), b.size())); }
std::size_t count(const Collect& c, alp::EventType t) {
    std::size_t n = 0;
    for (const auto& e : c.events) if (e.type == t) ++n;
    return n;
}
}  // namespace

TEST_CASE("single IPv4 packets are delivered whole, with SID", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    const Bytes pkt = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, Bytes(50, 9));
    feed(d, 1, alpSingle(pkt));
    feed(d, 1, alpSingle(pkt, 0, true, 7));
    REQUIRE(c.ipv4.size() == 2);
    CHECK(c.ipv4[0].second == pkt);
    CHECK(c.ipv4[1].second == pkt);
    CHECK_FALSE(c.sids[0]);
    CHECK(c.sids[1] == std::optional<std::uint8_t>(7));
    CHECK(d.stats().ipv4 == 2);
}

TEST_CASE("zero-length vendor packets and truncated packets produce events only", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    feed(d, 0, Bytes{0x08, 0x00, 0x05, 0xF1, 0x00, 0x00});
    feed(d, 0, Bytes{0x05, 0xDC, 0x45, 0x00});  // declares 1500, has 2
    feed(d, 0, Bytes{0x05});
    CHECK(c.ipv4.empty());
    CHECK(count(c, alp::EventType::ZeroLength) == 1);
    CHECK(count(c, alp::EventType::PrivateExtension) == 1);
    CHECK(count(c, alp::EventType::Truncated) == 2);
    CHECK(d.stats().zero_length == 1);
    CHECK(d.stats().truncated == 1);
    CHECK(d.stats().invalid == 1);
}

TEST_CASE("segmentation is reassembled in order and dropped on gaps", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    const Bytes pkt = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, Bytes(3000, 3));
    auto seg = [&](std::uint8_t sn, bool last, std::size_t from, std::size_t to) {
        Bytes b{static_cast<std::uint8_t>(0x10 | ((to - from) >> 8)), static_cast<std::uint8_t>((to - from) & 0xFF),
                static_cast<std::uint8_t>((sn << 3) | (last ? 0x04 : 0))};
        b.insert(b.end(), pkt.begin() + static_cast<std::ptrdiff_t>(from), pkt.begin() + static_cast<std::ptrdiff_t>(to));
        return b;
    };
    feed(d, 0, seg(0, false, 0, 1000));
    feed(d, 0, seg(1, false, 1000, 2000));
    feed(d, 0, seg(2, true, 2000, pkt.size()));
    REQUIRE(c.ipv4.size() == 1);
    CHECK(c.ipv4[0].second == pkt);
    CHECK(d.stats().reassembled == 1);
    // gap: SN 0, SN 2
    feed(d, 0, seg(0, false, 0, 1000));
    feed(d, 0, seg(2, true, 2000, pkt.size()));
    CHECK(c.ipv4.size() == 1);
    CHECK(count(c, alp::EventType::ReassemblyDrop) == 1);
    // segment without a first
    feed(d, 0, seg(1, true, 1000, 2000));
    CHECK(count(c, alp::EventType::ReassemblyDrop) == 2);
    // interleaved PLPs do not interfere
    feed(d, 0, seg(0, false, 0, 1000));
    feed(d, 1, seg(0, false, 0, 1000));
    feed(d, 0, seg(1, true, 1000, pkt.size()));
    feed(d, 1, seg(1, true, 1000, pkt.size()));
    CHECK(c.ipv4.size() == 3);
    CHECK(c.ipv4[1].first == 0);
    CHECK(c.ipv4[2].first == 1);
}

TEST_CASE("concatenated packets are split into components", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    const Bytes a = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, Bytes(72, 1));   // 100 bytes
    const Bytes b = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 2), 1, 2, Bytes(22, 2));   // 50 bytes
    const Bytes e = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 3), 1, 2, Bytes(122, 3));  // 150 bytes
    Bytes pkt{0x19, 0x2C, 0x02, 0x06, 0x40, 0x32};  // 3 components: 100, 50, implied 150
    pkt.insert(pkt.end(), a.begin(), a.end());
    pkt.insert(pkt.end(), b.begin(), b.end());
    pkt.insert(pkt.end(), e.begin(), e.end());
    feed(d, 2, pkt);
    REQUIRE(c.ipv4.size() == 3);
    CHECK(c.ipv4[0].second == a);
    CHECK(c.ipv4[1].second == b);
    CHECK(c.ipv4[2].second == e);
    CHECK(d.stats().concatenated == 1);
    CHECK(d.stats().components == 3);
}

TEST_CASE("SID filter drops foreign sub-streams", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    d.setSidFilter(0, {5});
    const Bytes pkt = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, Bytes(10, 9));
    feed(d, 0, alpSingle(pkt, 0, true, 5));
    feed(d, 0, alpSingle(pkt, 0, true, 6));
    feed(d, 0, alpSingle(pkt));         // no SID passes
    feed(d, 1, alpSingle(pkt, 0, true, 6));  // other PLP unfiltered
    CHECK(c.ipv4.size() == 3);
    CHECK(d.stats().sid_filtered == 1);
}

TEST_CASE("signaling and compressed-IP packets are routed by type", "[alp][decap]") {
    Collect c;
    alp::Decap d(c.cb());
    Bytes lmt{0x80, 0x03, 0x01, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x03, 0x00};  // LMT v7 with 1 PLP, 0 multicasts
    feed(d, 0, lmt);
    REQUIRE(c.signaling.size() == 1);
    CHECK(c.signaling[0].type == 1);
    CHECK(c.signaling[0].version == 7);
    CHECK(c.tables[0] == Bytes{0x03, 0x03, 0x00});
    feed(d, 0, alpSingle(Bytes{0xFD, 0x01, 0x02}, 2));
    REQUIRE(c.rohc.size() == 1);
    CHECK(c.rohc[0].size() == 3);
    feed(d, 0, alpSingle(Bytes{1}, 1));
    CHECK(count(c, alp::EventType::ReservedType) == 1);
}

TEST_CASE("MPEG-2 TS packets are reconstructed with sync bytes, null packets and deleted headers", "[alp][decap][ts]") {
    Collect c;
    alp::Decap d(c.cb());
    // two packets, no AHF
    Bytes p{0xE4};
    Bytes t1(187, 0x11), t2(187, 0x22);
    t1[0] = 0x01; t1[1] = 0x00; t1[2] = 0x15;
    t2[0] = 0x01; t2[1] = 0x00; t2[2] = 0x16;
    p.insert(p.end(), t1.begin(), t1.end());
    p.insert(p.end(), t2.begin(), t2.end());
    feed(d, 0, p);
    REQUIRE(c.ts.size() == 1);
    REQUIRE(c.ts[0].size() == 376);
    CHECK(c.ts[0][0] == 0x47);
    CHECK(c.ts[0][3] == 0x15);
    CHECK(c.ts[0][188] == 0x47);
    CHECK(c.ts[0][191] == 0x16);
    // HDM with 3 packets and 2 deleted nulls before them
    Bytes q{0xE7, 0x82};
    Bytes first(187, 0x33);
    first[0] = 0x40; first[1] = 0x64; first[2] = 0x1A;  // PID 0x064, CC 0xA
    q.insert(q.end(), first.begin(), first.end());
    q.insert(q.end(), 184, 0x44);
    q.insert(q.end(), 184, 0x55);
    feed(d, 0, q);
    REQUIRE(c.ts.size() == 2);
    const Bytes& out = c.ts[1];
    REQUIRE(out.size() == 5 * 188);
    CHECK(out[0] == 0x47);
    CHECK(out[1] == 0x1F);
    CHECK(out[2] == 0xFF);
    CHECK(out[188 * 2 + 3] == 0x1A);
    CHECK(out[188 * 3 + 1] == 0x40);
    CHECK(out[188 * 3 + 3] == 0x1B);  // continuity counter incremented
    CHECK(out[188 * 3 + 4] == 0x44);
    CHECK(out[188 * 4 + 3] == 0x1C);
    CHECK(out[188 * 4 + 187] == 0x55);
    CHECK(d.stats().ts_packets == 7);
}
