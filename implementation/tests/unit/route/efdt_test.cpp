#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "atsc3/route/efdt.hpp"
#include "corpus.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("EFDT: bare FDT-Instance as sent on TSI 0 (Sinclair)", "[route][efdt]") {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<FDT-Instance xmlns=\"urn:ietf:params:xml:ns:fdt\" xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0\" "
        "Expires=\"4294967295\" afdt:efdtVersion=\"218\">\r\n"
        "\t<File TOI=\"458970\" Content-Location=\"SLS\" Content-Length=\"11758\" Transfer-Length=\"11758\" "
        "Content-Type=\"application/mbms-envelope+xml\"/>\r\n</FDT-Instance>";
    route::Efdt e;
    std::string err;
    REQUIRE(route::parseEfdt(xml, e, &err));
    REQUIRE(e.efdt_version);
    CHECK(*e.efdt_version == 218);
    CHECK(e.expires == 4294967295ull);
    REQUIRE(e.files.size() == 1);
    CHECK(e.files[0].toi == 458970);
    CHECK(e.files[0].content_location == "SLS");
    CHECK(e.files[0].transfer_length == 11758);
    CHECK(e.files[0].content_type == "application/mbms-envelope+xml");
    CHECK(e.locationFor(458970) == "SLS");
    CHECK_FALSE(e.locationFor(5));
}

TEST_CASE("EFDT: S-TSID-namespace EFDT wrapper (UniSoft) with gzip files", "[route][efdt]") {
    const std::string xml =
        "<EFDT xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\" xmlns:fdt=\"urn:ietf:params:xml:ns:fdt\" "
        "xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/\">\n"
        "  <FDT-Instance afdt:efdtVersion=\"1\" Expires=\"3812485780\" afdt:maxTransportSize=\"53327\">\n"
        "    <fdt:File Content-Encoding=\"gzip\" Content-Length=\"541338\" Content-Location=\"guideFragments\" "
        "Content-Type=\"application/vnd.oma.bcast.sgdu\" TOI=\"10001\" Transfer-Length=\"53327\"/>\n"
        "    <fdt:File Content-Encoding=\"gzip\" Content-Length=\"1987\" Content-Location=\"serviceFragments\" "
        "Content-Type=\"application/vnd.oma.bcast.sgdu\" TOI=\"10000\" Transfer-Length=\"433\"/>\n"
        "  </FDT-Instance>\n</EFDT>";
    route::Efdt e;
    REQUIRE(route::parseEfdt(xml, e));
    CHECK(e.max_transport_size == 53327u);
    REQUIRE(e.files.size() == 2);
    const route::FdtFile* f = e.find(10000);
    REQUIRE(f);
    CHECK(f->content_encoding == "gzip");
    CHECK(f->content_length == 1987);
    CHECK(f->transfer_length == 433);
}

TEST_CASE("EFDT: fileTemplate expansion ($TOI$, $TOI%05d$, $$)", "[route][efdt]") {
    CHECK(route::expandFileTemplate("video-$TOI$.mp4v", 1676551953) == "video-1676551953.mp4v");
    CHECK(route::expandFileTemplate("myVideo$TOI%05d$.mps", 7) == "myVideo00007.mps");
    CHECK(route::expandFileTemplate("path/my$$Video$TOI%05d$.m$$ps", 42) == "path/my$Video00042.m$ps");
    CHECK(route::expandFileTemplate("test-0-$TOI$.m4pv", 3821) == "test-0-3821.m4pv");
    CHECK_FALSE(route::expandFileTemplate("broken$TOI", 1));
    CHECK_FALSE(route::expandFileTemplate("$Number$", 1));
    route::Efdt e;
    e.file_template = "a0-a02_2-$TOI$.m4s";
    route::FdtFile init;
    init.toi = 1;
    init.content_location = "a0-a02_2-init.mp4";
    e.files.push_back(init);
    CHECK(e.locationFor(1) == "a0-a02_2-init.mp4");
    CHECK(e.locationFor(55) == "a0-a02_2-55.m4s");
}

TEST_CASE("EFDT: schema example with 3GPP extension children", "[route][efdt][schema]") {
    const std::string p = schema("sls/route/EFDT-Example-20211208.xml");
    if (!exists(p)) SKIP("schemas not available");
    route::Efdt e;
    std::string err;
    REQUIRE(route::parseEfdt(slurp(p), e, &err));
    CHECK(e.efdt_version == 0);
    CHECK(e.max_expires_delta == 60u);
    CHECK(e.max_transport_size == 2000000u);
    CHECK(e.file_template == "myVideo$TOI%05d$.mps");
    REQUIRE(e.files.size() == 1);
    CHECK(e.files[0].toi == 3);
    CHECK(e.files[0].content_type == "multipart/related");
    CHECK(e.files[0].content_location == "tag:atsc.org,2016:appPackage");
    CHECK(e.locationFor(12) == "myVideo00012.mps");
}
