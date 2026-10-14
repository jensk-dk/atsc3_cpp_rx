#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "atsc3/sls/fragments.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
const char* kBwiStsid =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><S-TSID xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\" "
    "xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/\" xmlns:fdt=\"urn:ietf:params:xml:ns:fdt\">"
    "<RS><LS tsi=\"100\"><SrcFlow rt=\"true\"><EFDT><FDT-Instance Expires=\"4294967295\" afdt:efdtVersion=\"1\" "
    "afdt:maxTransportSize=\"4500000\" afdt:fileTemplate=\"video-$TOI$.mp4v\"><fdt:File TOI=\"1\" Content-Location=\"video-init.mp4\">"
    "</fdt:File></FDT-Instance></EFDT><ContentInfo><MediaInfo contentType=\"video\" repId=\"Video1_1\"></MediaInfo></ContentInfo>"
    "<Payload codePoint=\"8\" formatId=\"1\" frag=\"0\" order=\"true\"></Payload></SrcFlow></LS>"
    "<LS tsi=\"300\"><SrcFlow rt=\"true\"><EFDT><FDT-Instance Expires=\"4294967295\" afdt:efdtVersion=\"1\" "
    "afdt:fileTemplate=\"d4_4-$TOI$.m4s\"><fdt:File TOI=\"1\" Content-Location=\"d4_4-init.mp4\"/></FDT-Instance></EFDT>"
    "<ContentInfo><MediaInfo contentType=\"subtitles\" repId=\"d4_4\"/></ContentInfo><Payload codePoint=\"8\" formatId=\"1\" frag=\"0\" order=\"true\"/>"
    "</SrcFlow></LS></RS></S-TSID>";
}  // namespace

TEST_CASE("S-TSID: on-air Sinclair form (RS without addresses, EFDT with fileTemplate)", "[sls][stsid]") {
    sls::Stsid st;
    std::string err;
    REQUIRE(sls::parseStsid(kBwiStsid, st, &err));
    REQUIRE(st.rs.size() == 1);
    CHECK_FALSE(st.rs[0].dst_ip);
    CHECK_FALSE(st.rs[0].dst_port);
    REQUIRE(st.rs[0].ls.size() == 2);
    const sls::Ls& v = st.rs[0].ls[0];
    CHECK(v.tsi == 100);
    REQUIRE(v.src_flow);
    CHECK(v.src_flow->rt);
    REQUIRE(v.src_flow->efdt);
    CHECK(v.src_flow->efdt->file_template == "video-$TOI$.mp4v");
    CHECK(v.src_flow->efdt->locationFor(1) == "video-init.mp4");
    CHECK(v.src_flow->efdt->locationFor(44) == "video-44.mp4v");
    REQUIRE(v.src_flow->media_info);
    CHECK(v.src_flow->media_info->rep_id == "Video1_1");
    CHECK(v.src_flow->media_info->content_type == "video");
    REQUIRE(v.src_flow->payloads.size() == 1);
    CHECK(v.src_flow->payloads[0].codepoint == 8);
    CHECK(v.src_flow->payloads[0].format_id == route::FormatId::File);
    const route::ChannelConfig c = sls::channelConfig(st.rs[0].ls[1]);
    CHECK(c.tsi == 300);
    CHECK(c.real_time);
    CHECK(c.rep_id == "d4_4");
    CHECK(c.media_content_type == "subtitles");
    REQUIRE(c.efdt);
    CHECK(c.efdt->locationFor(9) == "d4_4-9.m4s");
}

