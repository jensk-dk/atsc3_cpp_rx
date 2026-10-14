#include <catch2/catch_test_macros.hpp>

#include "atsc3/alp/decap.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"
#include "atsc3/link/rohc.hpp"
#include "atsc3/link/signaling.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
Bytes lmtTable() {
    // num_PLPs_minus1=1 (2 PLPs); PLP 0: 2 multicasts (one with SID), PLP 2: 1 multicast with CID
    Bytes t{0x07};
    t.push_back(0x03); t.push_back(2);
    t.insert(t.end(), {0, 0, 0, 0, 224, 0, 23, 60, 0, 0, 0x13, 0x49, 0x3F});
    t.insert(t.end(), {10, 1, 1, 1, 239, 255, 2, 1, 0x27, 0x10, 0x13, 0x9D, 0xBF, 0x21});  // SID 0x21
    t.push_back(0x0B); t.push_back(1);
    t.insert(t.end(), {10, 1, 1, 1, 239, 255, 3, 1, 0, 0, 0x1F, 0x40, 0x7F, 0x05});  // CID 5
    return t;
}
}  // namespace

TEST_CASE("LMT parses PLP loops, SID and context id", "[link][lmt]") {
    link::Lmt lmt;
    const Bytes t = lmtTable();
    REQUIRE(link::parseLmt(ByteView(t.data(), t.size()), lmt));
    REQUIRE(lmt.plps.size() == 2);
    CHECK(lmt.plps[0].plp_id == 0);
    REQUIRE(lmt.plps[0].multicasts.size() == 2);
    CHECK(lmt.plps[0].multicasts[0].dst_ip == 0xE000173Cu);
    CHECK(lmt.plps[0].multicasts[0].dst_port == 4937);
    CHECK(lmt.plps[0].multicasts[0].src_ip == 0);
    CHECK_FALSE(lmt.plps[0].multicasts[0].sid);
    CHECK(lmt.plps[0].multicasts[1].src_ip == addr(10, 1, 1, 1));
    CHECK(lmt.plps[0].multicasts[1].src_port == 10000);
    CHECK(lmt.plps[0].multicasts[1].dst_port == 5021);
    CHECK(lmt.plps[0].multicasts[1].sid == std::optional<std::uint8_t>(0x21));
    CHECK(lmt.plps[1].plp_id == 2);
    CHECK(lmt.plps[1].multicasts[0].context_id == std::optional<std::uint8_t>(5));
    Bytes trunc(t.begin(), t.begin() + 10);
    CHECK_FALSE(link::parseLmt(ByteView(trunc.data(), trunc.size()), lmt));
}

TEST_CASE("RDT parses contexts with chains", "[link][rdt]") {
    // PLP 1, max_CID 3, adaptation mode 2 (01), context_config 1 (static), num_context 2
    Bytes t{static_cast<std::uint8_t>((1 << 2) | 0), static_cast<std::uint8_t>((3 << 2) | 1), 0x40 | 0x3F, 2,
            0, 0x02, 3, 0xAA, 0xBB, 0xCC,
            1, 0x01, 2, 0x11, 0x22};
    link::Rdt rdt;
    REQUIRE(link::parseRdt(ByteView(t.data(), t.size()), rdt));
    CHECK(rdt.plp_id == 1);
    CHECK(rdt.max_cid == 3);
    CHECK(rdt.adaptation_mode == 1);
    CHECK(rdt.context_config == 1);
    REQUIRE(rdt.contexts.size() == 2);
    CHECK(rdt.contexts[0].context_id == 0);
    CHECK(rdt.contexts[0].profile == 2);
    CHECK(rdt.contexts[0].chain == Bytes{0xAA, 0xBB, 0xCC});
    CHECK(rdt.contexts[1].chain == Bytes{0x11, 0x22});
    link::NullRohcDecompressor nul;
    nul.applyRdt(rdt);
    CHECK_FALSE(nul.decompress(1, ByteView(t.data(), 2)));
    CHECK(nul.dropped() == 1);
}

