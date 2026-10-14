#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "atsc3/ip/demux.hpp"
#include "atsc3/sls/route_receiver.hpp"
#include "lct.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
struct Rx {
    ip::Demux demux;
    std::vector<sls::SlsBundle> bundles;
    std::vector<std::pair<std::shared_ptr<const route::DeliveredObject>, sls::SessionAddress>> objects;
    std::vector<std::string> errors;
    std::unique_ptr<sls::RouteReceiver> rr;
    std::vector<Bytes> keep;
    std::int64_t t = 0;

    Rx() {
        sls::SessionAddress a;
        a.dst_ip = addr(239, 255, 10, 1);
        a.dst_port = 5000;
        sls::RouteReceiverCallbacks cb;
        cb.onSls = [this](const sls::SlsBundle& b) { bundles.push_back(b); };
        cb.onObject = [this](std::shared_ptr<const route::DeliveredObject> o, const sls::SessionAddress& ad) { objects.emplace_back(std::move(o), ad); };
        cb.onError = [this](const std::string& m) { errors.push_back(m); };
        rr = std::make_unique<sls::RouteReceiver>(demux, a, std::move(cb));
    }
    void send(std::uint32_t dst, std::uint16_t port, const Bytes& lct_pkt) {
        keep.push_back(ipv4Udp(addr(10, 0, 0, 1), dst, 40000, port, lct_pkt));
        io::UnitTime tm;
        tm.t_ns = t += 1'000'000;
        demux.onIpv4(0, ByteView(keep.back().data(), keep.back().size()), tm);
    }
    void sendObject(std::uint32_t dst, std::uint16_t port, std::uint32_t tsi, std::uint32_t toi, std::uint8_t cp, const std::string& body) {
        const Bytes b(body.begin(), body.end());
        LctSpec s;
        s.tsi = tsi;
        s.toi = toi;
        s.codepoint = cp;
        s.tol24 = static_cast<std::uint32_t>(b.size());
        std::size_t off = 0;
        while (off < b.size()) {
            const std::size_t n = std::min<std::size_t>(1000, b.size() - off);
            s.start_offset = static_cast<std::uint32_t>(off);
            send(dst, port, lct(s, Bytes(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + n))));
            off += n;
        }
    }
};
std::string bundle(const std::string& stsid_rs) {
    return "Content-Type: multipart/related; boundary=\"bc\"\r\n\r\n"
           "--bc\r\nContent-Type: application/mbms-envelope+xml\r\n\r\n<metadataEnvelope><item metadataURI=\"usbd.xml\" contentType=\"application/route-usd+xml\" version=\"1\"/>"
           "<item metadataURI=\"stsid.xml\" contentType=\"application/route-s-tsid+xml\" version=\"1\"/></metadataEnvelope>\r\n"
           "--bc\r\nContent-Type: application/route-usd+xml\r\nContent-Location: usbd.xml\r\n\r\n"
           "<BundleDescriptionROUTE><UserServiceDescription serviceId=\"7\"><Name lang=\"en\">Seven</Name></UserServiceDescription></BundleDescriptionROUTE>\r\n"
           "--bc\r\nContent-Type: application/route-s-tsid+xml\r\nContent-Location: stsid.xml\r\n\r\n"
           "<S-TSID xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/\" xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/\">" +
           stsid_rs + "</S-TSID>\r\n--bc--\r\n";
}
const char* kRsMedia =
    "<RS dIpAddr=\"239.255.10.2\" dPort=\"5002\"><LS tsi=\"100\"><SrcFlow rt=\"true\"><EFDT><FDT-Instance Expires=\"1\" afdt:fileTemplate=\"video-$TOI$.mp4v\">"
    "<File TOI=\"1\" Content-Location=\"video-init.mp4\"/></FDT-Instance></EFDT><ContentInfo><MediaInfo contentType=\"video\" repId=\"V\"/></ContentInfo>"
    "<Payload codePoint=\"8\" formatId=\"1\"/></SrcFlow></LS></RS>"
    "<RS><LS tsi=\"200\"><SrcFlow><Payload codePoint=\"3\" formatId=\"3\"/></SrcFlow></LS></RS>";
}  // namespace

