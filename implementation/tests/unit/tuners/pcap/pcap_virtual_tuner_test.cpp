#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;
using namespace std::chrono_literals;
using Kind = RecordingSink::Unit::Kind;

namespace {
pcap::VirtualTunerConfig cfgFor(const std::string& path, pcap::Pacing pacing = pcap::Pacing::NonRealTime) {
    pcap::VirtualTunerConfig c;
    c.path = path;
    c.pacing = pacing;
    return c;
}
/// Ethernet/UDP records: `n` LLS datagrams spaced `gap_ns` apart starting at `t0`.
std::vector<std::pair<std::int64_t, Bytes>> ipRecords(int n, std::int64_t gap_ns, std::int64_t t0 = 1'700'000'000'000'000'000LL) {
    std::vector<std::pair<std::int64_t, Bytes>> recs;
    for (int i = 0; i < n; ++i) {
        Bytes payload = llsPayload();
        payload.push_back(static_cast<std::uint8_t>(i));
        recs.push_back({t0 + i * gap_ns, ethernet(ipv4Udp(addr(10, 0, 0, 1), addr(224, 0, 23, 60), 5000, 4937, payload))});
    }
    return recs;
}
struct TempFile {
    std::string path;
    ~TempFile() { std::remove(path.c_str()); }
};
}  // namespace

TEST_CASE("unsupported capture fails to start with NoAtscContent", "[io][tuner]") {
    TempFile f{writePcap({{0, ethernet(ipv4Udp(addr(10, 0, 0, 1), addr(239, 255, 255, 250), 1900, 1900, Bytes(20, 'x')))}}, 1, "vt-lan.pcap")};
    pcap::VirtualTuner t(cfgFor(f.path));
    CHECK(t.openError() == service::TunerError::NoAtscContent);
    CHECK(t.mode() == pcap::CaptureFormat::Unsupported);
    RecordingSink sink;
    CHECK(t.start(sink, t.output()) == service::TunerError::NoAtscContent);
    pcap::VirtualTuner missing(cfgFor("/nonexistent/x.pcap"));
    CHECK(missing.openError() == service::TunerError::FileNotFound);
    pcap::VirtualTunerConfig forced = cfgFor(f.path);
    forced.force_mode = pcap::CaptureFormat::IpBypass;
    pcap::VirtualTuner t2(forced);
    REQUIRE(t2.openError() == service::TunerError::None);
    CHECK(t2.output() == service::TunerOutput::Ip);
    REQUIRE(t2.start(sink, t2.output()) == service::TunerError::None);
    REQUIRE(t2.waitFinished(5s));
    CHECK(sink.count(Kind::Ip) == 1);
}

TEST_CASE("ALP sample is delivered as ALP packets with events", "[io][tuner][corpus]") {
    const std::string p = corpus("alp-sample1.pcap");
    if (!exists(p)) SKIP("corpus not available");
    pcap::VirtualTuner t(cfgFor(p));
    REQUIRE(t.openError() == service::TunerError::None);
    CHECK(t.output() == service::TunerOutput::Alp);
    CHECK(t.mode() == pcap::CaptureFormat::Alp);
    CHECK_FALSE(t.l1().has_value());
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(5s));
    const auto units = sink.units();
    REQUIRE(units.size() == 17);
    CHECK(units[0].kind == Kind::Alp);
    CHECK(units[0].plp == io::kUnknownPlp);
    CHECK(units[1].bytes.size() == 1357);
    CHECK(units[1].bytes[0] == 0x05);
    CHECK(units[1].bytes[1] == 0x4B);
    CHECK(sink.hasEvent(service::TunerEventType::FormatDetected));
    CHECK(sink.hasEvent(service::TunerEventType::Lock));
    CHECK(sink.hasEvent(service::TunerEventType::EndOfStream));
    CHECK(t.stats().delivered_alp == 17);
    CHECK(t.stats().records == 17);
    // capture timestamps preserved and monotonic in non-real-time mode
    for (std::size_t i = 1; i < units.size(); ++i) {
        CHECK(units[i].time.capture_ns >= units[i - 1].time.capture_ns);
        CHECK(units[i].time.t_ns >= units[i - 1].time.t_ns);
    }
}

