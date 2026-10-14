// esg data model and programme index (spec 5.17.5) on fragments shaped like the Digicap/Sinclair ESG.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "atsc3/esg/esg.hpp"

using namespace atsc3;

namespace {
service::EsgFragment frag(service::EsgFragmentType type, const std::string& xml, std::uint32_t version = 0) {
    service::EsgFragment f;
    f.type = type;
    f.xml = xml;
    f.version = version;
    service::parseEsgFragmentHeader(xml, f);
    return f;
}
const char* kNs = "xmlns=\"urn:oma:xml:bcast:sg:fragments:1.1\" xmlns:sa=\"tag:atsc.org,2016:XMLSchemas/ATSC3/SA/1.0/\"";
std::string serviceXml(const char* id, const char* name, unsigned major, unsigned minor, const char* icon, unsigned version = 1) {
    return std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Service ") + kNs + " id=\"" + id + "\" globalServiceID=\"tag:example," + name + "\" version=\"" +
           std::to_string(version) + "\"><ServiceType>228</ServiceType><Name xml:lang=\"en\" text=\"" + name + "\"/><Description xml:lang=\"en\" text=\"" + name +
           " desc\"/><Genre href=\"http://www.atsc.org/XMLSchemas/mh/2009/1.0/genre-cs/:35\"/><PrivateExt><sa:ATSC3ServiceExtension><sa:Icon width=\"360\" height=\"270\">" +
           icon + "</sa:Icon><sa:MajorChannelNum>" + std::to_string(major) + "</sa:MajorChannelNum><sa:MinorChannelNum>" + std::to_string(minor) +
           "</sa:MinorChannelNum></sa:ATSC3ServiceExtension></PrivateExt></Service>";
}
std::string contentXml(const char* id, const char* svc, const char* title, const char* rating = nullptr) {
    std::string s = std::string("<Content ") + kNs + " id=\"" + id + "\" version=\"0\"><ServiceReference idRef=\"" + svc + "\"/><Name text=\"" + title +
                    "\" xml:lang=\"en\"/><Name text=\"" + title + " (es)\" xml:lang=\"es\"/><Description text=\"About " + title +
                    "\" xml:lang=\"en\"/><Length>PT59M</Length><Genre href=\"http://www.atsc.org/XMLSchemas/mh/2009/1.0/genre-cs/:55\"/>";
    if (rating) {
        s += std::string("<sa:ContentAdvisoryRatings><sa:RegionIdentifier>1</sa:RegionIdentifier><sa:RatingDescription xml:lang=\"en\">") + rating +
             "</sa:RatingDescription><sa:RatedDimensions>1</sa:RatedDimensions><sa:RatingDimVal><sa:RatingDimension>0</sa:RatingDimension><sa:RatingValueString>" + rating +
             "</sa:RatingValueString></sa:RatingDimVal></sa:ContentAdvisoryRatings>";
    }
    s += "<PrivateExt><sa:ContentIcon MIMEType=\"image/*\" width=\"240\" height=\"360\">http://img.example/" + std::string(id) +
         ".jpg?w=240&amp;h=360</sa:ContentIcon><sa:Components><sa:VideoComponent language=\"en\">HD</sa:VideoComponent><sa:CCComponent language=\"en\">CC</sa:CCComponent>"
         "</sa:Components></PrivateExt></Content>";
    return s;
}
}  // namespace

TEST_CASE("esg: Service fragment with A/332 extensions", "[esg]") {
    esg::Service s;
    std::string err;
    REQUIRE(esg::parseService(serviceXml("urn:svc:1", "WBFF", 45, 1, "s1.png"), s, &err));
    CHECK(s.id == "urn:svc:1");
    CHECK(s.version == 1);
    CHECK(s.global_service_id == "tag:example,WBFF");
    CHECK(s.service_type == 228u);
    REQUIRE(s.names.size() == 1);
    CHECK(s.names[0].text == "WBFF");
    CHECK(s.names[0].lang == "en");
    CHECK(esg::pickText(s.descriptions) == "WBFF desc");
    CHECK(s.major == 45);
    CHECK(s.minor == 1);
    REQUIRE(s.icons.size() == 1);
    CHECK(s.icons[0].uri == "s1.png");
    CHECK(s.icons[0].width == 360u);
    REQUIRE(s.genres.size() == 1);
    CHECK(s.genres[0].find(":35") != std::string::npos);
    CHECK_FALSE(esg::parseService("<Content id=\"x\" version=\"0\"/>", s, &err));
    // OMA-style Name with element text instead of @text
    REQUIRE(esg::parseService("<Service id=\"a\" version=\"2\"><Name xml:lang=\"fr\">Nom</Name></Service>", s));
    CHECK(esg::pickText(s.names) == "Nom");
    CHECK(esg::pickText(s.names, "fr") == "Nom");
}