TEST_CASE("RouteReceiver: SLS bundle opens the described sessions and names media objects", "[sls][receiver]") {
    Rx rx;
    CHECK(rx.rr->sessions().size() == 1);
    // media packets before the S-TSID: undescribed, dropped
    LctSpec early;
    early.tsi = 100;
    early.toi = 5;
    early.codepoint = 8;
    early.tol24 = 3;
    rx.send(addr(239, 255, 10, 2), 5002, lct(early, Bytes{1, 2, 3}));
    CHECK(rx.objects.empty());
    CHECK(rx.demux.stats().unrouted == 1);

    rx.sendObject(addr(239, 255, 10, 1), 5000, 0, 0x30001, 3, bundle(kRsMedia));
    REQUIRE(rx.bundles.size() == 1);
    CHECK(rx.bundles[0].usbd->service_id == 7);
    CHECK(rx.rr->sessions().size() == 2);
    CHECK(rx.rr->stats().stsid_applied == 1);
    // repeat of the same bundle: no second callback
    rx.sendObject(addr(239, 255, 10, 1), 5000, 0, 0x30001, 3, bundle(kRsMedia));
    CHECK(rx.bundles.size() == 1);
    CHECK(rx.rr->stats().sls_objects == 2);

    rx.sendObject(addr(239, 255, 10, 2), 5002, 100, 1, 8, "INIT");
    rx.sendObject(addr(239, 255, 10, 2), 5002, 100, 42, 8, "SEGMENT42");
    rx.sendObject(addr(239, 255, 10, 1), 5000, 200, 9, 3, "--p\r\nContent-Location: a.html\r\n\r\n<html/>\r\n--p--");
    REQUIRE(rx.objects.size() == 3);
    CHECK(rx.objects[0].first->content_location == "video-init.mp4");
    CHECK(rx.objects[0].first->rep_id == "V");
    CHECK(rx.objects[0].second.dst_port == 5002);
    CHECK(rx.objects[1].first->content_location == "video-42.mp4v");
    CHECK(rx.objects[1].first->from_template);
    CHECK(rx.objects[2].first->format == route::FormatId::UnsignedPackage);
    CHECK(rx.objects[2].second.dst_port == 5000);
    CHECK_FALSE(rx.objects[2].first->content_location);
    CHECK(rx.errors.empty());

    SECTION("an S-TSID update that drops the media RS unsubscribes its session") {
        rx.sendObject(addr(239, 255, 10, 1), 5000, 0, 0x30002, 3, bundle("<RS><LS tsi=\"200\"><SrcFlow><Payload codePoint=\"3\" formatId=\"3\"/></SrcFlow></LS></RS>"));
        CHECK(rx.bundles.size() == 2);
        CHECK(rx.rr->sessions().size() == 1);
        rx.sendObject(addr(239, 255, 10, 2), 5002, 100, 43, 8, "SEGMENT43");
        CHECK(rx.objects.size() == 3);
    }
    SECTION("unparseable SLS object is reported") {
        rx.sendObject(addr(239, 255, 10, 1), 5000, 0, 0x30003, 3, "garbage");
        REQUIRE(rx.errors.size() == 1);
        CHECK(rx.rr->stats().sls_errors == 1);
        CHECK(rx.bundles.size() == 1);
    }
}

TEST_CASE("RouteReceiver::fromSlt only accepts ROUTE broadcast services", "[sls][receiver]") {
    ip::Demux demux;
    lls::SltService svc;
    CHECK(sls::RouteReceiver::fromSlt(demux, svc, {}) == nullptr);
    svc.bss = lls::BroadcastSvcSignaling{};
    svc.bss->sls_protocol = 2;
    svc.bss->sls_destination_ip = addr(239, 1, 1, 1);
    svc.bss->sls_destination_port = 5000;
    CHECK(sls::RouteReceiver::fromSlt(demux, svc, {}) == nullptr);
    svc.bss->sls_protocol = 1;
    svc.bss->sls_source_ip = addr(10, 0, 0, 1);
    auto rr = sls::RouteReceiver::fromSlt(demux, svc, {});
    REQUIRE(rr);
    CHECK(rr->slsAddress().dst_port == 5000);
    CHECK(rr->slsAddress().src_ip == addr(10, 0, 0, 1));
    CHECK(rr->slsAddress().toString() == "10.0.0.1->239.1.1.1:5000");
}