TEST_CASE("PLP map lookup prefers exact source and layers sidecar entries", "[link][plpmap]") {
    link::Lmt lmt;
    const Bytes t = lmtTable();
    REQUIRE(link::parseLmt(ByteView(t.data(), t.size()), lmt));
    link::PlpMap map;
    map.applyLmt(0, lmt);
    CHECK(map.size() == 3);
    auto e = map.lookup(0xE000173Cu, 4937);
    REQUIRE(e);
    CHECK(e->plp_id == 0);
    e = map.lookup(addr(239, 255, 3, 1), 8000, addr(10, 1, 1, 1), 0);
    REQUIRE(e);
    CHECK(e->plp_id == 2);
    CHECK(e->context_id == std::optional<std::uint8_t>(5));
    CHECK_FALSE(map.lookup(addr(239, 255, 3, 1), 8000, addr(10, 9, 9, 9), 0));  // source mismatch
    CHECK_FALSE(map.lookup(addr(239, 255, 9, 9), 1));
    std::vector<io::LmtEntry> side{{5, 0, addr(239, 255, 24, 1), 0, 8000}};
    map.applySidecar(side);
    CHECK(map.size() == 4);
    e = map.lookup(addr(239, 255, 24, 1), 8000);
    REQUIRE(e);
    CHECK(e->plp_id == 5);
    CHECK(e->origin == io::LmtOrigin::Sidecar);
    map.applyLmt(0, link::Lmt{});  // new LMT from PLP 0 replaces its entries, sidecar stays
    CHECK(map.size() == 1);
}

TEST_CASE("LinkSignaling dedupes by version and routes user-private tables", "[link][signaling]") {
    int lmt_calls = 0, priv = 0;
    link::LinkSignaling::Callbacks cb;
    cb.onLmt = [&](std::uint8_t, const link::Lmt&) { ++lmt_calls; };
    cb.onUserPrivate = [&](std::uint8_t, const alp::SignalingInfo&, ByteView) { ++priv; };
    link::LinkSignaling ls(cb);
    const Bytes t = lmtTable();
    alp::SignalingInfo info;
    info.type = 1;
    info.version = 3;
    ls.onSignaling(0, info, ByteView(t.data(), t.size()));
    ls.onSignaling(0, info, ByteView(t.data(), t.size()));
    CHECK(lmt_calls == 1);
    info.version = 4;
    ls.onSignaling(0, info, ByteView(t.data(), t.size()));
    CHECK(lmt_calls == 2);
    CHECK(ls.stats().lmt == 3);
    CHECK(ls.stats().lmt_new == 2);
    REQUIRE(ls.lmt(0));
    CHECK(ls.lmt(0)->version == 4);
    info.type = 0xF2;
    ls.onSignaling(0, info, ByteView(t.data(), 2));
    CHECK(priv == 1);
    info.type = 1;
    info.format = 1;  // XML LMT is not a thing
    ls.onSignaling(0, info, ByteView(t.data(), t.size()));
    CHECK(ls.stats().unsupported == 1);
}

TEST_CASE("corpus ALP samples carry LMTs that parse and build the PLP map", "[link][corpus]") {
    struct Case {
        const char* file; std::uint8_t version; std::size_t plps; std::size_t multicasts;
        unsigned known_dst[4]; std::uint16_t known_port; std::uint8_t known_plp;
    };
    const Case cases[] = {{"alp-sample2.pcap", 56, 1, 4, {239, 255, 18, 1}, 5181, 0},
                          {"alp-sample1.pcap", 2, 3, 56, {239, 255, 45, 1}, 5002, 1},
                          {"ch11-25-slt.pcap", 2, 3, 56, {239, 255, 45, 1}, 5002, 1}};  // two LMTs alternate; the last one wins
    bool any = false;
    for (const Case& c : cases) {
        const std::string p = corpus(c.file);
        if (!exists(p)) continue;
        any = true;
        INFO(c.file);
        link::LinkSignaling ls({});
        alp::Callbacks cb;
        cb.onSignaling = [&](std::uint8_t plp, const alp::SignalingInfo& i, ByteView t) { ls.onSignaling(plp, i, t); };
        alp::Decap decap(cb);
        auto r = pcap::PcapReader::open(p);
        REQUIRE(r);
        pcap::PcapRecord rec;
        while (r->next(rec)) decap.feed(io::kUnknownPlp, ByteView(rec.data.data(), rec.data.size()));
        const auto lmt = ls.lmt(io::kUnknownPlp);
        REQUIRE(lmt);
        CHECK(lmt->version == c.version);
        CHECK(lmt->plps.size() == c.plps);
        std::size_t n = 0;
        for (const auto& plp : lmt->plps) {
            n += plp.multicasts.size();
            for (const auto& m : plp.multicasts) CHECK(io::isMulticast(m.dst_ip));
        }
        CHECK(n == c.multicasts);
        CHECK(ls.plpMap().size() == c.multicasts);
        CHECK(ls.stats().invalid == 0);
        // A/330 §7.1.1: the LLS multicast is normally absent from the LMT (alp-sample1's encoder lists it anyway).
        const auto known = ls.plpMap().lookup(addr(c.known_dst[0], c.known_dst[1], c.known_dst[2], c.known_dst[3]), c.known_port);
        REQUIRE(known);
        CHECK(known->plp_id == c.known_plp);
        CHECK(decap.stats().invalid == 0);
    }
    if (!any) SKIP("corpus not available");
}