TEST_CASE("esg: Content fragment: names per language, length, ratings, icon, components", "[esg]") {
    esg::Content c;
    REQUIRE(esg::parseContent(contentXml("urn:cnt:1", "urn:svc:1", "Family Feud", "TV-PG"), c));
    CHECK(c.id == "urn:cnt:1");
    CHECK(c.service_ids == std::vector<std::string>{"urn:svc:1"});
    CHECK(esg::pickText(c.names) == "Family Feud");
    CHECK(esg::pickText(c.names, "es") == "Family Feud (es)");
    CHECK(esg::pickText(c.names, "de") == "Family Feud");   // falls back to the first entry
    CHECK(c.length_s == 3540.0);
    REQUIRE(c.ratings.size() == 1);
    CHECK(c.ratings[0].description == "TV-PG");
    CHECK(c.ratings[0].region == 1);
    REQUIRE(c.ratings[0].dimensions.size() == 1);
    CHECK(c.ratings[0].dimensions[0].first == 0);
    CHECK(c.ratings[0].dimensions[0].second == "TV-PG");
    REQUIRE(c.icons.size() == 1);
    CHECK(c.icons[0].uri == "http://img.example/urn:cnt:1.jpg?w=240&h=360");
    CHECK(c.icons[0].mime == "image/*");
    REQUIRE(c.components.size() == 2);
    CHECK(c.components[0].kind == "video");
    CHECK(c.components[0].text == "HD");
    CHECK(c.components[1].kind == "cc");
    // OtherRatings
    REQUIRE(esg::parseContent(std::string("<Content ") + kNs + " id=\"o\" version=\"0\"><sa:OtherRatings ratingScheme=\"urn:x:mpaa\"><sa:RatingInformationString>PG-13</sa:RatingInformationString></sa:OtherRatings></Content>", c));
    REQUIRE(c.ratings.size() == 1);
    CHECK(c.ratings[0].scheme == "urn:x:mpaa");
    CHECK(c.ratings[0].description == "PG-13");
}

TEST_CASE("esg: Schedule fragment windows (NTP seconds, duration fallback)", "[esg]") {
    esg::Schedule s;
    REQUIRE(esg::parseSchedule(std::string("<Schedule ") + kNs + " version=\"0\" id=\"urn:sch:1\"><ServiceReference idRef=\"urn:svc:1\"/>"
                               "<ContentReference idRef=\"urn:cnt:1\"><PresentationWindow startTime=\"3887067600\" endTime=\"3887071140\" duration=\"3540\"/></ContentReference>"
                               "<ContentReference idRef=\"urn:cnt:2\"><PresentationWindow startTime=\"3887071140\" duration=\"60\" id=\"w2\"/></ContentReference></Schedule>", s));
    CHECK(s.service_id == "urn:svc:1");
    REQUIRE(s.contents.size() == 2);
    CHECK(s.contents[0].windows[0].start_unix == 1678078800);
    CHECK(s.contents[0].windows[0].end_unix == 1678082340);
    CHECK(s.contents[1].windows[0].end_unix == 1678082340 + 60);   // from duration
    CHECK(s.contents[1].windows[0].id == "w2");
}

