#include <catch2/catch_test_macros.hpp>

#include <string>

#include "atsc3/sls/bundle.hpp"
#include "atsc3/util/gzip.hpp"

using namespace atsc3;

namespace {
ByteView bv(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
std::string bundleText() {
    return "Content-Type: multipart/related;\r\n boundary=\"boundary-content\";\r\n type=application/mbms-envelope+xml\r\n\r\n"
           "--boundary-content\r\nContent-Type: application/mbms-envelope+xml\r\n\r\n"
           "<?xml version=\"1.0\" encoding=\"utf-8\"?><metadataEnvelope xmlns=\"urn:3gpp:metadata:2005:MBMS:envelope\">"
           "<item metadataURI=\"usbd.rusd\" contentType=\"application/route-usd+xml\" version=\"29\"></item>"
           "<item metadataURI=\"stsid.sls\" contentType=\"application/route-s-tsid+xml\" version=\"221\"></item>"
           "<item metadataURI=\"mpd.mpd\" contentType=\"application/dash+xml\" version=\"37\"></item>"
           "<item metadataURI=\"held.held\" contentType=\"application/atsc-held+xml\" version=\"1\"></item></metadataEnvelope>\r\n"
           "--boundary-content\r\nContent-Type: application/route-usd+xml; charset=utf-8\r\nContent-Location: usbd.rusd\r\n\r\n"
           "<BundleDescriptionROUTE xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ROUTEUSD/1.0/\"><UserServiceDescription serviceId=\"5003\">"
           "<DeliveryMethod><BroadcastAppService><BasePattern>video-</BasePattern></BroadcastAppService></DeliveryMethod></UserServiceDescription>"
           "</BundleDescriptionROUTE>\r\n"
           "--boundary-content\r\nContent-Type: application/route-s-tsid+xml; charset=utf-8\r\nContent-Location: stsid.sls\r\n\r\n"
           "<S-TSID xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\"><RS><LS tsi=\"100\"><SrcFlow rt=\"true\">"
           "<ContentInfo><MediaInfo contentType=\"video\" repId=\"Video1_1\"/></ContentInfo><Payload codePoint=\"8\" formatId=\"1\"/></SrcFlow></LS></RS></S-TSID>\r\n"
           "--boundary-content\r\nContent-Type: text/xml\r\nContent-Location: mpd.mpd\r\n\r\n"
           "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" profiles=\"urn:mpeg:dash:profile:isoff-broadcast:2015\"><Period id=\"P0\">"
           "<AdaptationSet contentType=\"video\"><Representation id=\"Video1_1\" bandwidth=\"1\"><SegmentTemplate media=\"video-$Number$.mp4v\" "
           "initialization=\"video-init.mp4\" duration=\"60060\" timescale=\"60000\" startNumber=\"1\"/></Representation></AdaptationSet></Period></MPD>\r\n"
           "--boundary-content--\r\n";
}
}  // namespace

TEST_CASE("SLS bundle: Annex C TOI bits", "[sls][bundle]") {
    const sls::SlsToi a = sls::decodeSlsToi(0x470001);
    CHECK_FALSE(a.gzip);
    CHECK(a.held);
    CHECK(a.mpd);
    CHECK(a.stsid);
    CHECK(a.usbd);
    CHECK_FALSE(a.dwd);
    CHECK_FALSE(a.apd);
    CHECK(a.version == 1);
    const sls::SlsToi b = sls::decodeSlsToi(0x300DD);
    CHECK(b.stsid);
    CHECK(b.usbd);
    CHECK_FALSE(b.mpd);
    CHECK(b.version == 221);
    const sls::SlsToi c = sls::decodeSlsToi(0x800700DAu);
    CHECK(c.gzip);
    CHECK(c.mpd);
    CHECK(c.version == 0xDA);
}

TEST_CASE("SLS bundle: Sinclair-style multipart with generic text/xml MPD part identified by root element", "[sls][bundle]") {
    const std::string text = bundleText();
    sls::SlsBundle b;
    std::string err;
    REQUIRE(sls::parseSlsBundle(bv(text), 0x470001, b, &err));
    CHECK(b.toi_bits.held);
    CHECK_FALSE(b.is_signed);
    REQUIRE(b.fragments.size() == 4);
    CHECK(b.fragments[0].kind == sls::FragmentKind::Envelope);
    CHECK(b.fragments[1].kind == sls::FragmentKind::Usbd);
    CHECK(b.fragments[1].version == 29u);
    CHECK(b.fragments[2].kind == sls::FragmentKind::Stsid);
    CHECK(b.fragments[2].version == 221u);
    CHECK(b.fragments[3].kind == sls::FragmentKind::Mpd);
    CHECK(b.fragments[3].version == 37u);
    REQUIRE(b.envelope);
    REQUIRE(b.usbd);
    CHECK(b.usbd->service_id == 5003);
    REQUIRE(b.stsid);
    CHECK(b.stsid->rs[0].ls[0].tsi == 100);
    REQUIRE(b.mpd);
    CHECK(b.mpd->representation("Video1_1"));
    CHECK_FALSE(b.held);
    REQUIRE(b.warnings.size() == 1);  // held.held announced but not present
    CHECK(b.warnings[0].find("held.held") != std::string::npos);
    CHECK(b.fragment(sls::FragmentKind::Stsid)->location == "stsid.sls");
}

TEST_CASE("SLS bundle: gzip-compressed and multipart/signed containers", "[sls][bundle]") {
    const std::string text = bundleText();
    const auto gz = util::gzipCompress(bv(text));
    sls::SlsBundle b;
    REQUIRE(sls::parseSlsBundle(ByteView(gz.data(), gz.size()), 0x80030001u, b));
    CHECK(b.toi_bits.gzip);
    CHECK(b.stsid);

    const std::string signed_text =
        "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"sig\"\r\n\r\n--sig\r\n" + text +
        "--sig\r\nContent-Type: application/pkcs7-signature\r\n\r\nSIGNATUREBYTES\r\n--sig--\r\n";
    sls::SlsBundle s;
    std::string err;
    REQUIRE(sls::parseSlsBundle(bv(signed_text), 0x30001, s, &err));
    CHECK(s.is_signed);
    CHECK(std::string(s.signature.begin(), s.signature.end()) == "SIGNATUREBYTES");
    CHECK(s.usbd);
    CHECK(s.stsid);
    CHECK_FALSE(sls::parseSlsBundle(bv("--x\r\n\r\nnothing\r\n--x--"), 1, s, &err));
}
