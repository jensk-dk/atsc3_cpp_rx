// STB UI model (spec 5.20.2): the channel list of the mux, remote keys with the list hidden and shown, the banner
// timer and present/following from the guide. Compiled only with ATSC3_STB (the model library lives in the integration).
#ifdef ATSC3_HAVE_STB_MODEL
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "atsc3/esg/esg.hpp"
#include "model.hpp"

using namespace atsc3;
using namespace atsc3::stb;

namespace {

service::ServiceInfo svc(std::uint16_t bsid, std::uint16_t id, std::optional<std::uint16_t> major, std::optional<std::uint16_t> minor, const char* name,
                         const char* protocol, std::uint8_t category, bool hidden = false, const char* component_kind = nullptr) {
    service::ServiceInfo s;
    s.bsid = bsid;
    s.service_id = id;
    s.major = major;
    s.minor = minor;
    s.short_name = name;
    s.protocol = protocol;
    s.category = category;
    s.hidden = hidden;
    s.global_service_id = "tag:example," + std::string(name);
    if (component_kind) {
        service::ComponentInfo c;
        c.kind = component_kind;
        s.components.push_back(c);
    }
    return s;
}

Channel ch(std::uint16_t id, std::uint16_t major, std::uint16_t minor, const char* name) {
    Channel c;
    c.bsid = 1;
    c.service_id = id;
    c.major = major;
    c.minor = minor;
    c.name = name;
    c.protocol = "ROUTE";
    c.global_service_id = "tag:example," + std::string(name);
    return c;
}

constexpr std::int64_t kNtp = 2208988800LL;   // NTP seconds -> Unix seconds
const char* kNs = "xmlns=\"urn:oma:xml:bcast:sg:fragments:1.1\" xmlns:sa=\"tag:atsc.org,2016:XMLSchemas/ATSC3/SA/1.0/\"";

service::EsgFragment frag(service::EsgFragmentType type, const std::string& xml) {
    service::EsgFragment f;
    f.type = type;
    f.xml = xml;
    service::parseEsgFragmentHeader(xml, f);
    return f;
}

esg::Guide guide() {
    std::vector<service::EsgFragment> frags;
    frags.push_back(frag(service::EsgFragmentType::Service,
                         std::string("<Service ") + kNs + " id=\"urn:svc:1\" globalServiceID=\"tag:example,WBFF\" version=\"1\"><ServiceType>228</ServiceType><Name xml:lang=\"en\" text=\"WBFF\"/>"
                                                    "<PrivateExt><sa:ATSC3ServiceExtension><sa:MajorChannelNum>45</sa:MajorChannelNum><sa:MinorChannelNum>1</sa:MinorChannelNum>"
                                                    "</sa:ATSC3ServiceExtension></PrivateExt></Service>"));
    const auto window = [](const char* content, long long start, long long end) {
        return std::string("<ContentReference idRef=\"") + content + "\"><PresentationWindow startTime=\"" + std::to_string(start) + "\" endTime=\"" + std::to_string(end) +
               "\"/></ContentReference>";
    };
    frags.push_back(frag(service::EsgFragmentType::Schedule, std::string("<Schedule ") + kNs + " id=\"urn:sch:1\" version=\"0\"><ServiceReference idRef=\"urn:svc:1\"/>" +
                                                                 window("urn:cnt:A", kNtp + 1000, kNtp + 2000) + window("urn:cnt:B", kNtp + 2000, kNtp + 3000) +
                                                                 window("urn:cnt:C", kNtp + 3500, kNtp + 4000) + "</Schedule>"));
    for (const char* c : {"A", "B", "C"}) {
        frags.push_back(frag(service::EsgFragmentType::Content, std::string("<Content ") + kNs + " id=\"urn:cnt:" + c + "\" version=\"0\"><ServiceReference idRef=\"urn:svc:1\"/><Name text=\"Show " +
                                                                    c + "\" xml:lang=\"en\"/></Content>"));
    }
    return esg::Guide::fromFragments(frags);
}

}  // namespace

