#include <catch2/catch_test_macros.hpp>

#include "xml/xml.hpp"

using namespace atsc3;

namespace {
ByteView bv(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
}  // namespace

TEST_CASE("namespace-tolerant lookups with prefixes, BOM and CRLF", "[xml]") {
    const std::string doc = "\xEF\xBB\xBF\r\n<?xml version=\"1.0\"?>\r\n<slt:SLT xmlns:slt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SLT/1.0/\" bsid=\"1 2\">"
                            "<slt:Service serviceId=\"5\" xml:lang=\"en\"><slt:Name lang=\"eng\">x</slt:Name></slt:Service><Other/></slt:SLT>";
    pugi::xml_document d;
    std::string err;
    REQUIRE(xml::load(d, bv(doc), &err));
    const pugi::xml_node r = xml::root(d, "SLT");
    REQUIRE(r);
    CHECK(xml::namespaceOf(r) == "tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SLT/1.0/");
    CHECK(xml::splitUIntList(xml::attr(r, "bsid").value()) == std::vector<std::uint32_t>{1, 2});
    const pugi::xml_node s = xml::child(r, "Service");
    REQUIRE(s);
    CHECK(xml::attrUInt(s, "serviceId") == std::optional<std::uint32_t>(5));
    CHECK(xml::lang(s) == std::optional<std::string>("en"));
    CHECK(xml::lang(xml::child(s, "Name")) == std::optional<std::string>("eng"));
    CHECK(xml::children(r, "Service").size() == 1);
    CHECK_FALSE(xml::root(d, "MPD"));
    CHECK(xml::serialize(xml::child(r, "Other")) == "<Other/>");
    CHECK_FALSE(xml::load(d, bv("<a><b></a>"), &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("xs:dateTime and xs:duration parsing", "[xml]") {
    CHECK(xml::parseDateTimeNs("1970-01-01T00:00:00Z") == std::optional<std::int64_t>(0));
    CHECK(xml::parseDateTimeNs("2016-09-11T13:00:00.000-07:00") == std::optional<std::int64_t>(1473624000LL * 1'000'000'000LL));
    CHECK(xml::parseDateTimeNs("2022-09-14T08:59:11+00:00") == std::optional<std::int64_t>(1663145951LL * 1'000'000'000LL));
    CHECK(xml::parseDateTimeNs("2023-03-07T22:35:01.5Z") == std::optional<std::int64_t>(1678228501LL * 1'000'000'000LL + 500'000'000LL));
    CHECK_FALSE(xml::parseDateTimeNs("2023-03-07"));
    CHECK_FALSE(xml::parseDateTimeNs("2023-03-07T22:35:01+0100"));
    CHECK(xml::parseDurationSeconds("-PT4H") == std::optional<double>(-14400.0));
    CHECK(xml::parseDurationSeconds("PT1M") == std::optional<double>(60.0));
    CHECK(xml::parseDurationSeconds("PT168H") == std::optional<double>(604800.0));
    CHECK(xml::parseDurationSeconds("P1DT2H3M4.5S") == std::optional<double>(86400.0 + 7200.0 + 180.0 + 4.5));
    CHECK_FALSE(xml::parseDurationSeconds("P1M"));
    CHECK_FALSE(xml::parseDurationSeconds("4H"));
}