TEST_CASE("non-real-time mode keeps the captured timeline and clamps gaps", "[io][tuner][pacing]") {
    auto recs = ipRecords(4, 10'000'000);                  // 10 ms apart
    recs[2].first = recs[1].first - 5'000'000;             // negative gap
    recs[3].first = recs[1].first + 100'000'000'000LL;     // 100 s jump
    TempFile f{writePcap(recs, 1, "vt-nrt.pcap")};
    auto cfg = cfgFor(f.path);
    cfg.max_gap_ns = 50'000'000;
    pcap::VirtualTuner t(cfg);
    REQUIRE(t.openError() == service::TunerError::None);
    RecordingSink sink;
    const auto host0 = std::chrono::steady_clock::now();
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(5s));
    const auto elapsed = std::chrono::steady_clock::now() - host0;
    CHECK(elapsed < 2s);  // no sleeping in non-real-time mode
    const auto u = sink.units();
    REQUIRE(u.size() == 4);
    CHECK(u[0].time.t_ns == recs[0].first);
    CHECK(u[1].time.t_ns == recs[0].first + 10'000'000);
    CHECK(u[2].time.t_ns == u[1].time.t_ns);                 // negative gap -> 0
    CHECK(u[3].time.t_ns == u[2].time.t_ns + 50'000'000);    // clamped to max_gap
    CHECK(u[3].time.capture_ns == recs[3].first);
    CHECK(sink.eventCount(service::TunerEventType::TimeJump) == 1);
    CHECK(t.stats().time_jumps == 1);
    CHECK(t.positionNs() == 60'000'000);
}

TEST_CASE("real-time mode honours record spacing and speed", "[io][tuner][pacing]") {
    TempFile f{writePcap(ipRecords(6, 40'000'000), 1, "vt-rt.pcap")};  // 5 gaps of 40 ms = 200 ms
    for (double speed : {1.0, 4.0}) {
        auto cfg = cfgFor(f.path, pcap::Pacing::RealTime);
        cfg.speed = speed;
        pcap::VirtualTuner t(cfg);
        REQUIRE(t.openError() == service::TunerError::None);
        RecordingSink sink;
        const auto host0 = std::chrono::steady_clock::now();
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(t.waitFinished(5s));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - host0);
        const auto u = sink.units();
        REQUIRE(u.size() == 6);
        const auto expected = std::chrono::milliseconds(static_cast<long>(200.0 / speed));
        INFO("speed " << speed << " elapsed " << elapsed.count() << " ms");
        CHECK(elapsed >= expected - 5ms);
        CHECK(elapsed < expected + 500ms);
        // t_ns is host time at the deadline; deliveries must not be early relative to it
        for (std::size_t i = 1; i < u.size(); ++i) {
            CHECK(u[i].host_ns + 2'000'000 >= u[i].time.t_ns);
            CHECK(u[i].time.t_ns - u[i - 1].time.t_ns == static_cast<std::int64_t>(40'000'000 / speed));
        }
    }
}

TEST_CASE("pause freezes delivery and resumes without a burst", "[io][tuner][pacing]") {
    TempFile f{writePcap(ipRecords(8, 30'000'000), 1, "vt-pause.pcap")};
    pcap::VirtualTuner t(cfgFor(f.path, pcap::Pacing::RealTime));
    REQUIRE(t.openError() == service::TunerError::None);
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(sink.waitUnits(2, 2s));
    t.pause(true);
    std::this_thread::sleep_for(150ms);
    const std::size_t while_paused = sink.units().size();
    std::this_thread::sleep_for(150ms);
    CHECK(sink.units().size() == while_paused);  // nothing delivered while paused
    t.pause(false);
    REQUIRE(t.waitFinished(5s));
    const auto u = sink.units();
    REQUIRE(u.size() == 8);
    // after resume the spacing between consecutive deliveries stays ~30 ms (no catch-up burst)
    for (std::size_t i = while_paused + 1; i < u.size(); ++i) {
        CHECK(u[i].host_ns - u[i - 1].host_ns >= 20'000'000);
    }
}

