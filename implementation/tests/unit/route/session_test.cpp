#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "atsc3/route/session.hpp"
#include "atsc3/util/gzip.hpp"
#include "lct.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
struct Harness {
    std::vector<std::shared_ptr<const route::DeliveredObject>> objects;
    std::vector<route::ObjectError> errors;
    std::vector<std::pair<std::uint32_t, route::Efdt>> efdts;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> fragments;  // toi, offset
    std::unique_ptr<route::Session> session;
    std::vector<Bytes> keep;

    explicit Harness(route::SessionConfig cfg = {}) {
        route::SessionCallbacks cb;
        cb.onObject = [this](std::shared_ptr<const route::DeliveredObject> o) { objects.push_back(std::move(o)); };
        cb.onObjectError = [this](const route::ObjectError& e) { errors.push_back(e); };
        cb.onEfdt = [this](std::uint32_t tsi, const route::Efdt& e) { efdts.emplace_back(tsi, e); };
        cb.onFragment = [this](std::uint32_t, std::uint32_t toi, ByteView, std::uint64_t off) { fragments.emplace_back(toi, off); };
        session = std::make_unique<route::Session>(std::move(cb), cfg);
    }
    void feed(const Bytes& pkt, std::int64_t t_ns = 0) {
        keep.push_back(pkt);
        ip::UdpDatagram d;
        d.time.t_ns = t_ns;
        d.dst_ip = addr(239, 255, 1, 1);
        d.dst_port = 5000;
        d.payload = ByteView(keep.back().data(), keep.back().size());
        session->onDatagram(d);
    }
};
Bytes seq(std::size_t n, std::uint8_t seed = 0) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7);
    return b;
}
Bytes slice(const Bytes& b, std::size_t off, std::size_t n) { return Bytes(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + n)); }
}  // namespace

TEST_CASE("ROUTE session: EXT_TOL object completes once contiguous, out of order, duplicates ignored", "[route][session]") {
    Harness h;
    const Bytes obj = seq(3000);
    LctSpec s;
    s.tsi = 100;
    s.toi = 5;
    s.codepoint = 8;
    s.tol24 = 3000;
    s.start_offset = 1340;
    h.feed(lct(s, slice(obj, 1340, 1340)));
    s.start_offset = 2680;
    h.feed(lct(s, slice(obj, 2680, 320)));
    CHECK(h.objects.empty());
    CHECK(h.session->openObjects() == 1);
    s.start_offset = 1340;
    h.feed(lct(s, slice(obj, 1340, 1340)));  // duplicate
    CHECK(h.session->stats().duplicates == 1);
    s.start_offset = 0;
    h.feed(lct(s, slice(obj, 0, 1340)));
    REQUIRE(h.objects.size() == 1);
    const auto& o = *h.objects[0];
    CHECK(o.tsi == 100);
    CHECK(o.toi == 5);
    CHECK(o.codepoint == 8);
    CHECK(o.format == route::FormatId::File);
    CHECK(o.completion == route::Completion::TransferLength);
    CHECK(o.data == obj);
    CHECK(o.packets == 4);
    CHECK_FALSE(o.content_location);
    CHECK(h.session->openObjects() == 0);
    CHECK(h.session->stats().objects_complete == 1);
}

TEST_CASE("ROUTE session: in-band EFDT (TOI 0) names objects, supplies lengths and gzip decoding", "[route][session]") {
    Harness h;
    const Bytes plain = seq(2500, 3);
    const Bytes gz = util::gzipCompress(ByteView(plain.data(), plain.size()));
    const std::string efdt =
        "<FDT-Instance xmlns=\"urn:ietf:params:xml:ns:fdt\" xmlns:afdt=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/\" "
        "Expires=\"4294967295\" afdt:efdtVersion=\"3\" afdt:fileTemplate=\"seg-$TOI$.m4s\">"
        "<File TOI=\"77\" Content-Location=\"sgdd_77\" Content-Type=\"application/vnd.oma.bcast.sgdd+xml\" Content-Encoding=\"gzip\" "
        "Content-Length=\"2500\" Transfer-Length=\"" + std::to_string(gz.size()) + "\"/></FDT-Instance>";
    const Bytes efdt_bytes(efdt.begin(), efdt.end());
    LctSpec e;
    e.tsi = 50;
    e.toi = 0;
    e.codepoint = 1;
    e.tol24 = static_cast<std::uint32_t>(efdt_bytes.size());
    h.feed(lct(e, efdt_bytes));
    REQUIRE(h.efdts.size() == 1);
    CHECK(h.efdts[0].first == 50);
    CHECK(h.session->efdt(50) != nullptr);
    CHECK(h.objects.empty());
    // same EFDT again: no second callback
    h.feed(lct(e, efdt_bytes));
    CHECK(h.efdts.size() == 1);

    // gzip object, no in-band length: Transfer-Length from the EFDT completes it
    LctSpec s;
    s.tsi = 50;
    s.toi = 77;
    s.codepoint = 1;
    std::size_t off = 0;
    while (off < gz.size()) {
        const std::size_t n = std::min<std::size_t>(1000, gz.size() - off);
        s.start_offset = static_cast<std::uint32_t>(off);
        h.feed(lct(s, slice(gz, off, n)));
        off += n;
    }
    REQUIRE(h.objects.size() == 1);
    CHECK(h.objects[0]->completion == route::Completion::FdtLength);
    CHECK(h.objects[0]->content_location == "sgdd_77");
    CHECK(h.objects[0]->content_type == "application/vnd.oma.bcast.sgdd+xml");
    CHECK(h.objects[0]->content_decoded);
    CHECK(h.objects[0]->data == plain);
    CHECK(h.objects[0]->transfer_length == gz.size());

    // template-named object
    s.toi = 123;
    s.tol24 = 10;
    s.start_offset = 0;
    h.feed(lct(s, seq(10)));
    REQUIRE(h.objects.size() == 2);
    CHECK(h.objects[1]->content_location == "seg-123.m4s");
    CHECK(h.objects[1]->from_template);
}

