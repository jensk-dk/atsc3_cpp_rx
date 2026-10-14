#include <catch2/catch_test_macros.hpp>

#include "atsc3/phy/bbp.hpp"
#include "atsc3/tools/bbp_packer.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::vector<Bytes> sampleAlps(int n, std::size_t base = 200) {
    std::vector<Bytes> v;
    for (int i = 0; i < n; ++i) {
        const std::size_t len = (static_cast<std::size_t>(i) * 397) % 1400 + base;  // 200..1600ish
        v.push_back(alpIpv4Packet(len, static_cast<std::uint32_t>(i)));
    }
    // a few long packets with HM headers and one LMT signaling packet
    v.push_back(alpIpv4Packet(4000, 99));
    v.push_back(Bytes{0x80, 0x03, 0x01, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x03, 0x00});
    v.push_back(alpIpv4Packet(3000, 100));
    return v;
}
struct Out {
    std::vector<std::pair<std::uint8_t, Bytes>> alps;
    phy::BbpDeframer::AlpSink sink() {
        return [this](std::uint8_t plp, ByteView b) { alps.emplace_back(plp, Bytes(b.begin(), b.end())); };
    }
};
}  // namespace

TEST_CASE("A/322 worked example: pointer 130 encodes as 82 04", "[phy][bbp]") {
    tools::BbpPackerConfig cfg;
    cfg.kpayload_bytes = 400;
    std::uint16_t counter = 0;
    std::vector<Bytes> alps{alpIpv4Packet(126 + 200, 1), alpIpv4Packet(50, 2)};
    // make the first packet end exactly at payload offset 130 of the second BBP: build directly instead
    Bytes b = basebandPacket(130, Bytes(300, 0), 400);
    CHECK(b[0] == 0x82);
    CHECK((b[1] & 0xFC) == 0x04);
    phy::BbpHeader h;
    REQUIRE(phy::parseBbpHeader(ByteView(b.data(), b.size()), h) == phy::BbpStatus::Ok);
    CHECK(h.mode);
    CHECK(h.pointer == 130);
    CHECK(h.ofi == 2);
    CHECK(h.ext_type == 7);
    CHECK(h.ext_len == 96);
    CHECK(h.header_length == 100);
    (void)packBbps(alps, cfg, counter);
}

TEST_CASE("BBP header modes: MODE 0, short/long/mixed extensions, counter, invalid pointer", "[phy][bbp]") {
    phy::BbpHeader h;
    Bytes m0{0x05, 1, 2, 3, 4, 5, 6};
    REQUIRE(phy::parseBbpHeader(ByteView(m0.data(), m0.size()), h) == phy::BbpStatus::Ok);
    CHECK_FALSE(h.mode);
    CHECK(h.pointer == 5);
    CHECK(h.header_length == 1);
    Bytes shortc{0x80, 0x01, 0x05, 0x12, 0x34, 0, 0, 0, 9, 9};  // OFI 01, EXT_TYPE 000, EXT_LEN 5: counter 0x1234 + 3 pad
    REQUIRE(phy::parseBbpHeader(ByteView(shortc.data(), shortc.size()), h) == phy::BbpStatus::Ok);
    CHECK(h.ofi == 1);
    REQUIRE(h.counter);
    CHECK(*h.counter == 0x1234);
    CHECK(h.header_length == 8);
    Bytes mixed{0x80, 0x03, 0x44, 0x00, 0x02, 0x00, 0x00, 0x00, 0xAA, 0xBB};  // NUM_EXT 2, EXT_LEN 4: 2 sub-headers
    REQUIRE(phy::parseBbpHeader(ByteView(mixed.data(), mixed.size()), h) == phy::BbpStatus::Ok);
    CHECK(h.ofi == 3);
    CHECK(h.num_ext == 2);
    CHECK(h.header_length == 8);
    Bytes bad{0xFF, 0xFC, 0, 0};  // pointer 8191 (all 13 bits), OFI 00 -> ok
    REQUIRE(phy::parseBbpHeader(ByteView(bad.data(), bad.size()), h) == phy::BbpStatus::Ok);
    CHECK(h.pointer == 8191);
    Bytes bad2{0x90, 0x00, 0, 0};  // pointer 16 but only 2 payload bytes
    REQUIRE(phy::parseBbpHeader(ByteView(bad2.data(), bad2.size()), h) == phy::BbpStatus::Invalid);
}

