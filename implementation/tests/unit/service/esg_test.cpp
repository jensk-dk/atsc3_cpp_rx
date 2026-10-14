// SGDU splitter and SGDD parser (spec 5.11 item 4): synthetic units built the way the Digicap encoder lays them
// out, plus the UniSoft SGDD variant (no namespace on the root, Unix-second times).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "atsc3/service/esg.hpp"

using namespace atsc3;
using namespace atsc3::service;

namespace {
void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
struct Slot {
    std::uint32_t transport_id, version;
    std::uint8_t encoding, type;
    std::string xml;
};
/// unit_header: extension_offset(32) reserved(8) n(32) + n × {id, version, offset}; payload slots in the given order.
std::vector<std::uint8_t> buildSgdu(const std::vector<Slot>& slots, std::uint32_t extension_offset = 0, const std::string& extension = "") {
    std::vector<std::uint8_t> v;
    be32(v, extension_offset);
    v.push_back(0);
    be32(v, static_cast<std::uint32_t>(slots.size()));
    std::uint32_t off = 0;
    for (const Slot& s : slots) {
        be32(v, s.transport_id);
        be32(v, s.version);
        be32(v, off);
        off += 1 + (s.encoding == 0 ? 1 : 0) + static_cast<std::uint32_t>(s.xml.size());
    }
    for (const Slot& s : slots) {
        v.push_back(s.encoding);
        if (s.encoding == 0) v.push_back(s.type);
        v.insert(v.end(), s.xml.begin(), s.xml.end());
    }
    v.insert(v.end(), extension.begin(), extension.end());
    return v;
}
ByteView bv(const std::vector<std::uint8_t>& v) { return ByteView(v.data(), v.size()); }

const std::string kService =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Service xmlns=\"urn:oma:xml:bcast:sg:fragments:1.1\" "
    "xmlns:sa=\"tag:atsc.org,2016:XMLSchemas/ATSC3/SA/1.0/\" id=\"urn:digicap:svc:5002:1\" globalServiceID=\"tag:sinclairplatform.com,2020:WBFF:1407\" "
    "version=\"1\" validFrom=\"3887067600\"><ServiceType>228</ServiceType><Name xml:lang=\"en\" text=\"WBFF\"/></Service>\n";
const std::string kSchedule =
    "<Schedule xmlns=\"urn:oma:xml:bcast:sg:fragments:1.1\" version=\"0\" id=\"urn:digicap:schf:045001\"><ServiceReference idRef=\"urn:digicap:svc:5002:1\"/>"
    "<ContentReference idRef=\"urn:digicap:cnt:1\"><PresentationWindow startTime=\"3887067600\" endTime=\"3887071140\" duration=\"3540\"/></ContentReference></Schedule>";
}  // namespace

TEST_CASE("SGDU: header layout, slot bounds, padding, fragment types", "[service][esg]") {
    const auto bytes = buildSgdu({{1, 1, 0, 1, kService}, {2, 0, 0, 3, kSchedule + "\n"}});
    Sgdu u;
    std::string err;
    REQUIRE(parseSgdu(bv(bytes), u, &err));
    CHECK(u.extension_offset == 0);
    CHECK(u.fragment_count == 2);
    REQUIRE(u.fragments.size() == 2);
    CHECK(u.skipped == 0);
    CHECK(u.warnings.empty());
    CHECK(u.fragments[0].transport_id == 1);
    CHECK(u.fragments[0].version == 1);
    CHECK(u.fragments[0].encoding == 0);
    CHECK(u.fragments[0].type == EsgFragmentType::Service);
    CHECK(u.fragments[0].offset == 0);
    CHECK(u.fragments[0].size == 2 + kService.size());
    CHECK(u.fragments[0].xml == kService.substr(0, kService.size() - 1));   // trailing newline trimmed
    CHECK(u.fragments[1].transport_id == 2);
    CHECK(u.fragments[1].type == EsgFragmentType::Schedule);
    CHECK(u.fragments[1].xml == kSchedule);
    CHECK(toString(u.fragments[1].type) == "Schedule");
}