TEST_CASE("S-TSID: UniSoft form with addresses, NRT package flow and non-schema 'text' media type", "[sls][stsid]") {
    const std::string xml =
        "<S-TSID xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\" xmlns:fdt=\"urn:ietf:params:xml:ns:fdt\" "
        "xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/\">\n"
        "  <RS dIpAddr=\"239.255.24.3\" dPort=\"8000\" sIpAddr=\"10.5.120.103\">\n"
        "    <LS bw=\"50000\" tsi=\"3\"><SrcFlow rt=\"true\"><EFDT><FDT-Instance afdt:efdtVersion=\"1\" Expires=\"4000000000\" "
        "afdt:fileTemplate=\"test-2-$TOI$.mp4i\" afdt:maxTransportSize=\"83\"/></EFDT><ContentInfo><MediaInfo contentType=\"text\" repId=\"2\"/>"
        "</ContentInfo><Payload codePoint=\"128\" formatId=\"1\" frag=\"0\" order=\"true\"/></SrcFlow></LS>\n"
        "    <LS bw=\"200000\" tsi=\"4\"><SrcFlow><EFDT><FDT-Instance afdt:appContextIdList=\"uscapp\" afdt:efdtVersion=\"1\" Expires=\"3812485780\">"
        "<fdt:File Content-Length=\"1092\" Content-Location=\"uscapp.zip.multipart\" Content-Type=\"multipart/related\" TOI=\"10000\"/>"
        "</FDT-Instance></EFDT><Payload codePoint=\"130\" formatId=\"3\" frag=\"0\" order=\"true\"/></SrcFlow></LS>\n"
        "  </RS>\n</S-TSID>";
    sls::Stsid st;
    REQUIRE(sls::parseStsid(xml, st));
    REQUIRE(st.rs.size() == 1);
    CHECK(st.rs[0].dst_ip == addr(239, 255, 24, 3));
    CHECK(st.rs[0].dst_port == 8000);
    CHECK(st.rs[0].src_ip == addr(10, 5, 120, 103));
    REQUIRE(st.rs[0].ls.size() == 2);
    CHECK(st.rs[0].ls[0].bw == 50000u);
    CHECK(st.rs[0].ls[0].src_flow->media_info->content_type == "text");
    const sls::Ls& nrt = st.rs[0].ls[1];
    CHECK_FALSE(nrt.src_flow->rt);
    CHECK(nrt.src_flow->payloads[0].codepoint == 130);
    CHECK(nrt.src_flow->payloads[0].format_id == route::FormatId::UnsignedPackage);
    REQUIRE(nrt.src_flow->efdt);
    CHECK(nrt.src_flow->efdt->app_context_id_list == std::vector<std::string>{"uscapp"});
    CHECK(nrt.src_flow->efdt->find(10000)->content_type == "multipart/related");
    CHECK_FALSE(sls::parseStsid("<S-TSID/>", st));
}

TEST_CASE("S-TSID and USBD schema examples parse (RepairFlow, 3GPP children, Name/BasePattern)", "[sls][schema]") {
    const std::string sp = schema("sls/route/S-TSID-Example-20220708.xml");
    const std::string up = schema("sls/route/ROUTEUSD-Example-20170920.xml");
    if (!exists(sp) || !exists(up)) SKIP("schemas not available");
    sls::Stsid st;
    std::string err;
    REQUIRE(sls::parseStsid(slurp(sp), st, &err));
    REQUIRE(st.rs.size() == 1);
    CHECK(st.rs[0].src_ip == addr(1, 2, 3, 4));
    CHECK(st.rs[0].dst_ip == addr(4, 3, 2, 1));
    CHECK(st.rs[0].dst_port == 99);
    REQUIRE(st.rs[0].ls.size() == 1);
    const sls::Ls& ls = st.rs[0].ls[0];
    CHECK(ls.tsi == 2);
    CHECK(ls.bw == 20000000u);
    CHECK(ls.start_time_ns);
    REQUIRE(ls.src_flow);
    CHECK(ls.src_flow->min_buff_size == 500u);
    REQUIRE(ls.src_flow->efdt);
    CHECK(ls.src_flow->efdt->file_template == "path/my$$Video$TOI%05d$.m$$ps");
    CHECK(ls.src_flow->efdt->locationFor(3) == "tag:atsc.org,2016:appPackage");
    CHECK(ls.src_flow->efdt->locationFor(4) == "path/my$Video00004.m$ps");
    REQUIRE(ls.src_flow->media_info);
    CHECK(ls.src_flow->media_info->startup);
    CHECK(ls.src_flow->media_info->lang == "en");
    CHECK(ls.src_flow->media_info->content_ratings.size() == 2);
    CHECK(ls.src_flow->payloads[0].fec_params == "f0f1f2f3f4f5f6f7f8f9fafb");
    REQUIRE(ls.repair_flow);
    CHECK(ls.repair_flow->maximum_delay == 100u);
    REQUIRE(ls.repair_flow->protected_objects.size() == 1);
    CHECK(ls.repair_flow->protected_objects[0].tsi == 2);
    CHECK(ls.repair_flow->protected_objects[0].source_toi_x == 1u);

    sls::Usbd u;
    REQUIRE(sls::parseUsbd(slurp(up), u, &err));
    CHECK(u.service_id == 44);
    CHECK(u.service_status == true);
    REQUIRE(u.names.size() == 1);
    CHECK(u.names[0].text == "KUSR");
    CHECK(u.names[0].lang == "en");
    CHECK(u.service_languages == std::vector<std::string>{"en"});
    REQUIRE(u.delivery_methods.size() == 1);
    CHECK(u.delivery_methods[0].broadcast_base_patterns == std::vector<std::string>{"http://kusr.com/KUSR"});
    CHECK(u.delivery_methods[0].unicast_base_patterns == std::vector<std::string>{"http://kusr.com"});
}

