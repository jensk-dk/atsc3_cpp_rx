// atsc3:// MRL and option parsing of the VLC plugin (spec 5.19.1); VLC-free, built with the vlc preset.
#ifdef ATSC3_HAVE_VLC_MRL
#include <catch2/catch_test_macros.hpp>

#include "mrl.hpp"

using namespace atsc3::vlcplugin;

TEST_CASE("VLC MRL: capture paths, query terms, udp and tuner forms", "[vlc][mrl]") {
    Mrl m = parseMrl("/tmp/cap.pcap?service=5004&pacing=nonrealtime&loop=1&live-delay=2500&bogus=x");
    CHECK(m.kind == Mrl::Kind::Capture);
    CHECK(m.target == "/tmp/cap.pcap");
    CHECK(m.options.at("service") == "5004");
    Options o;
    std::string unknown;
    applyOptions(m, o, &unknown);
    CHECK(o.service == "5004");
    CHECK_FALSE(o.realtime);
    CHECK(o.loop);
    CHECK(o.live_delay_ms == 2500);
    CHECK(unknown == "bogus");
    // percent-decoding and the '+' space
    m = parseMrl("/tmp/my%20cap.pcap?channel=2.1&alerts=osd");
    CHECK(m.target == "/tmp/my cap.pcap");
    CHECK(m.options.at("channel") == "2.1");
    o = Options{};
    applyOptions(m, o);
    CHECK(o.service == "2.1");
    CHECK(o.alerts == "osd");
    CHECK(parseChannel("2.1") == std::make_pair(std::uint16_t{2}, std::uint16_t{1}));
    CHECK_FALSE(parseChannel("5004"));
    CHECK_FALSE(parseChannel("a.b"));
    m = parseMrl("udp@239.255.24.1:8000?iface=eth0");
    CHECK(m.kind == Mrl::Kind::Udp);
    CHECK(m.target == "239.255.24.1:8000");
    m = parseMrl("tuner/sony0?freq=575000000&plp=0");
    CHECK(m.kind == Mrl::Kind::Tuner);
    CHECK(m.target == "sony0");
    m = parseMrl("");
    CHECK(m.kind == Mrl::Kind::Invalid);
    CHECK_FALSE(m.error.empty());
    // a bsid filter and a zero speed are sanitised
    m = parseMrl("x.pcap?bsid=1409&speed=0");
    o = Options{};
    applyOptions(m, o);
    CHECK(o.bsid == 1409);
    CHECK(o.speed == 1.0);
}
#endif
