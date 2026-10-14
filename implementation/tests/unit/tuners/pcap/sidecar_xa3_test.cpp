#include <catch2/catch_test_macros.hpp>

#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/sidecar_xa3.hpp"
#include "corpus.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
const char* kXa3 = R"(<?xml version="1.0" encoding="utf-8"?>
<ModulationParameters>
  <Atsc3>
    <General bandwidth="bw_6" minor_version="0" lls_flag="true"/>
    <Preamble fft_size="fft_8k" guard_interval="gi_5_1024"/>
    <Subframe pilot_pattern="pp_3_4">
      <Plp plp_id="0" modulation="qam256" code_rate="cod_9_15" lls_flag="true" layer="core" fec_code_length="ldpc_64k" fec_outer_code="bch" plp_type="non_disp">
        <Source><PcapFile filename="U:\pub\persistent\ATSC3-PCAP\test-capture.pcap" /></Source>
      </Plp>
      <Plp plp_id="3" modulation="qpsk" code_rate="cod_6_15" lls_flag="false" layer="core">
        <Source><PcapFile filename="/data/other.pcap" /></Source>
      </Plp>
    </Subframe>
    <Signaling>
      <Lmt cycle_time="1" gen_mode="from_plp_source">
        <Plp plp_id="0">
          <Multicast><Src ip0="10" ip1="5" ip2="120" ip3="103" port="37766"/><Dst ip0="239" ip1="255" ip2="24" ip3="1" port="8000"/></Multicast>
          <Multicast><Src ip0="10" ip1="5" ip2="120" ip3="103" port="41068"/><Dst ip0="239" ip1="255" ip2="24" ip3="99" port="8000"/></Multicast>
        </Plp>
      </Lmt>
    </Signaling>
  </Atsc3>
  <IqOutput iq_format="rf"/>
</ModulationParameters>)";
}  // namespace

TEST_CASE("xa3 profile parses into L1Info and LMT entries", "[io][sidecar]") {
    std::string err;
    const auto sc = pcap::parseXa3(kXa3, "mem.xa3", &err);
    REQUIRE(sc);
    CHECK(sc->l1.lls_flag);
    CHECK(sc->l1.bandwidth == "bw_6");
    CHECK(sc->l1.fft_size == "fft_8k");
    CHECK(sc->l1.pilot_pattern == "pp_3_4");
    REQUIRE(sc->l1.plps.size() == 2);
    CHECK(sc->l1.plps[0].plp_id == 0);
    CHECK(sc->l1.plps[0].modulation == "qam256");
    CHECK(sc->l1.plps[0].code_rate == "cod_9_15");
    CHECK(sc->l1.plps[0].lls_flag);
    CHECK(sc->l1.plps[1].plp_id == 3);
    CHECK_FALSE(sc->l1.plps[1].lls_flag);
    REQUIRE(sc->l1.lmt.size() == 2);
    CHECK(sc->l1.lmt[0].plp_id == 0);
    CHECK(sc->l1.lmt[0].src_ip == 0x0A057867u);
    CHECK(sc->l1.lmt[0].src_port == 37766);
    CHECK(sc->l1.lmt[1].dst_ip == 0xEFFF1863u);
    CHECK(sc->l1.lmt[1].dst_port == 8000);
    CHECK(sc->l1.lmt_origin == io::LmtOrigin::Sidecar);
    CHECK(sc->plpForCapture("/somewhere/test-capture.pcap") == std::optional<std::uint8_t>(0));
    CHECK(sc->plpForCapture("other.pcap") == std::optional<std::uint8_t>(3));
    CHECK(sc->plpForCapture("unknown.pcap") == std::optional<std::uint8_t>(0));  // first PLP fallback
}

TEST_CASE("malformed or foreign XML is rejected without throwing", "[io][sidecar]") {
    std::string err;
    CHECK_FALSE(pcap::parseXa3("<nope/>", "x", &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(pcap::parseXa3("<<<", "x", &err));
    CHECK_FALSE(pcap::loadXa3("/nonexistent/file.xa3", &err));
}

TEST_CASE("corpus .xa3 is found next to the UniSoft capture", "[io][sidecar][corpus]") {
    const std::string cap = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(cap)) SKIP("corpus not available");
    const auto path = pcap::findXa3Sidecar(cap);
    REQUIRE(path);
    CHECK(path->find("UniSoft-3xROUTE-24-October-2019.xa3") != std::string::npos);
    const auto sc = pcap::loadXa3(*path);
    REQUIRE(sc);
    REQUIRE(sc->l1.plps.size() == 1);
    CHECK(sc->l1.plps[0].modulation == "qam256");
    CHECK(sc->l1.lmt.size() == 4);
    CHECK(io::ipv4ToString(sc->l1.lmt[0].dst_ip) == "239.255.24.1");
    CHECK(sc->plpForCapture(cap) == std::optional<std::uint8_t>(0));
    CHECK_FALSE(pcap::findXa3Sidecar(corpus("alp-sample1.pcap")));
}