TEST_CASE("stb: the channel list is the mux's A/V services in channel order", "[stb]") {
    std::vector<service::ServiceInfo> services;
    services.push_back(svc(1409, 5007, 24, 3, "WNUV", "MMTP", 1));
    services.push_back(svc(1409, 5004, 24, 1, "WMAR", "ROUTE", 1));
    services.push_back(svc(1409, 5008, 24, 9, "ESG", "ROUTE", 4));                       // category 4 without A/V components
    services.push_back(svc(564, 1, 11, 1, "HID", "ROUTE", 1, true));                      // hidden
    services.push_back(svc(564, 2, 11, 2, "BB", "broadband", 1));                         // not on this mux
    services.push_back(svc(564, 7, std::nullopt, std::nullopt, "NUM", "ROUTE", 0, false, "video"));   // no channel number, a video component
    const std::vector<Channel> list = channelsOf(services, false);
    REQUIRE(list.size() == 3);
    CHECK(list[0].label() == "24.1 WMAR");
    CHECK(list[1].label() == "24.3 WNUV");
    CHECK(list[2].label() == "#7 NUM");
    const std::vector<Channel> all = channelsOf(services, true);
    REQUIRE(all.size() == 4);
    CHECK(all[0].label() == "11.1 HID");
    CHECK(all[0].hidden);
    REQUIRE(findChannel(list, "24.3"));
    CHECK(findChannel(list, "24.3")->service_id == 5007);
    REQUIRE(findChannel(list, "5004"));
    CHECK(findChannel(list, "5004")->name == "WMAR");
    CHECK_FALSE(findChannel(list, "9.9"));
    CHECK_FALSE(findChannel(list, "abc"));
    CHECK_FALSE(findChannel(list, ""));
    CHECK(parseChannelNumber("24.3") == std::make_pair(std::uint16_t{24}, std::uint16_t{3}));
    CHECK_FALSE(parseChannelNumber("24."));
    CHECK_FALSE(parseChannelNumber("x.1"));
}

TEST_CASE("stb: with the list hidden Up/Down change channel with wrap-around, Return opens the list, i shows the banner", "[stb]") {
    UiModel m;
    m.setChannels({ch(1, 24, 1, "A"), ch(2, 24, 2, "B"), ch(3, 24, 3, "C")});
    CHECK_FALSE(m.tuned());
    m.setTuned(ch(1, 24, 1, "A"), 0.0);
    CHECK(m.tunedIndex() == 0);
    CHECK(m.bannerVisible(1.0));
    CHECK(m.bannerVisible(4.9));
    CHECK_FALSE(m.bannerVisible(5.1));

    Action a = m.onKey(Key::Up, 10.0);
    REQUIRE(a.kind == Action::Kind::Tune);
    CHECK(a.channel->service_id == 2);
    m.setTuned(*a.channel, 10.0);
    a = m.onKey(Key::Up, 11.0);
    CHECK(a.channel->service_id == 3);
    m.setTuned(*a.channel, 11.0);
    a = m.onKey(Key::Up, 12.0);
    CHECK(a.channel->service_id == 1);   // wraps
    m.setTuned(*a.channel, 12.0);
    a = m.onKey(Key::Down, 13.0);
    CHECK(a.channel->service_id == 3);   // wraps the other way
    m.setTuned(*a.channel, 13.0);
    a = m.onKey(Key::PageDown, 14.0);
    CHECK(a.channel->service_id == 2);
    m.setTuned(*a.channel, 14.0);

    m.hideBanner();
    CHECK_FALSE(m.bannerVisible(14.5));
    CHECK(m.onKey(Key::Info, 20.0).kind == Action::Kind::None);
    CHECK(m.bannerVisible(21.0));
    CHECK_FALSE(m.listVisible());
    CHECK(m.onKey(Key::Back, 22.0).kind == Action::Kind::None);
    CHECK_FALSE(m.bannerVisible(22.0));
    CHECK(m.onKey(Key::Select, 30.0).kind == Action::Kind::None);   // Enter opens the list on the tuned channel
    CHECK(m.listVisible());
    CHECK(m.highlight() == m.tunedIndex());
    CHECK(m.onKey(Key::Back, 31.0).kind == Action::Kind::None);
    CHECK_FALSE(m.listVisible());
    CHECK(m.onKey(Key::Quit, 40.0).kind == Action::Kind::Quit);
    CHECK(m.onKey(Key::None, 41.0).kind == Action::Kind::None);
}

