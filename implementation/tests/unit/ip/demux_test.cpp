#include <catch2/catch_test_macros.hpp>

#include "atsc3/ip/demux.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
io::UnitTime at(std::int64_t ns) {
    io::UnitTime t;
    t.t_ns = ns;
    t.capture_ns = ns;
    return t;
}
void feed(ip::Demux& d, const Bytes& pkt, std::int64_t ns = 0, std::uint8_t plp = 3) {
    d.onIpv4(plp, ByteView(pkt.data(), pkt.size()), at(ns));
}
}  // namespace

TEST_CASE("flows are routed to exact, wildcard and tap subscribers", "[ip][demux]") {
    ip::Demux d;
    std::vector<std::string> log;
    d.subscribe(ip::FlowKey{addr(239, 1, 1, 1), 5000, 0}, [&](const ip::UdpDatagram& u) { log.push_back("wild:" + std::to_string(u.payload.size())); });
    d.subscribe(ip::FlowKey{addr(239, 1, 1, 1), 5000, addr(10, 0, 0, 2)}, [&](const ip::UdpDatagram&) { log.push_back("exact"); });
    const auto tap = d.tapAll([&](const ip::UdpDatagram& u) { log.push_back("tap:" + std::to_string(u.dst_port)); });
    feed(d, ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 5000, Bytes(3, 0)));   // wildcard source
    feed(d, ipv4Udp(addr(10, 0, 0, 2), addr(239, 1, 1, 1), 1, 5000, Bytes(4, 0)));   // exact source wins
    feed(d, ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 5001, Bytes(5, 0)));   // unrouted
    CHECK(log == std::vector<std::string>{"wild:3", "tap:5000", "exact", "tap:5000", "tap:5001"});
    CHECK(d.stats().routed == 2);
    CHECK(d.stats().unrouted == 1);
    const auto un = d.unroutedFlows();
    REQUIRE(un.size() == 1);
    CHECK(un[0].dst_port == 5001);
    CHECK(un[0].plp == 3);
    d.unsubscribe(tap);
    feed(d, ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 5000, Bytes(3, 0)));
    CHECK(log.size() == 6);
    CHECK(log.back() == "wild:3");
}

TEST_CASE("datagram fields, PLP and SID pass through", "[ip][demux]") {
    ip::Demux d;
    ip::UdpDatagram got;
    Bytes payload_copy;
    d.subscribe(ip::FlowKey{addr(239, 2, 2, 2), 8000}, [&](const ip::UdpDatagram& u) {
        got = u;
        payload_copy.assign(u.payload.begin(), u.payload.end());
    });
    const Bytes payload{9, 8, 7};
    const Bytes pkt = ipv4Udp(addr(10, 5, 120, 103), addr(239, 2, 2, 2), 33699, 8000, payload);
    d.onIpv4(1, ByteView(pkt.data(), pkt.size()), at(42), 0x21);
    CHECK(got.src_ip == addr(10, 5, 120, 103));
    CHECK(got.src_port == 33699);
    CHECK(got.dst_port == 8000);
    CHECK(got.plp == 1);
    CHECK(got.sid == std::optional<std::uint8_t>(0x21));
    CHECK(got.time.t_ns == 42);
    CHECK(payload_copy == payload);
}

TEST_CASE("bad IP header checksum, non-UDP and bad UDP length are counted", "[ip][demux]") {
    ip::Demux d;
    int calls = 0;
    d.tapAll([&](const ip::UdpDatagram&) { ++calls; });
    Bytes pkt = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, Bytes(10, 1));
    Bytes bad = pkt;
    bad[10] ^= 0xFF;
    feed(d, bad);
    CHECK(d.stats().ip_checksum_bad == 1);
    Bytes tcp = pkt;
    tcp[9] = 6;
    tcp[10] = 0; tcp[11] = 0;
    {  // recompute checksum for the modified header
        std::uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2) sum += static_cast<std::uint32_t>((tcp[static_cast<std::size_t>(i)] << 8) | tcp[static_cast<std::size_t>(i) + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        const std::uint16_t cs = static_cast<std::uint16_t>(~sum);
        tcp[10] = static_cast<std::uint8_t>(cs >> 8);
        tcp[11] = static_cast<std::uint8_t>(cs & 0xFF);
    }
    feed(d, tcp);
    CHECK(d.stats().non_udp == 1);
    Bytes shortudp = pkt;
    shortudp[24] = 0xFF;  // UDP length beyond the packet
    feed(d, shortudp);
    CHECK(d.stats().udp_bad_length == 1);
    Bytes v6{0x60, 0, 0, 0};
    feed(d, v6);
    CHECK(d.stats().ipv6_or_other == 1);
    CHECK(calls == 0);
    feed(d, pkt);
    CHECK(calls == 1);
    // bad UDP checksum: counted, delivered unless strict
    Bytes badudp = pkt;
    badudp[26] = 0x12; badudp[27] = 0x34;
    feed(d, badudp);
    CHECK(d.stats().udp_checksum_bad == 1);
    CHECK(calls == 2);
    ip::DemuxConfig strict;
    strict.strict_udp_checksum = true;
    ip::Demux ds(strict);
    int c2 = 0;
    ds.tapAll([&](const ip::UdpDatagram&) { ++c2; });
    feed(ds, badudp);
    CHECK(c2 == 0);
}

TEST_CASE("IPv4 fragments are reassembled, out of order and with timeout", "[ip][demux]") {
    ip::Demux d;
    std::vector<Bytes> got;
    d.tapAll([&](const ip::UdpDatagram& u) { got.emplace_back(u.payload.begin(), u.payload.end()); });
    Bytes payload(2000);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);
    const Bytes whole = ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1, 2, payload, 0x4242);
    // split the IP payload (UDP header + data = 2008 bytes) into 3 fragments of 1000/1000/8 bytes
    auto frag = [&](std::size_t off, std::size_t len, bool more) {
        Bytes f(whole.begin(), whole.begin() + 20);
        f.insert(f.end(), whole.begin() + 20 + static_cast<std::ptrdiff_t>(off), whole.begin() + 20 + static_cast<std::ptrdiff_t>(off + len));
        const std::uint16_t total = static_cast<std::uint16_t>(20 + len);
        f[2] = static_cast<std::uint8_t>(total >> 8);
        f[3] = static_cast<std::uint8_t>(total & 0xFF);
        const std::uint16_t fo = static_cast<std::uint16_t>((more ? 0x2000 : 0) | (off / 8));
        f[6] = static_cast<std::uint8_t>(fo >> 8);
        f[7] = static_cast<std::uint8_t>(fo & 0xFF);
        f[10] = 0; f[11] = 0;
        std::uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2) sum += static_cast<std::uint32_t>((f[static_cast<std::size_t>(i)] << 8) | f[static_cast<std::size_t>(i) + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        const std::uint16_t cs = static_cast<std::uint16_t>(~sum);
        f[10] = static_cast<std::uint8_t>(cs >> 8);
        f[11] = static_cast<std::uint8_t>(cs & 0xFF);
        return f;
    };
    feed(d, frag(1000, 1000, true), 10);
    feed(d, frag(2000, 8, false), 20);
    CHECK(got.empty());
    feed(d, frag(0, 1000, true), 30);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == payload);
    CHECK(d.stats().reassembled == 1);
    CHECK(d.stats().fragments == 3);
    // a lone fragment expires
    feed(d, frag(0, 1000, true), 1'000'000'000);
    feed(d, whole, 5'000'000'000);  // triggers expiry
    CHECK(d.stats().fragments_dropped == 1);
    CHECK(got.size() == 2);
}
