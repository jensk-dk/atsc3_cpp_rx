#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"
#include "atsc3/tools/bbp_packer.hpp"
#include "atsc3/tools/stltp2alp.hpp"
#include "atsc3/tools/stltp_packer.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::vector<Bytes> readAll(const std::string& path, std::uint16_t expect_lt) {
    auto r = pcap::PcapReader::open(path);
    REQUIRE(r);
    std::vector<Bytes> out;
    pcap::PcapRecord rec;
    while (r->next(rec)) {
        REQUIRE(rec.link_type == expect_lt);
        out.push_back(rec.data);
    }
    return out;
}
}  // namespace

TEST_CASE("ALP -> BBP -> STLTP capture -> stltp2alp reproduces the ALP packets", "[roundtrip][stltp2alp]") {
    // source ALP packets: the corpus sample when available, else synthetic
    std::vector<Bytes> alps;
    if (exists(corpus("alp-sample2.pcap"))) {
        for (const Bytes& b : readAll(corpus("alp-sample2.pcap"), 289)) alps.push_back(b);
    }
    for (int i = 0; i < 120; ++i) alps.push_back(alpIpv4Packet(static_cast<std::size_t>(100 + (i * 611) % 1500), static_cast<std::uint32_t>(i)));
    // drop vendor zero-length markers: a BBP packer is a modulator and would not emit them
    std::vector<Bytes> clean;
    for (const Bytes& a : alps) if (!(a.size() >= 2 && a[0] == 0x08 && a[1] == 0x00)) clean.push_back(a);
    alps.swap(clean);

    for (std::size_t kp : {5916u, 700u}) {
        INFO("kpayload " << kp);
        tools::BbpPackerConfig pcfg;
        pcfg.kpayload_bytes = kp;
        std::uint16_t counter = 0;
        // spread the packets over two PLPs: even index -> PLP 0, odd -> PLP 2
        std::vector<Bytes> a0, a2;
        for (std::size_t i = 0; i < alps.size(); ++i) (i % 2 ? a2 : a0).push_back(alps[i]);
        const auto b0 = packBbps(a0, pcfg, counter);
        const auto b2 = packBbps(a2, pcfg, counter);
        std::uint16_t inner_seq = 0, outer_seq = 0;
        std::vector<Bytes> inner;
        for (std::size_t i = 0; i < std::max(b0.size(), b2.size()); ++i) {
            if (i < b0.size()) for (const Bytes& p : tools::bppsPackets(0, b0[i], inner_seq, 1)) inner.push_back(p);
            if (i < b2.size()) for (const Bytes& p : tools::bppsPackets(2, b2[i], inner_seq, 1)) inner.push_back(p);
        }
        std::vector<std::pair<std::int64_t, Bytes>> recs;
        std::int64_t ts = 1'600'000'000'000'000'000LL;
        for (const Bytes& t : tools::stltpTunnel(inner, outer_seq)) {
            recs.push_back({ts, ethernet(ipv4Udp(addr(192, 168, 59, 100), addr(239, 232, 11, 51), 58532, 30000, t))});
            ts += 100'000;
        }
        const std::string in = writePcap(recs, 1, "rt-stltp-" + std::to_string(kp) + ".pcap");
        const std::string out = tempPath("rt-alp-" + std::to_string(kp) + ".pcap");
        const std::string bbp = tempPath("rt-bbp-" + std::to_string(kp) + ".pcap");

        tools::Stltp2AlpOptions opts;
        opts.input = in;
        opts.output = out;
        opts.bbp_output = bbp;
        tools::Stltp2AlpResult res;
        std::string err;
        REQUIRE(tools::convertStltpToAlp(opts, res, &err) == 0);
        CHECK(res.deframer.pending_mismatch == 0);
        CHECK(res.deframer.alp_invalid == 0);
        CHECK(res.bbp_per_plp[0] == b0.size());
        CHECK(res.bbp_per_plp[2] == b2.size());
        CHECK(res.alp_written == alps.size());
        CHECK(res.bbp_written == b0.size() + b2.size());

        // ALP output: all packets, per-PLP order preserved (PLP 0 and 2 interleave BBP by BBP)
        const auto got = readAll(out, 289);
        REQUIRE(got.size() == alps.size());
        std::vector<Bytes> g0, g2;
        for (const Bytes& g : got) {
            // classify by looking the packet up in the sources (packets are unique by construction)
            bool in0 = false;
            for (const Bytes& x : a0) if (x == g) { in0 = true; break; }
            (in0 ? g0 : g2).push_back(g);
        }
        CHECK(g0 == a0);
        CHECK(g2 == a2);

        // BBP container output round-trips the packed BBPs
        const auto bb = readAll(bbp, pcap::kBbpContainerLinkType);
        REQUIRE(bb.size() == b0.size() + b2.size());
        pcap::BbpContainerRecord rec;
        REQUIRE(pcap::parseBbpContainerRecord(ByteView(bb[0].data(), bb[0].size()), rec));
        CHECK(rec.plp_id == 0);
        CHECK(Bytes(rec.bbp.begin(), rec.bbp.end()) == b0[0]);

        // PLP filter
        tools::Stltp2AlpOptions only2 = opts;
        only2.bbp_output.reset();
        only2.plp = 2;
        tools::Stltp2AlpResult r2;
        REQUIRE(tools::convertStltpToAlp(only2, r2, &err) == 0);
        CHECK(readAll(out, 289) == a2);

        std::remove(in.c_str());
        std::remove(out.c_str());
        std::remove(bbp.c_str());
    }
}

TEST_CASE("stltp2alp rejects a non-STLTP capture", "[roundtrip][stltp2alp]") {
    const std::string in = writePcap({{0, Bytes{0x05, 0x4B, 0x45}}}, 289, "rt-notstltp.pcap");
    tools::Stltp2AlpOptions opts;
    opts.input = in;
    opts.output = tempPath("rt-notstltp-out.pcap");
    tools::Stltp2AlpResult res;
    std::string err;
    CHECK(tools::convertStltpToAlp(opts, res, &err) == 3);
    CHECK(err.find("STLTP") != std::string::npos);
    opts.input = "/nonexistent.pcap";
    CHECK(tools::convertStltpToAlp(opts, res, &err) == 2);
    std::remove(in.c_str());
}