TEST_CASE("loop wraps with an event and start_offset/duration select a window", "[io][tuner]") {
    TempFile f{writePcap(ipRecords(10, 10'000'000), 1, "vt-loop.pcap")};
    {
        auto cfg = cfgFor(f.path);
        cfg.loop = true;
        pcap::VirtualTuner t(cfg);
        RecordingSink sink;
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(sink.waitUnits(35, 5s));
        t.stop();
        CHECK(sink.eventCount(service::TunerEventType::Wrap) >= 3);
        CHECK_FALSE(sink.hasEvent(service::TunerEventType::EndOfStream));  // stopped, not ended
        const auto u = sink.units();
        for (std::size_t i = 1; i < u.size(); ++i) CHECK(u[i].time.t_ns >= u[i - 1].time.t_ns);  // continuous timeline
        CHECK(t.stats().wraps >= 3);
    }
    {
        auto cfg = cfgFor(f.path);
        cfg.start_offset_ns = 25'000'000;  // skip records 0..2
        cfg.duration_ns = 30'000'000;      // keep records at 30,40,50 ms
        pcap::VirtualTuner t(cfg);
        RecordingSink sink;
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(t.waitFinished(5s));
        const auto u = sink.units();
        REQUIRE(u.size() == 3);
        CHECK(u[0].bytes.back() == 3);
        CHECK(u[2].bytes.back() == 5);
    }
}

TEST_CASE("IP bypass delivers UDP datagrams, applies filters and tags the sidecar PLP", "[io][tuner]") {
    auto recs = ipRecords(3, 1'000'000);
    recs.push_back({recs.back().first + 1, ethernet(ipv4Udp(addr(10, 0, 0, 1), addr(239, 255, 24, 1), 5000, 8000, lctPacket(1, 1, 128, Bytes(8, 0))))});
    Bytes arp = ethernet(recs[0].second);  // garbage non-IP frame
    arp[12] = 0x08; arp[13] = 0x06;
    recs.push_back({recs.back().first + 1, arp});
    TempFile f{writePcap(recs, 1, "vt-ip.pcap")};
    const std::string xa3 = f.path.substr(0, f.path.size() - 5) + ".xa3";
    {
        std::FILE* x = std::fopen(xa3.c_str(), "w");
        REQUIRE(x);
        std::fputs("<ModulationParameters><Atsc3><Subframe><Plp plp_id=\"7\" modulation=\"qam64\"><Source><PcapFile filename=\"C:\\\\caps\\\\vt-ip.pcap\"/></Source></Plp></Subframe>"
                   "<Signaling><Lmt><Plp plp_id=\"7\"><Multicast><Src ip0=\"10\" ip1=\"0\" ip2=\"0\" ip3=\"1\" port=\"5000\"/><Dst ip0=\"239\" ip1=\"255\" ip2=\"24\" ip3=\"1\" port=\"8000\"/></Multicast></Plp></Lmt></Signaling></Atsc3></ModulationParameters>", x);
        std::fclose(x);
    }
    TempFile fx{xa3};
    {
        pcap::VirtualTuner t(cfgFor(f.path));
        REQUIRE(t.openError() == service::TunerError::None);
        CHECK(t.mode() == pcap::CaptureFormat::IpBypass);
        CHECK(t.output() == service::TunerOutput::Ip);
        REQUIRE(t.sidecar());
        const auto l1 = t.l1();
        REQUIRE(l1);
        REQUIRE(l1->plps.size() == 1);
        CHECK(l1->plps[0].modulation == "qam64");
        CHECK(l1->lmt.size() == 1);
        RecordingSink sink;
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(t.waitFinished(5s));
        const auto u = sink.units();
        REQUIRE(u.size() == 4);
        for (const auto& x : u) {
            CHECK(x.kind == Kind::Ip);
            CHECK(x.plp == 7);
            CHECK(x.bytes[0] == 0x45);
        }
        CHECK(t.stats().dropped_non_ip == 1);
    }
    {
        auto cfg = cfgFor(f.path);
        cfg.sidecar = "off";
        cfg.ip_filter.push_back(pcap::IpFilter{addr(239, 255, 24, 1), 0});
        pcap::VirtualTuner t(cfg);
        REQUIRE(t.openError() == service::TunerError::None);
        CHECK_FALSE(t.sidecar());
        RecordingSink sink;
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(t.waitFinished(5s));
        REQUIRE(sink.units().size() == 1);
        CHECK(sink.units()[0].plp == io::kUnknownPlp);
        CHECK(t.stats().dropped_filtered == 3);
    }
}

