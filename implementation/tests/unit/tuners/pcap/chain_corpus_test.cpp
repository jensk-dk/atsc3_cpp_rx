#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "atsc3/alp/decap.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/link/signaling.hpp"
#include "atsc3/phy/bbp.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;
using namespace std::chrono_literals;

namespace {
// Tuner (STLTP) -> BBP deframer -> ALP decap -> link signaling, in-process.
class ChainSink final : public service::ITunerSink {
public:
    ChainSink()
        : link_({}),
          decap_([this] {
              alp::Callbacks cb;
              cb.onIpv4 = [this](std::uint8_t, ByteView ip, std::optional<std::uint8_t>) {
                  ++ipv4;
                  io::Ipv4Udp h;
                  if (io::parseIpv4Udp(ip, h) && h.udp && h.dst == 0xE000173Cu) ++lls;
              };
              cb.onSignaling = [this](std::uint8_t plp, const alp::SignalingInfo& i, ByteView t) { link_.onSignaling(plp, i, t); };
              cb.onEvent = [this](const alp::Event&) { ++events; };
              return cb;
          }()),
          deframer_([this](std::uint8_t plp, ByteView alp) {
              ++alps;
              decap_.feed(plp, alp);
          }) {}
    void onBbp(const io::BasebandPacket& p) override {
        ++bbps;
        deframer_.feed(p.plp_id, p.bytes, p.error);
    }
    void onAlp(const io::AlpPacket&) override {}
    void onIp(const io::IpDatagram&) override {}
    void onTunerEvent(const service::TunerEvent&) override {}

    std::size_t bbps = 0, alps = 0, ipv4 = 0, lls = 0, events = 0;
    const phy::DeframerStats& deframer() const { return deframer_.stats(); }
    const alp::Stats& decap() const { return decap_.stats(); }
    const link::LinkSignaling& link() const { return link_; }

private:
    link::LinkSignaling link_;
    alp::Decap decap_;
    phy::BbpDeframer deframer_;
};
}  // namespace

TEST_CASE("BWI STLTP feed decodes through deframer, ALP and link signaling", "[io][phy][alp][link][corpus]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    pcap::VirtualTunerConfig cfg;
    cfg.path = p;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 3'000'000'000;
    pcap::VirtualTuner tuner(cfg);
    REQUIRE(tuner.openError() == service::TunerError::None);
    ChainSink sink;
    REQUIRE(tuner.start(sink, tuner.output()) == service::TunerError::None);
    REQUIRE(tuner.waitFinished(120s));
    INFO("bbps " << sink.bbps << " alps " << sink.alps << " ipv4 " << sink.ipv4 << " lls " << sink.lls);
    CHECK(sink.bbps > 1000);
    CHECK(sink.alps > 3000);
    CHECK(sink.ipv4 > 3000);
    CHECK(sink.lls > 5);
    CHECK(sink.deframer().pending_mismatch == 0);
    CHECK(sink.deframer().alp_invalid == 0);
    CHECK(sink.deframer().bbp_invalid == 0);
    CHECK(sink.deframer().pointer_no_start > 50);
    CHECK(sink.decap().invalid == 0);
    CHECK(sink.decap().truncated == 0);
    CHECK(sink.decap().zero_length == 0);
    CHECK(sink.decap().signaling > 5);
    // The LMT arrives on every PLP that carries LLS; version 2 with three PLPs (as in the small samples)
    const auto& ls = sink.link();
    CHECK(ls.stats().lmt_new >= 1);
    bool found = false;
    for (std::uint8_t plp = 0; plp < 3; ++plp) {
        if (const auto lmt = ls.lmt(plp)) {
            found = true;
            CHECK(lmt->version == 2);
            CHECK(lmt->plps.size() == 3);
        }
    }
    CHECK(found);
    const auto e = ls.plpMap().lookup(addr(239, 255, 45, 1), 5002);
    REQUIRE(e);
    CHECK(e->plp_id == 1);
    const auto e2 = ls.plpMap().lookup(addr(239, 255, 11, 1), 5003);
    REQUIRE(e2);
    CHECK(e2->plp_id == 2);
}
