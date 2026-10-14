#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "atsc3/io/inet.hpp"
#include "atsc3/lls/tables.hpp"
#include "corpus.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("SLT schema examples parse with defaults and lists", "[lls][slt][schema]") {
    const std::string p1 = schema("lls/SLT-Example-20211209.xml");
    if (!exists(p1)) SKIP("schemas not available");
    lls::Slt slt;
    std::string err;
    REQUIRE(lls::parseSlt(readFile(p1), slt, &err));
    CHECK(slt.bsid == std::vector<std::uint16_t>{1234, 5678});
    CHECK(slt.slt_capabilities == std::optional<std::string>("?????"));
    REQUIRE(slt.slt_inet_url.size() == 1);
    CHECK(slt.slt_inet_url[0].url == "http://www.user.com");
    CHECK(slt.slt_inet_url[0].url_type == 1);
    REQUIRE(slt.services.size() == 2);
    const lls::SltService& s = slt.services[0];
    CHECK(s.service_id == 1);
    CHECK(s.global_service_id == std::optional<std::string>("mytv.com/8.2"));
    CHECK(s.protected_flag);
    CHECK(s.major_channel_no == std::optional<std::uint16_t>(8));
    CHECK(s.minor_channel_no == std::optional<std::uint16_t>(1));
    CHECK(s.service_category == 1);
    CHECK(s.short_service_name == std::optional<std::string>("KUSER"));
    CHECK(s.hidden);
    CHECK(s.broadband_access_required);
    CHECK(s.essential == std::optional<bool>(true));
    CHECK(s.drm_system_id == std::optional<std::string>("urn:uuid:1234"));
    REQUIRE(s.simulcast);
    CHECK(s.simulcast->tsid == 4321);
    CHECK(s.simulcast->major == std::optional<std::uint16_t>(2));
    REQUIRE(s.bss);
    CHECK(s.bss->sls_protocol == 1);
    CHECK(s.bss->sls_major_version == 1);
    CHECK(io::ipv4ToString(s.bss->sls_destination_ip) == "1.2.3.4");
    CHECK(s.bss->sls_destination_port == 99);
    CHECK(io::ipv4ToString(s.bss->sls_source_ip) == "5.6.7.8");
    REQUIRE(s.other_bsid.size() == 1);
    CHECK(s.other_bsid[0].bsids == std::vector<std::uint16_t>{4321, 9876});
    CHECK(s.other_bsid[0].type == 1);
    CHECK(s.raw_xml.find("KUSER") != std::string::npos);
    const lls::SltService& s2 = slt.services[1];
    CHECK(s2.service_id == 2);
    CHECK_FALSE(s2.protected_flag);
    CHECK_FALSE(s2.essential);
    CHECK_FALSE(s2.bss);
    CHECK(s2.configuration == lls::SltService::Configuration::Broadcast);

    // prefixed-namespace example with five services and OtherRf/Directional in example 3
    REQUIRE(lls::parseSlt(readFile(schema("lls/SLT-Example2-20211209.xml")), slt, &err));
    CHECK(slt.bsid == std::vector<std::uint16_t>{65535});
    CHECK(slt.services.size() == 5);
    CHECK(slt.services[1].slt_svc_seq_num == 255);
    CHECK(slt.services[3].svc_capabilities == std::optional<std::string>("0700"));
    REQUIRE(lls::parseSlt(readFile(schema("lls/SLT-Example3-20211209.xml")), slt, &err));
    REQUIRE(slt.services.size() == 1);
    REQUIRE(slt.services[0].other_rf.size() == 1);
    const lls::OtherRf& rf = slt.services[0].other_rf[0];
    CHECK(rf.other_bsid_rf == 179);
    CHECK(rf.other_bsid == 532);
    CHECK(rf.lat > 38.9);
    CHECK(rf.elev == 310);
    REQUIRE(rf.directional.size() == 4);
    CHECK(rf.directional[1].heading == 90);
    CHECK(rf.directional[1].haat == std::optional<std::int64_t>(144));
    CHECK(slt.services[0].other_bsid[0].type == 3);
}

TEST_CASE("SLT without namespace (UniSoft 2019) parses", "[lls][slt]") {
    const std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<SLT bsid=\"24\">\n"
                          "<Service globalServiceID=\"urn:atsc:serviceid:usc03\" majorChannelNo=\"24\" minorChannelNo=\"3\" serviceCategory=\"1\" serviceId=\"3\" shortServiceName=\"usc03\" sltSvcSeqNum=\"0\">\n"
                          "<BroadcastSvcSignaling slsDestinationIpAddress=\"239.255.24.3\" slsDestinationUdpPort=\"8000\" slsProtocol=\"1\" slsSourceIpAddress=\"10.5.120.103\"/></Service>\n"
                          "<Service serviceCategory=\"4\" serviceId=\"99\" shortServiceName=\"esg\" sltSvcSeqNum=\"0\"/></SLT>";
    lls::Slt slt;
    REQUIRE(lls::parseSlt(x, slt));
    CHECK(slt.bsid == std::vector<std::uint16_t>{24});
    REQUIRE(slt.services.size() == 2);
    CHECK(slt.services[0].bss->sls_destination_port == 8000);
    CHECK(slt.services[1].service_category == 4);
    CHECK_FALSE(lls::parseSlt("<SystemTime/>", slt));
    CHECK_FALSE(lls::parseSlt("<<<", slt));
}

