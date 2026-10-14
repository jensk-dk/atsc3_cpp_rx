#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

#include "atsc3/tuners/pcap/pcap_reader.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::string writeRaw(const std::string& name, const Bytes& b) {
    const std::string p = tempPath(name);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p;
}
void le32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
void be32(Bytes& b, std::uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
void le16(Bytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v & 0xFF)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
}  // namespace

TEST_CASE("classic pcap written by PcapWriter reads back with ns timestamps", "[io][pcap]") {
    const std::string p = writePcap({{1'700'000'000'123'456'789LL, Bytes{1, 2, 3}}, {1'700'000'000'223'456'789LL, Bytes{4}}},
                                    289, "ns.pcap");
    auto r = pcap::PcapReader::open(p);
    REQUIRE(r);
    REQUIRE_FALSE(r->isPcapng());
    REQUIRE(r->interfaces() == std::vector<std::uint16_t>{289});
    pcap::PcapRecord rec;
    REQUIRE(r->next(rec));
    CHECK(rec.link_type == 289);
    CHECK(rec.ts_ns == 1'700'000'000'123'456'789LL);
    CHECK(rec.data == Bytes{1, 2, 3});
    REQUIRE(r->next(rec));
    CHECK(rec.ts_ns == 1'700'000'000'223'456'789LL);
    REQUIRE_FALSE(r->next(rec));
    CHECK_FALSE(r->truncated());
    r->rewind();
    REQUIRE(r->next(rec));
    CHECK(rec.data.size() == 3);
    std::remove(p.c_str());
}

TEST_CASE("classic pcap big-endian microsecond variant", "[io][pcap]") {
    Bytes f;
    be32(f, 0xA1B2C3D4);
    f.insert(f.end(), {0, 2, 0, 4});
    be32(f, 0);
    be32(f, 0);
    be32(f, 65535);
    be32(f, 1);
    be32(f, 10);     // sec
    be32(f, 5);      // usec
    be32(f, 2);      // incl
    be32(f, 2);      // orig
    f.insert(f.end(), {0xAA, 0xBB});
    const std::string p = writeRaw("be.pcap", f);
    auto r = pcap::PcapReader::open(p);
    REQUIRE(r);
    pcap::PcapRecord rec;
    REQUIRE(r->next(rec));
    CHECK(rec.link_type == 1);
    CHECK(rec.ts_ns == 10'000'005'000LL);
    CHECK(rec.data == Bytes{0xAA, 0xBB});
    std::remove(p.c_str());
}

TEST_CASE("truncated classic pcap tail ends the stream cleanly", "[io][pcap]") {
    const std::string p = writePcap({{0, Bytes(100, 7)}}, 289, "trunc.pcap");
    {
        std::ifstream in(p, std::ios::binary);
        Bytes all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        all.resize(all.size() - 40);
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(all.size()));
    }
    auto r = pcap::PcapReader::open(p);
    REQUIRE(r);
    pcap::PcapRecord rec;
    REQUIRE_FALSE(r->next(rec));
    CHECK(r->truncated());
    std::remove(p.c_str());
}

TEST_CASE("pcapng with two interfaces and per-interface resolution", "[io][pcap][pcapng]") {
    Bytes f;
    auto block = [&](std::uint32_t type, const Bytes& body) {
        const std::uint32_t total = static_cast<std::uint32_t>(12 + body.size());
        le32(f, type);
        le32(f, total);
        f.insert(f.end(), body.begin(), body.end());
        le32(f, total);
    };
    Bytes shb;
    le32(shb, 0x1A2B3C4D);
    le16(shb, 1);
    le16(shb, 0);
    for (int i = 0; i < 8; ++i) shb.push_back(0xFF);
    block(0x0A0D0D0A, shb);
    Bytes idb0;  // link 289, default µs
    le16(idb0, 289);
    le16(idb0, 0);
    le32(idb0, 0);
    block(1, idb0);
    Bytes idb1;  // Ethernet with if_tsresol = 9 (ns)
    le16(idb1, 1);
    le16(idb1, 0);
    le32(idb1, 0);
    le16(idb1, 9);
    le16(idb1, 1);
    idb1.push_back(9);
    idb1.insert(idb1.end(), {0, 0, 0});
    le16(idb1, 0);
    le16(idb1, 0);
    block(1, idb1);
    auto epb = [&](std::uint32_t ifid, std::uint64_t ts, const Bytes& data) {
        Bytes b;
        le32(b, ifid);
        le32(b, static_cast<std::uint32_t>(ts >> 32));
        le32(b, static_cast<std::uint32_t>(ts & 0xFFFFFFFF));
        le32(b, static_cast<std::uint32_t>(data.size()));
        le32(b, static_cast<std::uint32_t>(data.size()));
        b.insert(b.end(), data.begin(), data.end());
        while (b.size() % 4) b.push_back(0);
        block(6, b);
    };
    epb(0, 1'000'000, Bytes{0x05, 0x4B, 0x45});
    epb(1, 2'000'000'000ULL, Bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x08, 0x00, 0x45});
    const std::string p = writeRaw("two.pcapng", f);
    auto r = pcap::PcapReader::open(p);
    REQUIRE(r);
    REQUIRE(r->isPcapng());
    pcap::PcapRecord rec;
    REQUIRE(r->next(rec));
    CHECK(rec.interface_id == 0);
    CHECK(rec.link_type == 289);
    CHECK(rec.ts_ns == 1'000'000'000LL);  // 1e6 µs
    CHECK(rec.data.size() == 3);
    REQUIRE(r->next(rec));
    CHECK(rec.interface_id == 1);
    CHECK(rec.link_type == 1);
    CHECK(rec.ts_ns == 2'000'000'000LL);  // ns resolution
    REQUIRE_FALSE(r->next(rec));
    REQUIRE(r->interfaces() == std::vector<std::uint16_t>{289, 1});
    std::remove(p.c_str());
}

TEST_CASE("corpus sample opens with link type 289", "[io][pcap][corpus]") {
    const std::string p = corpus("alp-sample1.pcap");
    if (!exists(p)) SKIP("corpus not available");
    auto r = pcap::PcapReader::open(p);
    REQUIRE(r);
    pcap::PcapRecord rec;
    std::size_t n = 0;
    while (r->next(rec)) ++n;
    CHECK(n == 17);
    CHECK(rec.link_type == 289);
}