TEST_CASE("BBP container capture is delivered as Baseband Packets", "[io][tuner]") {
    const Bytes bbp0 = basebandPacket(0, Bytes(100, 1), 200);
    const Bytes bbp1 = basebandPacket(8191, Bytes(50, 2), 200);
    std::vector<std::pair<std::int64_t, Bytes>> recs{
        {0, pcap::makeBbpContainerRecord(1, 0, ByteView(bbp0.data(), bbp0.size()))},
        {1000, pcap::makeBbpContainerRecord(2, pcap::kBbpFlagError, ByteView(bbp1.data(), bbp1.size()))}};
    TempFile f{writePcap(recs, pcap::kBbpContainerLinkType, "vt-bbp.pcap")};
    pcap::VirtualTuner t(cfgFor(f.path));
    REQUIRE(t.openError() == service::TunerError::None);
    CHECK(t.output() == service::TunerOutput::Bbp);
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(5s));
    const auto u = sink.units();
    REQUIRE(u.size() == 2);
    CHECK(u[0].kind == Kind::Bbp);
    CHECK(u[0].plp == 1);
    CHECK(u[0].bytes == bbp0);
    CHECK(u[1].plp == 2);
    const auto l1 = t.l1();
    REQUIRE(l1);
    CHECK(l1->plps.size() == 2);
}

TEST_CASE("synthetic STLTP capture yields BBPs through the embedded demux", "[io][tuner][stltp]") {
    std::uint16_t inner_seq = 0, outer_seq = 0;
    std::vector<Bytes> inner;
    std::vector<Bytes> bbps;
    for (int i = 0; i < 4; ++i) {
        bbps.push_back(basebandPacket(static_cast<std::uint16_t>(i), Bytes(static_cast<std::size_t>(4500 + i * 100), static_cast<std::uint8_t>(0x10 + i)), 5916));
        for (const Bytes& p : bppsPackets(static_cast<std::uint8_t>(i % 2), bbps.back(), inner_seq, 1)) inner.push_back(p);
    }
    std::vector<std::pair<std::int64_t, Bytes>> recs;
    std::int64_t ts = 0;
    for (const Bytes& t : stltpTunnel(inner, outer_seq, 1360, 0)) {
        recs.push_back({ts, ethernet(ipv4Udp(addr(192, 168, 59, 100), addr(239, 232, 11, 51), 58532, 30000, t))});
        ts += 500'000;
    }
    // a non-tunnel datagram mixed in
    recs.push_back({ts, ethernet(ipv4Udp(addr(192, 168, 59, 100), addr(224, 0, 23, 60), 1, 4937, llsPayload()))});
    TempFile f{writePcap(recs, 1, "vt-stltp.pcap")};
    for (bool mixed : {false, true}) {
        auto cfg = cfgFor(f.path);
        cfg.allow_mixed = mixed;
        pcap::VirtualTuner t(cfg);
        REQUIRE(t.openError() == service::TunerError::None);
        CHECK(t.mode() == pcap::CaptureFormat::Stltp);
        CHECK(t.output() == service::TunerOutput::Bbp);
        RecordingSink sink;
        REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
        REQUIRE(t.waitFinished(5s));
        REQUIRE(sink.count(Kind::Bbp) == 4);
        const auto u = sink.units();
        std::size_t k = 0;
        for (const auto& x : u) {
            if (x.kind != Kind::Bbp) continue;
            CHECK(x.plp == k % 2);
            CHECK(x.bytes == bbps[k]);
            ++k;
        }
        CHECK(sink.count(Kind::Ip) == (mixed ? 1u : 0u));
        CHECK(t.stats().dropped_mixed == (mixed ? 0u : 1u));
        const auto l1 = t.l1();
        REQUIRE(l1);
        CHECK(l1->plps.size() == 2);
    }
}