TEST_CASE("ROUTE session: close-object flag, carousel repetition and timeout complete or fail length-less objects", "[route][session]") {
    SECTION("close object (B) without any length") {
        Harness h;
        LctSpec s;
        s.tsi = 7;
        s.toi = 9;
        s.codepoint = 128;
        h.feed(lct(s, seq(100)));
        s.start_offset = 100;
        s.close_object = true;
        h.feed(lct(s, seq(20)));
        REQUIRE(h.objects.size() == 1);
        CHECK(h.objects[0]->completion == route::Completion::CloseObject);
        CHECK(h.objects[0]->data.size() == 120);
        CHECK(h.objects[0]->close_object_seen);
    }
    SECTION("carousel: the object restarts at offset 0 with identical bytes") {
        Harness h;
        const Bytes obj = seq(300, 9);
        LctSpec s;
        s.tsi = 7;
        s.toi = 9;
        s.codepoint = 1;
        h.feed(lct(s, slice(obj, 0, 200)));
        s.start_offset = 200;
        h.feed(lct(s, slice(obj, 200, 100)));
        CHECK(h.objects.empty());
        s.start_offset = 0;
        h.feed(lct(s, slice(obj, 0, 200)));
        REQUIRE(h.objects.size() == 1);
        CHECK(h.objects[0]->completion == route::Completion::Carousel);
        CHECK(h.objects[0]->data == obj);
        CHECK(h.session->openObjects() == 1);  // the repetition is being collected
    }
    SECTION("timeout reports the partial object") {
        route::SessionConfig cfg;
        cfg.nrt_object_timeout_ns = 1'000'000'000;
        Harness h(cfg);
        LctSpec s;
        s.tsi = 7;
        s.toi = 9;
        s.tol24 = 1000;
        h.feed(lct(s, seq(100)), 0);
        s.tsi = 8;
        h.feed(lct(s, seq(10)), 5'000'000'000);  // unrelated packet 5 s later triggers expiry
        REQUIRE(h.errors.size() == 1);
        CHECK(h.errors[0].tsi == 7);
        CHECK(h.errors[0].toi == 9);
        CHECK(h.errors[0].reason == "timeout");
        CHECK(h.errors[0].received == 100);
        CHECK(h.errors[0].expected == 1000);
        CHECK(h.session->stats().objects_timeout == 1);
    }
}

TEST_CASE("ROUTE session: channel config drives format, rep id, undescribed TSIs and fragments", "[route][session]") {
    route::SessionConfig cfg;
    cfg.deliver_undescribed = false;
    cfg.fragments = true;
    Harness h(cfg);
    route::ChannelConfig ch;
    ch.tsi = 100;
    ch.real_time = true;
    ch.rep_id = "Video1_1";
    ch.media_content_type = "video";
    route::PayloadRule pr;
    pr.codepoint = 130;
    pr.format_id = route::FormatId::UnsignedPackage;
    ch.payloads.push_back(pr);
    h.session->setChannel(ch);
    CHECK(h.session->channel(100));
    CHECK_FALSE(h.session->channel(101));

    LctSpec s;
    s.tsi = 101;  // not described: dropped
    s.toi = 1;
    s.tol24 = 10;
    h.feed(lct(s, seq(10)));
    CHECK(h.objects.empty());

    s.tsi = 100;
    s.codepoint = 130;
    s.tol24 = 30;
    h.feed(lct(s, seq(10)));
    s.start_offset = 10;
    h.feed(lct(s, seq(20)));
    REQUIRE(h.objects.size() == 1);
    CHECK(h.objects[0]->format == route::FormatId::UnsignedPackage);
    CHECK(h.objects[0]->rep_id == "Video1_1");
    CHECK(h.objects[0]->media_content_type == "video");
    REQUIRE(h.fragments.size() == 2);
    CHECK(h.fragments[0].second == 0);
    CHECK(h.fragments[1].second == 10);

    SECTION("entity mode strips HTTP entity headers") {
        const std::string ent = "Content-Type: video/mp4\r\nContent-Location: ent.mp4\r\n\r\nDATA";
        Bytes eb(ent.begin(), ent.end());
        LctSpec e;
        e.tsi = 100;
        e.toi = 2;
        e.codepoint = 2;
        e.tol24 = static_cast<std::uint32_t>(eb.size());
        h.feed(lct(e, eb));
        REQUIRE(h.objects.size() == 2);
        CHECK(h.objects[1]->format == route::FormatId::Entity);
        CHECK(h.objects[1]->content_location == "ent.mp4");
        CHECK(h.objects[1]->content_type == "video/mp4");
        CHECK(std::string(h.objects[1]->data.begin(), h.objects[1]->data.end()) == "DATA");
    }
    SECTION("payload beyond the announced length is rejected") {
        LctSpec e;
        e.tsi = 100;
        e.toi = 3;
        e.codepoint = 8;
        e.tol24 = 5;
        e.start_offset = 3;
        h.feed(lct(e, seq(10)));
        CHECK(h.session->stats().out_of_bounds == 1);
        CHECK(h.objects.size() == 1);
    }
    SECTION("bad LCT is counted, not delivered") {
        h.feed(Bytes{0x02, 0x00, 0x04, 0x01});
        CHECK(h.session->stats().bad_lct == 1);
    }
}
