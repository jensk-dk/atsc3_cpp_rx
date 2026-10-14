#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "atsc3/sls/mpd.hpp"
#include "atsc3/tools/routedump.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
const tools::ServiceReport* find(const tools::RouteDumpResult& r, std::uint16_t id) {
    for (const auto& s : r.services)
        if (s.service_id == id) return &s;
    return nullptr;
}
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("routedump: BWI STLTP feed -> five ROUTE services with playable static MPDs, ESG NRT files, MMT skipped",
          "[sls][tools][corpus]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    tools::RouteDumpOptions o;
    o.input = p;
    o.out_dir = tempPath("routedump-bwi");
    o.duration_ns = 8'000'000'000;
    tools::RouteDumpResult r;
    std::string err;
    REQUIRE(tools::routeDump(o, r, &err) == 0);
    CHECK(r.mode == pcap::CaptureFormat::Stltp);
    REQUIRE(r.services.size() == 6);
    for (const std::uint16_t id : std::vector<std::uint16_t>{5002, 5003, 5004, 5006}) {
        const auto* s = find(r, id);
        REQUIRE(s);
        INFO("service " << id);
        CHECK(s->protocol == "ROUTE");
        CHECK(s->bsid == 1409);
        CHECK(s->have_usbd);
        CHECK(s->have_stsid);
        CHECK(s->have_mpd);
        CHECK(s->object_errors == 0);
        REQUIRE(s->representations.size() == 4);
        for (const auto& rep : s->representations) {
            CHECK(rep.init);
            CHECK(rep.segments >= 5);
            CHECK(rep.gaps == 0);
        }
        REQUIRE(s->mpd_path);
        sls::Mpd m;
        REQUIRE(sls::parseMpd(slurp(*s->mpd_path), m));
        CHECK(m.type == "static");
        CHECK(m.media_presentation_duration_s.value_or(0) >= 5.0);
        for (const auto& rep : s->representations) {
            const sls::Representation* mr = m.representation(rep.rep_id);
            REQUIRE(mr);
            CHECK(mr->segment_template.start_number == rep.first_key);
            const std::string init = sls::resolveTemplate(*mr->segment_template.initialization, mr->id, 0, 0);
            CHECK(std::filesystem::exists(std::filesystem::path(s->dir) / init));
            const std::string first = sls::resolveTemplate(*mr->segment_template.media, mr->id, rep.first_key, rep.first_key);
            CHECK(std::filesystem::exists(std::filesystem::path(s->dir) / first));
        }
    }
    const auto* esg = find(r, 5005);
    REQUIRE(esg);
    CHECK(esg->have_stsid);
    CHECK_FALSE(esg->have_mpd);
    CHECK(esg->nrt_objects >= 5);
    CHECK(std::filesystem::exists(std::filesystem::path(esg->dir) / "nrt" / "sgdd_1919"));
    const auto* mmt = find(r, 5007);
    REQUIRE(mmt);
    CHECK(mmt->skipped);
    CHECK(mmt->protocol == "MMTP");
    // WBFF carries the Sinclair app packages (multipart/related) on the SLS session: unpacked
    const auto* wbff = find(r, 5002);
    CHECK(wbff->nrt_objects >= 3);
    CHECK(std::filesystem::is_directory(std::filesystem::path(wbff->dir) / "nrt" / "Alert.pkg.d"));
    CHECK(std::filesystem::exists(std::filesystem::path(wbff->dir) / "sls" / "stsid.xml"));
    CHECK(std::filesystem::exists(std::filesystem::path(wbff->dir) / "sls" / "held.xml"));
}

TEST_CASE("routedump: UniSoft raw-IP capture (sidecar PLPs) -> three A/V services and the ESG service",
          "[sls][tools][corpus]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    tools::RouteDumpOptions o;
    o.input = p;
    o.out_dir = tempPath("routedump-unisoft");
    o.duration_ns = 12'000'000'000;
    tools::RouteDumpResult r;
    std::string err;
    REQUIRE(tools::routeDump(o, r, &err) == 0);
    CHECK(r.mode == pcap::CaptureFormat::IpBypass);
    REQUIRE(r.services.size() == 4);
    const auto* s1 = find(r, 1);
    REQUIRE(s1);
    CHECK(s1->name == "usc01");
    CHECK(s1->have_held);
    REQUIRE(s1->representations.size() == 2);
    CHECK(s1->representations[0].init);
    CHECK(s1->representations[0].segments >= 4);
    REQUIRE(s1->mpd_path);
    CHECK(s1->nrt_objects >= 1);
    CHECK(std::filesystem::exists(std::filesystem::path(s1->dir) / "nrt" / "uscapp.zip.multipart"));
    CHECK(std::filesystem::is_directory(std::filesystem::path(s1->dir) / "nrt" / "uscapp.zip.multipart.d"));
    const auto* s3 = find(r, 3);
    REQUIRE(s3);
    REQUIRE(s3->representations.size() == 3);  // video, audio, text (closed captions)
    REQUIRE(s3->mpd_path);
    sls::Mpd m;
    REQUIRE(sls::parseMpd(slurp(*s3->mpd_path), m));
    CHECK(m.representation("2"));
    const auto* esg = find(r, 99);
    REQUIRE(esg);
    CHECK(esg->nrt_objects >= 2);
    CHECK(std::filesystem::exists(std::filesystem::path(esg->dir) / "nrt" / "atsc3sgdd"));
    // gzip-encoded SGDD was inflated: XML text
    const std::string sgdd = slurp((std::filesystem::path(esg->dir) / "nrt" / "atsc3sgdd").string());
    CHECK(sgdd.find("<") != std::string::npos);
    CHECK(sgdd.find("ServiceGuideDeliveryDescriptor") != std::string::npos);
}

TEST_CASE("routedump: safeRelativePath", "[tools]") {
    CHECK(tools::safeRelativePath("video-1.mp4v") == "video-1.mp4v");
    CHECK(tools::safeRelativePath("path/my$Video.mps") == "path/my$Video.mps");
    CHECK(tools::safeRelativePath("/abs/x.bin") == "abs/x.bin");
    CHECK(tools::safeRelativePath("../../etc/passwd") == "__/__/etc/passwd");
    CHECK(tools::safeRelativePath("http://cdn.example.com/a/b.m4s?x=1") == "a/b.m4s");
    CHECK(tools::safeRelativePath("tag:atsc.org,2016:appPackage") == "tag_atsc.org_2016_appPackage");
    CHECK(tools::safeRelativePath("a\\b:c") == "a/b_c");
}