TEST_CASE("esg: Guide joins services, schedules and contents; versions replace; carousel repeats collapse", "[esg]") {
    using service::EsgFragmentType;
    std::vector<service::EsgFragment> frags;
    frags.push_back(frag(EsgFragmentType::Service, serviceXml("urn:svc:2", "WMAR", 2, 1, "s2.png")));
    frags.push_back(frag(EsgFragmentType::Service, serviceXml("urn:svc:1", "WBFF", 45, 1, "s1.png")));
    frags.push_back(frag(EsgFragmentType::Content, contentXml("urn:cnt:1", "urn:svc:1", "News at 5", "TV-G")));
    frags.push_back(frag(EsgFragmentType::Content, contentXml("urn:cnt:2", "urn:svc:1", "Family Feud", "TV-PG")));
    frags.push_back(frag(EsgFragmentType::Schedule, std::string("<Schedule ") + kNs + " version=\"0\" id=\"urn:sch:1\"><ServiceReference idRef=\"urn:svc:1\"/>"
                                                        "<ContentReference idRef=\"urn:cnt:1\"><PresentationWindow startTime=\"3887067600\" endTime=\"3887071200\"/></ContentReference>"
                                                        "<ContentReference idRef=\"urn:cnt:2\"><PresentationWindow startTime=\"3887071200\" endTime=\"3887073000\"/></ContentReference>"
                                                        "<ContentReference idRef=\"urn:cnt:missing\"><PresentationWindow startTime=\"3887073000\" endTime=\"3887074800\"/></ContentReference></Schedule>"));
    // a second schedule fragment repeating the first window (carousel / other grouping)
    frags.push_back(frag(EsgFragmentType::Schedule, std::string("<Schedule ") + kNs + " version=\"0\" id=\"urn:sch:1b\"><ServiceReference idRef=\"urn:svc:1\"/>"
                                                        "<ContentReference idRef=\"urn:cnt:1\"><PresentationWindow startTime=\"3887067600\" endTime=\"3887071200\"/></ContentReference></Schedule>"));
    frags.push_back(frag(EsgFragmentType::Access, "<Access id=\"acc\" version=\"0\"/>"));
    esg::Guide g = esg::Guide::fromFragments(frags);
    const esg::GuideStats& st = g.stats();
    CHECK(st.services == 2);
    CHECK(st.schedules == 2);
    CHECK(st.contents == 2);
    CHECK(st.programmes == 3);
    CHECK(st.ignored == 1);
    CHECK(st.failed == 0);
    CHECK(st.unresolved_contents == 1);
    REQUIRE(g.services().size() == 2);
    CHECK(g.services()[0]->major == 2);    // sorted by channel number
    CHECK(g.services()[1]->major == 45);
    REQUIRE(g.content("urn:cnt:2"));
    CHECK(g.content("urn:cnt:nope") == nullptr);
    const auto span = g.span();
    REQUIRE(span);
    CHECK(span->first == 1678078800);
    CHECK(span->second == 1678078800 + 7200);
    const auto all = g.programmes();
    REQUIRE(all.size() == 3);
    CHECK(all[0].content_id == "urn:cnt:1");
    CHECK(all[0].content != nullptr);
    CHECK(esg::pickText(all[0].content->names) == "News at 5");
    CHECK(all[2].content == nullptr);   // unresolved content reference stays as a row
    const std::string svc = "urn:svc:1";
    CHECK(g.programmes(1678078800 + 3600, 1678078800 + 3601, &svc).size() == 1);   // overlap query
    CHECK(g.programmes(1678078800 + 3600, 1678078800 + 3601, &svc)[0].content_id == "urn:cnt:2");
    const std::string other = "urn:svc:2";
    CHECK(g.programmes(0, 1u << 31, &other).empty());

    // newer version of a Service replaces, older is ignored
    esg::Guide g2;
    CHECK(g2.add(frag(EsgFragmentType::Service, serviceXml("urn:svc:1", "WBFF", 45, 1, "s1.png", 3))));
    CHECK(g2.add(frag(EsgFragmentType::Service, serviceXml("urn:svc:1", "OLD", 45, 1, "s1.png", 2))));
    CHECK(g2.add(frag(EsgFragmentType::Service, serviceXml("urn:svc:1", "NEW", 45, 1, "s1.png", 4))));
    g2.build();
    REQUIRE(g2.services().size() == 1);
    CHECK(esg::pickText(g2.services()[0]->names) == "NEW");
    std::string err;
    CHECK_FALSE(g2.add(frag(EsgFragmentType::Content, "broken <"), &err));
    CHECK(g2.stats().failed == 1);
}
