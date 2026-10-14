#include <catch2/catch_test_macros.hpp>

#include "atsc3/io/inet.hpp"
#include "link_strip.hpp"   // private header of tuners/pcap
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

TEST_CASE("IPv4 text conversion", "[io][inet]") {
    std::uint32_t a = 0;
    REQUIRE(io::parseIpv4("224.0.23.60", a));
    CHECK(a == 0xE000173Cu);
    CHECK(io::ipv4ToString(a) == "224.0.23.60");
    CHECK_FALSE(io::parseIpv4("256.1.1.1", a));
    CHECK_FALSE(io::parseIpv4("1.2.3", a));
    CHECK(io::isMulticast(addr(239, 255, 24, 1)));
    CHECK_FALSE(io::isMulticast(addr(10, 0, 0, 1)));
}

TEST_CASE("IPv4/UDP parse and link stripping", "[io][inet]") {
    const Bytes payload{1, 2, 3, 4, 5};
    const Bytes pkt = ipv4Udp(addr(10, 1, 1, 1), addr(239, 1, 2, 3), 1234, 5678, payload);
    io::Ipv4Udp h;
    REQUIRE(io::parseIpv4Udp(ByteView(pkt.data(), pkt.size()), h));
    CHECK(h.udp);
    CHECK(h.src == addr(10, 1, 1, 1));
    CHECK(h.dst == addr(239, 1, 2, 3));
    CHECK(h.sport == 1234);
    CHECK(h.dport == 5678);
    CHECK(h.payload.size() == 5);
    CHECK(h.payload[4] == 5);

    ByteView ipv;
    const Bytes eth = ethernet(pkt);
    REQUIRE(pcap::stripToIpv4(1, ByteView(eth.data(), eth.size()), ipv));
    CHECK(ipv.size() == pkt.size());
    const Bytes vlan = ethernet(pkt, true);
    REQUIRE(pcap::stripToIpv4(1, ByteView(vlan.data(), vlan.size()), ipv));
    CHECK(ipv.size() == pkt.size());
    const Bytes sll = linuxSll(pkt);
    REQUIRE(pcap::stripToIpv4(113, ByteView(sll.data(), sll.size()), ipv));
    REQUIRE(pcap::stripToIpv4(101, ByteView(pkt.data(), pkt.size()), ipv));
    Bytes arp = eth;
    arp[12] = 0x08;
    arp[13] = 0x06;
    CHECK_FALSE(pcap::stripToIpv4(1, ByteView(arp.data(), arp.size()), ipv));
}

TEST_CASE("RTP parse with extension and padding", "[io][inet]") {
    Bytes r = rtp(97, true, 0x3880, 0x1EF107B2, 0x00000010, Bytes{9, 9, 9, 9, 9, 9, 9, 2});
    r[0] = static_cast<std::uint8_t>(r[0] | 0x20);  // padding: last byte (2) = pad length
    io::Rtp h;
    REQUIRE(io::parseRtp(ByteView(r.data(), r.size()), h));
    CHECK(h.version == 2);
    CHECK(h.marker);
    CHECK(h.pt == 97);
    CHECK(h.seq == 0x3880);
    CHECK(h.ssrc == 0x10);
    CHECK(h.payload.size() == 6);
    Bytes ext{0x90, 0x61, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0xAB, 0xCD, 0, 1, 1, 2, 3, 4, 0x55};
    REQUIRE(io::parseRtp(ByteView(ext.data(), ext.size()), h));
    CHECK(h.extension);
    CHECK(h.ext.size() == 4);
    CHECK(h.payload.size() == 1);
    CHECK(h.payload[0] == 0x55);
}