TEST_CASE("pack/deframe round trip reproduces the ALP sequence for several Kpayload sizes", "[phy][bbp][roundtrip]") {
    const std::vector<Bytes> alps = sampleAlps(60);
    for (std::size_t kp : {5916u, 1001u, 300u, 37u, 9u}) {
        for (bool counter : {false, true}) {
            INFO("kpayload " << kp << " counter " << counter);
            tools::BbpPackerConfig cfg;
            cfg.kpayload_bytes = kp;
            cfg.counter = counter;
            std::uint16_t cnt = 65530;  // wraps during the run
            const auto bbps = packBbps(alps, cfg, cnt);
            REQUIRE_FALSE(bbps.empty());
            for (const Bytes& b : bbps) REQUIRE(b.size() == kp);
            Out out;
            phy::BbpDeframer d(out.sink());
            for (const Bytes& b : bbps) d.feed(3, ByteView(b.data(), b.size()));
            REQUIRE(out.alps.size() == alps.size());
            for (std::size_t i = 0; i < alps.size(); ++i) {
                CHECK(out.alps[i].first == 3);
                CHECK(out.alps[i].second == alps[i]);
            }
            CHECK(d.stats().pending_mismatch == 0);
            CHECK(d.stats().alp_invalid == 0);
            CHECK(d.stats().orphan_prefix == 0);
            CHECK(d.stats().counter_jumps == 0);
            CHECK(d.stats().bbp == bbps.size());
            if (kp <= 300) CHECK(d.stats().pointer_no_start > 0);
        }
    }
}

TEST_CASE("deframer survives lost BBPs, error flags and counter jumps", "[phy][bbp]") {
    const std::vector<Bytes> alps = sampleAlps(40);
    tools::BbpPackerConfig cfg;
    cfg.kpayload_bytes = 500;
    cfg.counter = true;
    std::uint16_t cnt = 0;
    auto bbps = packBbps(alps, cfg, cnt);
    REQUIRE(bbps.size() > 20);
    Out out;
    phy::BbpDeframer d(out.sink());
    for (std::size_t i = 0; i < bbps.size(); ++i) {
        if (i == 5) continue;  // lost BBP: counter jump
        d.feed(0, ByteView(bbps[i].data(), bbps[i].size()), i == 12 /* FEC error */);
    }
    CHECK(d.stats().counter_jumps >= 1);
    CHECK(d.stats().bbp_error == 1);
    CHECK(out.alps.size() < alps.size());
    CHECK(out.alps.size() > alps.size() / 2);
    for (const auto& [plp, a] : out.alps) {
        bool found = false;
        for (const Bytes& x : alps) if (x == a) { found = true; break; }
        CHECK(found);  // every delivered packet is a genuine one (no corrupted output)
    }
    // interleaved PLPs keep separate pending state
    Out out2;
    phy::BbpDeframer d2(out2.sink());
    std::uint16_t c0 = 0, c1 = 0;
    const auto b0 = packBbps(sampleAlps(10), cfg, c0);
    const auto b1 = packBbps(sampleAlps(10, 300), cfg, c1);
    for (std::size_t i = 0; i < std::max(b0.size(), b1.size()); ++i) {
        if (i < b0.size()) d2.feed(0, ByteView(b0[i].data(), b0[i].size()));
        if (i < b1.size()) d2.feed(1, ByteView(b1[i].data(), b1[i].size()));
    }
    CHECK(out2.alps.size() == 26);
    CHECK(d2.stats().pending_mismatch == 0);
}