TEST_CASE("SystemTime, RRT, AEAT and OSMN schema examples parse", "[lls][schema]") {
    if (!exists(schema("lls/SYSTIME-Example-20211208.xml"))) SKIP("schemas not available");
    lls::SystemTime st;
    REQUIRE(lls::parseSystemTime(readFile(schema("lls/SYSTIME-Example-20211208.xml")), st));
    CHECK(st.current_utc_offset == 60);
    CHECK(st.ptp_prepend == 12345);
    CHECK(st.leap59);
    CHECK_FALSE(st.leap61);
    CHECK(st.utc_local_offset_s == std::optional<double>(-28800.0));
    CHECK(st.ds_status);
    CHECK(st.ds_day_of_month == std::optional<std::uint8_t>(16));
    CHECK(st.ds_hour == std::optional<std::uint8_t>(15));
    // on-air form without XML declaration / namespace
    REQUIRE(lls::parseSystemTime("<SystemTime currentUtcOffset=\"37\" utcLocalOffset=\"-PT7H\" dsStatus=\"true\"/>\n", st));
    CHECK(st.current_utc_offset == 37);
    CHECK(st.utc_local_offset_s == std::optional<double>(-25200.0));

    lls::Rrt rrt;
    REQUIRE(lls::parseRrt(readFile(schema("lls/RRT-Example-20210128.xml")), rrt));
    REQUIRE(rrt.tables.size() == 1);
    CHECK(rrt.tables[0].region_identifier == 1);
    CHECK(rrt.tables[0].region_id_text[0].text == "Canada");
    REQUIRE(rrt.tables[0].dimensions.size() == 1);
    CHECK(rrt.tables[0].dimensions[0].graduated);
    CHECK(rrt.tables[0].dimensions[0].ratings[0].abbrev[0].text == "MV");
    CHECK(rrt.tables[0].dimensions[0].ratings[0].text[0].lang == "en");

    lls::Aeat aeat;
    REQUIRE(lls::parseAeat(readFile(schema("sls/emergency/AEAT-example-tornado-alert-bilingual-20210322.xml")), aeat));
    REQUIRE(aeat.aeas.size() == 1);
    const lls::Aea& a = aeat.aeas[0];
    CHECK(a.aea_id == "AEA-2016091113002100");
    CHECK(a.issuer == "KUSR-TV");
    CHECK(a.audience == "public");
    CHECK(a.aea_type == "alert");
    CHECK(a.priority == std::optional<std::uint8_t>(3));
    CHECK(a.wakeup);
    CHECK(a.event_code == std::optional<std::string>("TOR"));
    CHECK(a.event_code_type == std::optional<std::string>("SAME"));
    CHECK(a.effective_ns == std::optional<std::int64_t>(1473624000LL * 1'000'000'000LL));
    REQUIRE(a.locations.size() == 2);
    CHECK(a.locations[1].type == "polygon");
    REQUIRE(a.aea_text.size() == 2);
    CHECK(a.aea_text[1].lang == "es");
    REQUIRE(a.live_media);
    CHECK(a.live_media->bsid == std::vector<std::uint16_t>{47, 23});
    CHECK(a.live_media->service_id == 3);
    REQUIRE(a.media.size() == 2);
    CHECK(a.media[0].url == "AEAtextAudio/AEAtextAudio-en.mp3");
    CHECK(a.media[0].content_length == std::optional<std::uint64_t>(301024));
    CHECK(a.media[1].lang == std::optional<std::string>("es"));
    // the traffic example carries a BOM and aeaTableId
    REQUIRE(lls::parseAeat(readFile(schema("sls/emergency/AEAT-example-traffic-newsdesk-alert-20210322.xml")), aeat));
    CHECK(aeat.aea_table_id == std::optional<std::string>("123456"));
    CHECK(aeat.aeas[0].category == std::optional<std::string>("TRANSIT"));
    CHECK(aeat.aeas[0].sub_audience.empty());
    REQUIRE(lls::parseAeat(readFile(schema("sls/emergency/AEAT-example-subgroup-20210322.xml")), aeat));
    CHECK(aeat.aeas[0].sub_audience.size() == 4);

    lls::Omn omn;
    REQUIRE(lls::parseOmn(readFile(schema("sls/application/ONSCREEN-Example-20210401.xml")), omn));
    REQUIRE(omn.entries.size() == 1);
    CHECK(omn.entries[0].bsid == std::vector<std::uint16_t>{3});
    CHECK(omn.entries[0].service_id == std::optional<std::uint16_t>(5));
    CHECK(omn.entries[0].service_id_range == 4);
    CHECK(omn.entries[0].notification_duration_s == 60.0);
    CHECK_FALSE(omn.entries[0].ksc_flag);
}