TEST_CASE("stb: the channel list highlights, clamps and tunes the highlighted channel", "[stb]") {
    UiModel m;
    m.page_rows = 2;
    m.setChannels({ch(1, 24, 1, "A"), ch(2, 24, 2, "B"), ch(3, 24, 3, "C"), ch(4, 24, 4, "D")});
    m.setTuned(ch(2, 24, 2, "B"), 0.0);
    CHECK_FALSE(m.listVisible());
    CHECK(m.onKey(Key::List, 1.0).kind == Action::Kind::None);
    CHECK(m.listVisible());
    CHECK(m.highlight() == 1);                                 // opens on the tuned channel
    m.onKey(Key::Down, 2.0);
    CHECK(m.highlight() == 2);
    m.onKey(Key::PageDown, 2.0);
    CHECK(m.highlight() == 3);                                 // clamped at the end
    m.onKey(Key::Down, 2.0);
    CHECK(m.highlight() == 3);
    m.onKey(Key::PageUp, 2.0);
    CHECK(m.highlight() == 1);
    m.onKey(Key::Up, 2.0);
    m.onKey(Key::Up, 2.0);
    m.onKey(Key::Up, 2.0);
    CHECK(m.highlight() == 0);                                 // clamped at the top
    CHECK(m.tunedIndex() == 1);                                // moving the highlight does not tune
    CHECK(m.onKey(Key::Back, 3.0).kind == Action::Kind::None);
    CHECK_FALSE(m.listVisible());
    CHECK(m.highlight() == 0);

    m.showList();
    CHECK(m.highlight() == 1);
    m.onKey(Key::Down, 4.0);
    const Action a = m.onKey(Key::Select, 5.0);
    REQUIRE(a.kind == Action::Kind::Tune);
    CHECK(a.channel->service_id == 3);
    CHECK_FALSE(m.listVisible());
    m.setTuned(*a.channel, 5.0);
    CHECK(m.tunedIndex() == 2);

    m.hideBanner();
    m.showList();
    CHECK(m.onKey(Key::Select, 20.0).kind == Action::Kind::None);   // selecting the tuned channel only shows the banner
    CHECK_FALSE(m.listVisible());
    CHECK(m.bannerVisible(21.0));

    // a new service table keeps the tuned channel by identity and drops a vanished highlight
    m.showList();
    m.onKey(Key::Up, 30.0);
    CHECK(m.highlight() == 1);
    m.setChannels({ch(4, 24, 4, "D"), ch(3, 24, 3, "C"), ch(1, 24, 1, "A")});
    CHECK(m.tunedIndex() == 1);
    CHECK(m.highlight() == 1);                                 // B is gone: falls back to the tuned row
    m.setChannels({ch(4, 24, 4, "D")});
    CHECK(m.tunedIndex() == -1);
    CHECK_FALSE(m.tuned());
    CHECK(m.highlight() == 0);
    m.setChannels({});
    CHECK(m.onKey(Key::Up, 40.0).kind == Action::Kind::None);
}

TEST_CASE("stb: present and following from the guide, matched by globalServiceID then channel number", "[stb]") {
    const esg::Guide g = guide();
    REQUIRE(g.programmes().size() == 3);
    Channel c = ch(1, 45, 1, "WBFF");
    NowNext nn = nowNext(g, c, 1500);
    REQUIRE(nn.now);
    CHECK(nn.now->title == "Show A");
    CHECK(nn.now->start_unix == 1000);
    CHECK(nn.now->end_unix == 2000);
    REQUIRE(nn.next);
    CHECK(nn.next->title == "Show B");
    nn = nowNext(g, c, 2500);
    CHECK(nn.now->title == "Show B");
    CHECK(nn.next->title == "Show C");
    nn = nowNext(g, c, 3200);   // the gap between B and C
    CHECK_FALSE(nn.now);
    REQUIRE(nn.next);
    CHECK(nn.next->title == "Show C");
    CHECK(nn.next->start_unix == 3500);
    nn = nowNext(g, c, 5000);
    CHECK_FALSE(nn.now);
    CHECK_FALSE(nn.next);
    // no globalServiceID match: the channel number finds the ESG service
    c.global_service_id = "tag:example,OTHER";
    nn = nowNext(g, c, 1500);
    REQUIRE(nn.now);
    CHECK(nn.now->title == "Show A");
    c.major = 99;
    CHECK_FALSE(nowNext(g, c, 1500).now);
    CHECK_FALSE(nowNext(g, c, 1500).next);
}

TEST_CASE("stb: GstNavigation key names map to remote keys", "[stb]") {
    CHECK(keyOf("Up") == Key::Up);
    CHECK(keyOf("KP_Down") == Key::Down);
    CHECK(keyOf("Page_Up") == Key::PageUp);
    CHECK(keyOf("minus") == Key::PageDown);
    CHECK(keyOf("Return") == Key::Select);
    CHECK(keyOf("KP_Enter") == Key::Select);
    CHECK(keyOf("Escape") == Key::Back);
    CHECK(keyOf("l") == Key::List);
    CHECK(keyOf("i") == Key::Info);
    CHECK(keyOf("q") == Key::Quit);
    CHECK(keyOf("F1") == Key::None);
    CHECK(clockText(0).size() == 5);
}
#endif