TEST_CASE("SGDU: entries out of order, non-zero extension_offset bounds the last slot", "[service][esg]") {
    // header lists fragment B (offset of the second slot) before A (offset 0)
    std::vector<std::uint8_t> v;
    const std::string a = "<Content id=\"a\" version=\"1\"/>", b = "<Content id=\"b\" version=\"2\"/>";
    const std::uint32_t slot_a = 2 + static_cast<std::uint32_t>(a.size()), slot_b = 2 + static_cast<std::uint32_t>(b.size());
    be32(v, slot_a + slot_b);   // extension_offset: payload bytes before the extension
    v.push_back(0);
    be32(v, 2);
    be32(v, 20); be32(v, 2); be32(v, slot_a);   // B
    be32(v, 10); be32(v, 1); be32(v, 0);        // A
    v.push_back(0); v.push_back(2); v.insert(v.end(), a.begin(), a.end());
    v.push_back(0); v.push_back(2); v.insert(v.end(), b.begin(), b.end());
    const std::string ext = "EXTENSION-BYTES";
    v.insert(v.end(), ext.begin(), ext.end());
    Sgdu u;
    REQUIRE(parseSgdu(bv(v), u));
    REQUIRE(u.fragments.size() == 2);
    CHECK(u.fragments[0].transport_id == 20);
    CHECK(u.fragments[0].xml == b);   // the extension is not part of the last slot
    CHECK(u.fragments[1].transport_id == 10);
    CHECK(u.fragments[1].xml == a);
    REQUIRE(u.warnings.size() == 1);
    CHECK(u.warnings[0].find("extension_offset") != std::string::npos);
}

