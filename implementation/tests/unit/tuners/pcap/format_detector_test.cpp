#include <catch2/catch_test_macros.hpp>

#include <cstdio>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/tuners/pcap/format_detector.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
pcap::Detection detectFile(const std::string& path, pcap::DetectorConfig cfg = {}) {
    auto r = pcap::PcapReader::open(path);
    REQUIRE(r);
    return pcap::detectCaptureFormat(*r, cfg);
}
}  // namespace

TEST_CASE("DLT 289 records are ALP", "[io][detect]") {
    const std::string p = writePcap({{0, Bytes{0x05, 0x4B, 0x45}}, {1000, Bytes{0x05, 0x4B, 0x45}}}, 289, "alp.pcap");
    const auto d = detectFile(p);
    CHECK(d.format == pcap::CaptureFormat::Alp);
    CHECK(d.alp_records == 2);
    std::remove(p.c_str());
}

TEST_CASE("BBP container records are BBP", "[io][detect]") {
    const Bytes bbp = basebandPacket(0, Bytes{0x05, 0x01, 0x45}, 32);
    const Bytes rec = pcap::makeBbpContainerRecord(2, 0, ByteView(bbp.data(), bbp.size()));
    const std::string p = writePcap({{0, rec}}, pcap::kBbpContainerLinkType, "bbp.pcap");
    const auto d = detectFile(p);
    CHECK(d.format == pcap::CaptureFormat::Bbp);
    pcap::BbpContainerRecord parsed;
    REQUIRE(pcap::parseBbpContainerRecord(ByteView(rec.data(), rec.size()), parsed));
    CHECK(parsed.plp_id == 2);
    CHECK(parsed.bbp.size() == 32);
    std::remove(p.c_str());
}

TEST_CASE("plain IP with LLS/LCT/MMTP is IP bypass; LAN chatter alone is Unsupported", "[io][detect]") {
    std::vector<std::pair<std::int64_t, Bytes>> recs;
    const std::uint32_t src = addr(10, 5, 120, 103);
    recs.push_back({0, ethernet(ipv4Udp(src, addr(239, 255, 255, 250), 1900, 1900, Bytes(40, 'N')))});
    recs.push_back({1, ethernet(ipv4Udp(src, addr(224, 0, 0, 251), 5353, 5353, Bytes(40, 'M')))});
    const std::string lan = writePcap(recs, 1, "lan.pcap");
    CHECK(detectFile(lan).format == pcap::CaptureFormat::Unsupported);
    std::remove(lan.c_str());

    recs.push_back({2, ethernet(ipv4Udp(src, addr(224, 0, 23, 60), 50000, 4937, llsPayload()))});
    const std::string lls = writePcap(recs, 1, "lls.pcap");
    CHECK(detectFile(lls).format == pcap::CaptureFormat::IpBypass);
    std::remove(lls.c_str());

    recs.pop_back();
    recs.push_back({2, ethernet(ipv4Udp(src, addr(239, 255, 24, 1), 50000, 8000, lctPacket(1, 5, 128, Bytes(20, 1))))});
    const std::string lct = writePcap(recs, 1, "lct.pcap");
    const auto d = detectFile(lct);
    CHECK(d.format == pcap::CaptureFormat::IpBypass);
    REQUIRE_FALSE(d.flows.empty());
    std::remove(lct.c_str());

    recs.pop_back();
    recs.push_back({2, ethernet(ipv4Udp(src, addr(239, 255, 24, 3), 50000, 8000, mmtpPacket(20, 1, Bytes(10, 2))))});
    const std::string mmt = writePcap(recs, 1, "mmt.pcap");
    CHECK(detectFile(mmt).format == pcap::CaptureFormat::IpBypass);
    std::remove(mmt.c_str());
}

TEST_CASE("STLTP tunnel wins over other UDP and needs sequence continuity", "[io][detect]") {
    std::vector<std::pair<std::int64_t, Bytes>> recs;
    const std::uint32_t src = addr(192, 168, 59, 100);
    std::uint16_t seq = 100;
    for (int i = 0; i < 20; ++i) {
        recs.push_back({i * 1000, ethernet(ipv4Udp(src, addr(239, 232, 11, 51), 58532, 30000,
                                                   rtp(97, false, seq++, 0x1000, 0, Bytes(100, 0x45))))});
    }
    recs.push_back({30000, ethernet(ipv4Udp(src, addr(224, 0, 23, 60), 50000, 4937, llsPayload()))});
    const std::string p = writePcap(recs, 1, "stltp.pcap");
    const auto d = detectFile(p);
    CHECK(d.format == pcap::CaptureFormat::Stltp);
    REQUIRE(d.tunnel_pt.has_value());
    CHECK(*d.tunnel_pt == 97);

    pcap::DetectorConfig strict;
    strict.stltp_min_packets = 50;
    CHECK(detectFile(p, strict).format == pcap::CaptureFormat::IpBypass);  // LLS present, tunnel not confirmed
    std::remove(p.c_str());
}

TEST_CASE("corpus verdicts match the spec", "[io][detect][corpus]") {
    struct Case { const char* file; pcap::CaptureFormat format; };
    const Case cases[] = {
        {"alp-sample1.pcap", pcap::CaptureFormat::Alp},
        {"alp-sample2.pcap", pcap::CaptureFormat::Alp},
        {"ch11-25-slt.pcap", pcap::CaptureFormat::Alp},
        {"lls-tables-alp.pcap", pcap::CaptureFormat::Alp},
        {"mp4-ftyp-styp-sidx.pcap", pcap::CaptureFormat::Alp},
        {"dektec/ATSC3-STLTP/BWI_20230307_1742.pcap", pcap::CaptureFormat::Stltp},
        {"dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap", pcap::CaptureFormat::IpBypass},
        {"dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap", pcap::CaptureFormat::IpBypass},
    };
    bool any = false;
    for (const Case& c : cases) {
        const std::string p = corpus(c.file);
        if (!exists(p)) continue;
        any = true;
        INFO(c.file);
        const auto d = detectFile(p);
        INFO(d.summary());
        CHECK(d.format == c.format);
        if (c.format == pcap::CaptureFormat::Stltp) {
            CHECK(d.tunnel_pt == std::optional<std::uint8_t>(97));
            CHECK(d.marker_text.find("stltpAddress=239.232.11.51") != std::string::npos);
        }
    }
    if (!any) SKIP("corpus not available");
}