TEST_CASE("BWI STLTP capture: first seconds decode to BBPs for PLPs 0-2", "[io][tuner][stltp][corpus]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    auto cfg = cfgFor(p);
    cfg.duration_ns = 2'000'000'000;
    pcap::VirtualTuner t(cfg);
    REQUIRE(t.openError() == service::TunerError::None);
    CHECK(t.output() == service::TunerOutput::Bbp);
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(60s));
    const auto u = sink.units();
    REQUIRE(u.size() > 500);
    std::size_t per_plp[3] = {0, 0, 0};
    for (const auto& x : u) {
        REQUIRE(x.kind == Kind::Bbp);
        REQUIRE(x.plp <= 2);
        CHECK(x.bytes.size() == 5916);
        CHECK((x.bytes[0] & 0x80) != 0);  // MODE=1
        ++per_plp[x.plp];
    }
    CHECK(per_plp[0] > 0);
    CHECK(per_plp[1] > 0);
    CHECK(per_plp[2] > per_plp[1]);
    CHECK(t.stats().dropped_non_udp == 1);  // the ICMP marker packet
    const auto l1 = t.l1();
    REQUIRE(l1);
    CHECK(l1->plps.size() == 3);
}

TEST_CASE("UniSoft raw-IP capture: IP bypass with PLP 0 from the DekTec sidecar", "[io][tuner][corpus]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    auto cfg = cfgFor(p);
    cfg.duration_ns = 500'000'000;
    pcap::VirtualTuner t(cfg);
    REQUIRE(t.openError() == service::TunerError::None);
    CHECK(t.mode() == pcap::CaptureFormat::IpBypass);
    REQUIRE(t.sidecar());
    const auto l1 = t.l1();
    REQUIRE(l1);
    CHECK(l1->lmt.size() == 4);
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(60s));
    const auto u = sink.units();
    REQUIRE(u.size() > 100);
    bool saw_lls = false;
    for (const auto& x : u) {
        CHECK(x.kind == Kind::Ip);
        CHECK(x.plp == 0);
        io::Ipv4Udp h;
        REQUIRE(io::parseIpv4Udp(ByteView(x.bytes.data(), x.bytes.size()), h));
        if (h.dst == 0xE000173Cu) saw_lls = true;
    }
    CHECK(saw_lls);
}

TEST_CASE("max_records delivers exactly the first N records", "[io][tuner]") {
    TempFile f{writePcap(ipRecords(10, 1'000'000), 1, "vt-limit.pcap")};
    pcap::VirtualTunerConfig c = cfgFor(f.path);
    c.max_records = 4;
    pcap::VirtualTuner t(c);
    REQUIRE(t.openError() == service::TunerError::None);
    RecordingSink sink;
    REQUIRE(t.start(sink, t.output()) == service::TunerError::None);
    REQUIRE(t.waitFinished(5s));
    t.stop();
    CHECK(t.stats().records == 4);
    CHECK(t.stats().delivered_ip == 4);
    CHECK(sink.count(Kind::Ip) == 4);
    CHECK(t.finished());
}