TEST_CASE("USBD on air (lang attribute without xml: prefix), envelope and HELD", "[sls][usbd][held]") {
    sls::Usbd u;
    REQUIRE(sls::parseUsbd(
        "<BundleDescriptionROUTE xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ROUTEUSD/1.0/\"><UserServiceDescription serviceId=\"1\">"
        "<Name lang=\"eng\">usc01</Name><DeliveryMethod><BroadcastAppService><BasePattern>test-0-</BasePattern><BasePattern>test-1-</BasePattern>"
        "</BroadcastAppService></DeliveryMethod></UserServiceDescription></BundleDescriptionROUTE>",
        u));
    CHECK(u.service_id == 1);
    CHECK_FALSE(u.service_status);
    CHECK(u.names[0].lang == "eng");
    CHECK(u.names[0].text == "usc01");
    CHECK(u.delivery_methods[0].broadcast_base_patterns.size() == 2);
    REQUIRE(sls::parseUsbd("<BundleDescriptionROUTE><UserServiceDescription serviceId=\"5005\"></UserServiceDescription></BundleDescriptionROUTE>", u));
    CHECK(u.service_id == 5005);
    CHECK_FALSE(sls::parseUsbd("<BundleDescriptionROUTE/>", u));

    sls::Envelope env;
    REQUIRE(sls::parseEnvelope(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><metadataEnvelope xmlns=\"urn:3gpp:metadata:2005:MBMS:envelope\">"
        "<item metadataURI=\"usbd.rusd\" contentType=\"application/route-usd+xml\" version=\"29\"></item>"
        "<item metadataURI=\"mpd.mpd\" contentType=\"application/dash+xml\" version=\"37\" validFrom=\"2023-03-07T22:35:01Z\"></item></metadataEnvelope>",
        env));
    REQUIRE(env.items.size() == 2);
    CHECK(env.items[0].version == 29u);
    CHECK(env.find("mpd.mpd")->content_type == "application/dash+xml");
    CHECK(env.find("mpd.mpd")->valid_from_ns);
    CHECK(env.find("none") == nullptr);

    sls::Held h;
    REQUIRE(sls::parseHeld(
        "<HELD xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/AppSignaling/HELD/1.0/\"><HTMLEntryPackage appContextId=\"uscapp\" "
        "bcastEntryPackageUrl=\"uscapp.zip.multipart\" bcastEntryPageUrl=\"uscapp/home.html\" coupledServices=\"1 2\" lctTSIRef=\"4\"/></HELD>",
        h));
    REQUIRE(h.packages.size() == 1);
    CHECK(h.packages[0].app_context_id == "uscapp");
    CHECK(h.packages[0].bcast_entry_package_url == "uscapp.zip.multipart");
    CHECK(h.packages[0].coupled_services == std::vector<std::uint16_t>{1, 2});
    CHECK(h.packages[0].lct_tsi_ref == std::vector<std::uint32_t>{4});
}
