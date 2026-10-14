#include <catch2/catch_test_macros.hpp>

#include <string>

#include "atsc3/route/multipart.hpp"

using namespace atsc3;

namespace {
ByteView bv(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
std::string str(ByteView v) { return std::string(v.begin(), v.end()); }
}  // namespace

TEST_CASE("multipart: Sinclair style (folded Content-Type, trailing ';', CRLF, type param)", "[route][multipart]") {
    const std::string body =
        "Content-Type: multipart/related;\r\n boundary=\"boundary-content\";\r\n type=application/mbms-envelope+xml\r\n\r\n"
        "--boundary-content\r\nContent-Type: application/mbms-envelope+xml\r\nContent-Location: envelope.xml\r\n\r\n"
        "<metadataEnvelope/>\r\n"
        "--boundary-content\r\nContent-Type: application/route-usd+xml; charset=utf-8\r\nContent-Location: usbd.rusd\r\n\r\n"
        "<BundleDescriptionROUTE/>\r\n"
        "--boundary-content--\r\n";
    route::Multipart mp;
    std::string err;
    REQUIRE(route::parseMultipart(bv(body), mp, &err));
    CHECK(mp.boundary == "boundary-content");
    CHECK(mp.headers.contentType() == "multipart/related");
    CHECK(mp.headers.contentTypeParam("type") == "application/mbms-envelope+xml");
    REQUIRE(mp.parts.size() == 2);
    CHECK(mp.parts[0].headers.contentType() == "application/mbms-envelope+xml");
    CHECK(mp.parts[0].headers.contentLocation() == "envelope.xml");
    CHECK(str(mp.parts[0].body) == "<metadataEnvelope/>");
    CHECK(mp.parts[1].headers.contentType() == "application/route-usd+xml");
    CHECK(mp.parts[1].headers.contentTypeParam("charset") == "utf-8");
    CHECK(str(mp.parts[1].body) == "<BundleDescriptionROUTE/>");
}

TEST_CASE("multipart: UniSoft style (boundary value itself starts with '--', LF line ends)", "[route][multipart]") {
    const std::string body =
        "Content-Type: multipart/related;\n  boundary=\"--boundary_at_1571960980689\"\n\n"
        "----boundary_at_1571960980689\nContent-Type: application/mbms-envelope+xml\nContent-Location: envelope.xml\n\n"
        "<a/>\n"
        "----boundary_at_1571960980689\nContent-Type: application/route-s-tsid+xml\nContent-Location: stsid.xml\n\n"
        "<S-TSID/>\n"
        "----boundary_at_1571960980689--\n";
    route::Multipart mp;
    REQUIRE(route::parseMultipart(bv(body), mp));
    CHECK(mp.boundary == "--boundary_at_1571960980689");
    REQUIRE(mp.parts.size() == 2);
    CHECK(str(mp.parts[0].body) == "<a/>");
    CHECK(mp.parts[1].headers.contentLocation() == "stsid.xml");
    CHECK(str(mp.parts[1].body) == "<S-TSID/>");
}

TEST_CASE("multipart: body without top-level headers takes the boundary from the first delimiter line", "[route][multipart]") {
    const std::string body = "--b1\r\nContent-Location: x.bin\r\nContent-Transfer-Encoding: binary\r\n\r\n\x01\x02\r\n--b1\r\n\r\nraw\r\n--b1--";
    route::Multipart mp;
    REQUIRE(route::parseMultipart(bv(body), mp));
    CHECK(mp.boundary == "b1");
    REQUIRE(mp.parts.size() == 2);
    CHECK(mp.parts[0].headers.get("content-transfer-encoding") == "binary");
    CHECK(mp.parts[0].body.size() == 2);
    CHECK(str(mp.parts[1].body) == "raw");
    CHECK(mp.parts[1].headers.fields.empty());
}

TEST_CASE("multipart: signed container is flagged; entity headers parse standalone", "[route][multipart]") {
    const std::string body =
        "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=sig\r\n\r\n"
        "--sig\r\nContent-Type: multipart/related; boundary=inner\r\n\r\n--inner\r\n\r\nx\r\n--inner--\r\n"
        "--sig\r\nContent-Type: application/pkcs7-signature\r\n\r\nSIG\r\n--sig--\r\n";
    route::Multipart mp;
    REQUIRE(route::parseMultipart(bv(body), mp));
    CHECK(mp.is_signed);
    REQUIRE(mp.parts.size() == 2);
    CHECK(mp.parts[1].headers.contentType() == "application/pkcs7-signature");
    route::Multipart inner;
    REQUIRE(route::parseMultipart(mp.parts[0].body, inner));
    REQUIRE(inner.parts.size() == 1);
    CHECK(str(inner.parts[0].body) == "x");

    route::EntityHeaders h;
    const std::string ent = "Content-Location: a/b.mp4\r\nContent-Encoding: gzip\r\nX-Multi: one\r\n two\r\n\r\nBODY";
    const std::size_t n = route::parseEntityHeaders(bv(ent), h);
    CHECK(n == ent.size() - 4);
    CHECK(h.contentLocation() == "a/b.mp4");
    CHECK(h.contentEncoding() == "gzip");
    CHECK(h.get("x-multi") == "one two");
    CHECK(route::parseEntityHeaders(bv("<xml/>"), h) == 0);
    const std::string ftyp{'\0', '\0', '\0', '\x18', 'f', 't', 'y', 'p'};
    CHECK(route::parseEntityHeaders(bv(ftyp), h) == 0);
    CHECK_FALSE(route::parseMultipart(bv("not multipart"), mp));
}