TEST_CASE("SGDU: non-XML encodings and bad offsets are skipped, truncated headers rejected", "[service][esg]") {
    auto bytes = buildSgdu({{1, 1, 1, 0, "v=0 sdp"}, {2, 1, 0, 2, "<Content id=\"x\" version=\"3\"/>"}});
    Sgdu u;
    REQUIRE(parseSgdu(bv(bytes), u));
    REQUIRE(u.fragments.size() == 2);
    CHECK(u.fragments[0].encoding == 1);
    CHECK(u.fragments[0].xml.empty());
    CHECK(u.fragments[1].xml == "<Content id=\"x\" version=\"3\"/>");
    CHECK(u.warnings.size() == 1);

    // an entry pointing past the payload
    std::vector<std::uint8_t> bad;
    be32(bad, 0); bad.push_back(0); be32(bad, 1);
    be32(bad, 7); be32(bad, 1); be32(bad, 500);
    bad.push_back(0); bad.push_back(1); bad.push_back('<');
    REQUIRE(parseSgdu(bv(bad), u));
    CHECK(u.skipped == 1);
    CHECK(u.fragments[0].xml.empty());

    std::string err;
    std::vector<std::uint8_t> shrt = {0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(parseSgdu(bv(shrt), u, &err));
    CHECK_FALSE(err.empty());
    std::vector<std::uint8_t> claims;   // header claims 1000 fragments in 9 bytes
    be32(claims, 0); claims.push_back(0); be32(claims, 1000);
    CHECK_FALSE(parseSgdu(bv(claims), u, &err));
    CHECK(err.find("1000") != std::string::npos);
}

TEST_CASE("ESG fragment header: type from the root, id, version, validity in NTP or Unix seconds", "[service][esg]") {
    EsgFragment f;
    REQUIRE(parseEsgFragmentHeader(kService, f));
    CHECK(f.type == EsgFragmentType::Service);
    CHECK(f.id == "urn:digicap:svc:5002:1");
    CHECK(f.version == 1);
    REQUIRE(f.valid_from_unix);
    CHECK(*f.valid_from_unix == 3887067600ll - 2208988800ll);
    CHECK_FALSE(f.valid_to_unix);
    EsgFragment g;
    REQUIRE(parseEsgFragmentHeader("<Content id=\"c\" version=\"7\" validTo=\"1554706800\"/>", g));
    CHECK(g.type == EsgFragmentType::Content);
    CHECK(g.version == 7);
    CHECK(*g.valid_to_unix == 1554706800);   // Unix seconds pass through (UniSoft deviation)
    CHECK_FALSE(parseEsgFragmentHeader("not xml", g));
    CHECK(esgFragmentTypeFromRoot("PreviewData") == EsgFragmentType::PreviewData);
    CHECK(esgFragmentTypeFromRoot("Whatever") == EsgFragmentType::Unknown);
    CHECK(esgTimeToUnix(0) == 0);
    CHECK(esgTimeToUnix(kNtpUnixEpoch) == 0);
}

TEST_CASE("SGDD: Digicap descriptor (namespaced, NTP times, two units)", "[service][esg]") {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><ServiceGuideDeliveryDescriptor xmlns=\"urn:oma:xml:bcast:sg:sgdd:1.0\" id=\"urn:digicap:sgdd:1409\" version=\"30\">"
        "<DescriptorEntry><GroupingCriteria><TimeGroupingCriteria startTime=\"3887067600\" endTime=\"3887326800\"/></GroupingCriteria>"
        "<Transport transmissionSessionID=\"60\"/>"
        "<ServiceGuideDeliveryUnit transportObjectID=\"3378\" contentLocation=\"sgdu_content_3378\">"
        "<Fragment transportID=\"1\" version=\"0\" fragmentType=\"2\" fragmentEncoding=\"0\" id=\"urn:digicap:cnt:5002:SH042254050000:1678078800\"/>"
        "<Fragment transportID=\"2\" version=\"0\" fragmentType=\"2\" fragmentEncoding=\"0\" id=\"urn:digicap:cnt:5002:SH000000010000:1678082340\"/>"
        "</ServiceGuideDeliveryUnit>"
        "<ServiceGuideDeliveryUnit transportObjectID=\"4153\" contentLocation=\"sgdu_service_schedule_4153\">"
        "<Fragment transportID=\"1\" version=\"1\" fragmentType=\"1\" fragmentEncoding=\"0\" id=\"urn:digicap:svc:5002:1678228469449\" validFrom=\"3887067600\"/>"
        "<Fragment transportID=\"6\" version=\"0\" fragmentType=\"3\" fragmentEncoding=\"0\" id=\"urn:digicap:schf:045001:20230307000001\"/>"
        "</ServiceGuideDeliveryUnit></DescriptorEntry></ServiceGuideDeliveryDescriptor>";
    Sgdd d;
    std::string err;
    REQUIRE(parseSgdd(xml, d, &err));
    CHECK(d.id == "urn:digicap:sgdd:1409");
    CHECK(d.version == 30u);
    REQUIRE(d.entries.size() == 1);
    const SgddEntry& e = d.entries[0];
    CHECK(e.start_unix == 1678078800);
    CHECK(e.end_unix == 1678338000);
    CHECK(e.tsi == 60u);
    CHECK_FALSE(e.ip);
    REQUIRE(e.units.size() == 2);
    CHECK(e.units[0].toi == 3378u);
    CHECK(e.units[0].content_location == "sgdu_content_3378");
    REQUIRE(e.units[0].fragments.size() == 2);
    CHECK(e.units[0].fragments[0].type == EsgFragmentType::Content);
    CHECK(e.units[0].fragments[1].id == "urn:digicap:cnt:5002:SH000000010000:1678082340");
    REQUIRE(e.units[1].fragments.size() == 2);
    CHECK(e.units[1].fragments[0].type == EsgFragmentType::Service);
    CHECK(e.units[1].fragments[0].version == 1);
    CHECK(e.units[1].fragments[0].valid_from_unix == 1678078800);
    CHECK(e.units[1].fragments[1].type == EsgFragmentType::Schedule);
}

TEST_CASE("SGDD: UniSoft descriptor (root without namespace, Unix-second times, Transport address)", "[service][esg]") {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ServiceGuideDeliveryDescriptor id=\"atsc3sgdd\" version=\"2\" xmlns:ns2=\"urn:oma:xml:bcast:sg:sgdd:1.0\">\n"
        "    <DescriptorEntry>\n        <GroupingCriteria>\n            <TimeGroupingCriteria startTime=\"1554706800\" endTime=\"1555916400\"/>\n        </GroupingCriteria>\n"
        "        <Transport ipAddress=\"239.255.33.99\" port=\"8000\" srcIpAddress=\"10.5.120.1\" transmissionSessionID=\"1000\" hasFDT=\"false\"/>\n"
        "        <ServiceGuideDeliveryUnit transportObjectID=\"0\" contentLocation=\"guideFragments\">\n"
        "            <Fragment transportID=\"1300000000\" id=\"bcast://www.titantv.com/1009420\" version=\"0\" fragmentEncoding=\"0\" fragmentType=\"2\"/>\n"
        "        </ServiceGuideDeliveryUnit>\n    </DescriptorEntry>\n</ServiceGuideDeliveryDescriptor>\n";
    Sgdd d;
    REQUIRE(parseSgdd(xml, d));
    CHECK(d.id == "atsc3sgdd");
    REQUIRE(d.entries.size() == 1);
    CHECK(d.entries[0].start_unix == 1554706800);
    CHECK(d.entries[0].ip == "239.255.33.99");
    CHECK(d.entries[0].port == 8000);
    CHECK(d.entries[0].src_ip == "10.5.120.1");
    CHECK(d.entries[0].tsi == 1000u);
    REQUIRE(d.entries[0].units.size() == 1);
    CHECK(d.entries[0].units[0].toi == 0u);
    CHECK(d.entries[0].units[0].fragments[0].transport_id == 1300000000u);
    std::string err;
    CHECK_FALSE(parseSgdd(std::string("<Other/>"), d, &err));
    CHECK(err.find("ServiceGuideDeliveryDescriptor") != std::string::npos);
}
