// REST contract tests against web::Server over a service::Engine, without a browser (spec 9, item 10).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include <httplib.h>

#include "atsc3/service/engine.hpp"
#include "atsc3/web/json.hpp"
#include "atsc3/web/server.hpp"
#include "corpus.hpp"
#include "tuner.hpp"

using namespace atsc3;
using namespace atsc3::test;

TEST_CASE("json writer escapes and nests", "[http][json]") {
    web::Json j;
    j.beginObject().kv("a", "x\"y\\z\n").kv("b", true).kv("c", std::int64_t{-5}).key("d").beginArray().value(1.5).null().value("é").endArray();
    j.kv("bad", std::string("a\xff" "b")).endObject();
    CHECK(j.str() == "{\"a\":\"x\\\"y\\\\z\\n\",\"b\":true,\"c\":-5,\"d\":[1.5,null,\"é\"],\"bad\":\"a\xEF\xBF\xBD" "b\"}");
}

TEST_CASE("REST API: status, captures, open, services, select, detail, auth", "[http][corpus]") {
    const std::string sample = corpus("lls-tables-alp.pcap");
    if (!exists(sample)) SKIP("corpus not available");
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    cfg.captureDirs = {corpus("")};
    web::Server server(engine, cfg);
    std::string err;
    REQUIRE(server.start(&err));
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(10);

    auto r = cli.Get("/api/v1/status");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Cache-Control") == "no-store");
    CHECK(r->body.find("\"open\":false") != std::string::npos);

    r = cli.Get("/api/v1/source");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("lls-tables-alp.pcap") != std::string::npos);
    CHECK(r->body.find("\"format\":\"ALP\"") != std::string::npos);

    // outside the capture directories
    r = cli.Post("/api/v1/source", httplib::Params{{"path", "/etc/passwd"}});
    REQUIRE(r);
    CHECK(r->status == 403);

    r = cli.Post("/api/v1/source", httplib::Params{{"path", sample}, {"pacing", "nonrealtime"}});
    REQUIRE(r);
    CHECK(r->status == 200);
    // the tiny capture is consumed in a few ms; wait for the SLTs of both LLS groups to be registered
    for (int i = 0; i < 250 && engine.services().size() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    r = cli.Get("/api/v1/services");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body);
    CHECK(r->body.find("\"shortName\":\"WRBW\"") != std::string::npos);
    CHECK(r->body.find("\"bsid\":668") != std::string::npos);
    CHECK(r->body.find("\"protocol\":\"ROUTE\"") != std::string::npos);

    r = cli.Post("/api/v1/services/668/1/select", httplib::Params{});
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(engine.selection().count({668, 1}) == 1);
    r = cli.Get("/api/v1/services/668/1");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"selected\":true") != std::string::npos);
    CHECK(r->body.find("SLT entry") != std::string::npos);
    r = cli.Get("/api/v1/services/668/1/sls/SLT%20entry");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type").find("xml") != std::string::npos);
    r = cli.Delete("/api/v1/services/668/1/select");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(engine.selection().empty());
    r = cli.Get("/api/v1/services/1/2");
    REQUIRE(r);
    CHECK(r->status == 404);

    r = cli.Get("/api/v1/lls");
    REQUIRE(r);
    CHECK(r->body.find("\"name\":\"SLT\"") != std::string::npos);
    r = cli.Get("/api/v1/link");
    REQUIRE(r);
    CHECK(r->status == 200);
    // ESG / time endpoints answer with an empty guide when the stream has no ESG service
    r = cli.Get("/api/v1/time");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"captureUnixMs\":") != std::string::npos);
    CHECK(r->body.find("\"wallUnixMs\":") != std::string::npos);
    r = cli.Get("/api/v1/esg");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body);
    CHECK(r->body.find("\"services\":[]") != std::string::npos);
    CHECK(r->body.find("\"programmes\":0") != std::string::npos);
    r = cli.Get("/api/v1/esg/schedule?from=0&to=1");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"programmes\":[]") != std::string::npos);
    r = cli.Get("/api/v1/esg/content/urn%3Ax");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/api/v1/esg/icons/none.png");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/api/v1/esg/fragments?type=Service");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"total\":0") != std::string::npos);
    r = cli.Get("/api/v1/status");
    REQUIRE(r);
    CHECK(r->body.find("\"esgVersion\":") != std::string::npos);
    // media origin: known service without an MPD / segments, unknown service
    r = cli.Get("/api/v1/time/xsdate");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.size() == 24);   // 2026-10-09T12:00:00.000Z
    CHECK(r->body[10] == 'T');
    r = cli.Get("/media/668/1/index.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"segments\":[]") != std::string::npos);
    r = cli.Get("/media/668/1/nothing.m4s");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/media/1/2/index.json");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/media/1/2/mpd.mpd");
    REQUIRE(r);
    CHECK(r->status == 404);
    // long poll returns immediately when the version differs
    r = cli.Get("/api/v1/status?since=0&wait=5000");
    REQUIRE(r);
    CHECK(r->status == 200);
    r = cli.Post("/api/v1/source", httplib::Params{{"action", "close"}});
    REQUIRE(r);
    CHECK(r->body.find("\"open\":false") != std::string::npos);
    r = cli.Get("/");
    REQUIRE(r);
    CHECK(r->status == 200);  // API-only landing page
    server.stop();
    CHECK_FALSE(server.running());
}

TEST_CASE("REST API: bearer token", "[http]") {
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    cfg.token = "s3cret";
    web::Server server(engine, cfg);
    REQUIRE(server.start());
    httplib::Client cli("127.0.0.1", server.port());
    auto r = cli.Get("/api/v1/status");
    REQUIRE(r);
    CHECK(r->status == 401);
    httplib::Headers h{{"Authorization", "Bearer s3cret"}};
    r = cli.Get("/api/v1/status", h);
    REQUIRE(r);
    CHECK(r->status == 200);
    r = cli.Get("/api/v1/status?token=s3cret");
    REQUIRE(r);
    CHECK(r->status == 200);
}

TEST_CASE("REST API: EPG from the BWI ESG service", "[http][corpus][esg]") {
    const std::string sample = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(sample)) SKIP("corpus not available");
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    cfg.captureDirs = {corpus("")};
    web::Server server(engine, cfg);
    REQUIRE(server.start());
    pcap::VirtualTunerConfig tcfg;
    service::OpenOptions o;
    tcfg.pacing = pcap::Pacing::NonRealTime;
    tcfg.duration_ns = 30'000'000'000;
    REQUIRE(engine.open(pcapTuner(sample, tcfg), o));
    for (int i = 0; i < 1200 && !engine.status().finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(engine.status().finished);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(10);
    auto r = cli.Get("/api/v1/esg");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body.substr(0, 2000));
    CHECK(r->body.find("\"name\":\"WBFF\"") != std::string::npos);
    CHECK(r->body.find("\"globalServiceId\":\"tag:sinclairplatform.com,2020:WBFF:1407\"") != std::string::npos);
    CHECK(r->body.find("\"serviceId\":5002") != std::string::npos);   // resolved against the SLT
    CHECK(r->body.find("\"location\":\"sgdd_1919\"") != std::string::npos);
    CHECK(r->body.find("\"icon\":\"/api/v1/esg/icons/s28719_ll_h15_ac.png\"") != std::string::npos);
    r = cli.Get("/api/v1/time");
    REQUIRE(r);
    CHECK(r->body.find("\"captureUnixMs\":1678228") != std::string::npos);   // 2023-03-07, the capture's own clock
    // the programme on air at the capture time, on WBFF
    r = cli.Get("/api/v1/esg/schedule?from=1678228500&to=1678228501&service=urn%3Adigicap%3Asvc%3A5002%3A1678228469449");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body);
    CHECK(r->body.find("\"title\":\"FOX 45 News at 5PM\"") != std::string::npos);
    CHECK(r->body.find("\"bsid\":1409") != std::string::npos);
    CHECK(r->body.find("\"sltServiceId\":5002") != std::string::npos);
    r = cli.Get("/api/v1/esg/content/urn%3Adigicap%3Acnt%3A5002%3ASH042254050000%3A1678078800");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"title\":\"Ukraine in Crisis!\"") != std::string::npos);
    CHECK(r->body.find("\"lengthS\":3540") != std::string::npos);
    CHECK(r->body.find("<Content") != std::string::npos);
    r = cli.Get("/api/v1/esg/icons/s28719_ll_h15_ac.png");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "image/png");
    CHECK(r->body.size() == 8612);
    CHECK(r->body.compare(1, 3, "PNG") == 0);
}

TEST_CASE("REST API: local DASH origin for a selected BWI service", "[http][corpus][media]") {
    const std::string sample = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(sample)) SKIP("corpus not available");
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    cfg.captureDirs = {corpus("")};
    web::Server server(engine, cfg);
    REQUIRE(server.start());
    engine.select(1409, 5004, true);
    pcap::VirtualTunerConfig tcfg;
    service::OpenOptions o;
    tcfg.pacing = pcap::Pacing::NonRealTime;
    tcfg.duration_ns = 8'000'000'000;
    REQUIRE(engine.open(pcapTuner(sample, tcfg), o));
    for (int i = 0; i < 1200 && !engine.status().finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(engine.status().finished);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(10);
    auto r = cli.Get("/media/1409/5004/player.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"mpdUrl\":\"/media/1409/5004/mpd.mpd\"") != std::string::npos);
    CHECK(r->body.find("\"liveDelay\":2.502") != std::string::npos);   // 2 x 1.001 s + 0.5 s
    r = cli.Get("/media/1409/5004/mpd.mpd");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "application/dash+xml");
    CHECK(r->body.find("<BaseURL>/media/1409/5004/</BaseURL>") != std::string::npos);
    CHECK(r->body.find("value=\"/media/1409/5004/time.xsdate\"") != std::string::npos);
    r = cli.Get("/api/v1/time/xsdate");
    REQUIRE(r);
    CHECK(r->body.rfind("2023-03-07T", 0) == 0);   // the capture's clock
    r = cli.Get("/media/1409/5004/time.xsdate");   // the service's clock: newest cached segment's availability
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.rfind("2023-03-07T22:3", 0) == 0);
    r = cli.Get("/media/1409/5004/index.json");
    REQUIRE(r);
    CHECK(r->body.find("\"liveEdgeMs\":16782285") != std::string::npos);   // 2023-03-07T22:35 UTC, within the capture
    CHECK(r->body.find("\"addressing\":\"number\"") != std::string::npos);
    CHECK(r->body.find("\"minSegments\":") == std::string::npos);        // that one lives in player.json
    r = cli.Get("/media/1409/5004/player.json");
    REQUIRE(r);
    CHECK(r->body.find("\"minSegments\":4") != std::string::npos);        // ceil(2.502 / 1.001) + 1
    r = cli.Get("/media/1409/5004/index.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body.substr(0, 1500));
    CHECK(r->body.find("\"id\":\"Video1_1\"") != std::string::npos);
    CHECK(r->body.find("\"name\":\"video-init.mp4\"") != std::string::npos);
    r = cli.Get("/media/1409/5004/video-init.mp4");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "video/mp4");
    CHECK(r->body.compare(4, 4, "ftyp") == 0);
    httplib::Headers range{{"Range", "bytes=0-7"}};
    r = cli.Get("/media/1409/5004/video-init.mp4", range);
    REQUIRE(r);
    CHECK(r->status == 206);
    CHECK(r->body.size() == 8);
    r = cli.Get("/media/1409/5004/offline.mpd");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("type=\"static\"") != std::string::npos);
    r = cli.Get("/media/1409/5003/mpd.mpd");   // SLS known, not selected: MPD exists, no segments
    REQUIRE(r);
    CHECK(r->status == 200);
    r = cli.Get("/media/1409/5003/offline.mpd");
    REQUIRE(r);
    CHECK(r->status == 404);
}

TEST_CASE("REST API: MMT push origin for a selected UniSoft MMT service", "[http][corpus][mmt]") {
    const std::string sample = corpus("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap");
    if (!exists(sample)) SKIP("corpus not available");
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    cfg.captureDirs = {corpus("")};
    web::Server server(engine, cfg);
    REQUIRE(server.start());
    engine.select(24, 3, true);
    pcap::VirtualTunerConfig tcfg;
    service::OpenOptions o;
    tcfg.pacing = pcap::Pacing::NonRealTime;
    tcfg.duration_ns = 12'000'000'000;
    REQUIRE(engine.open(pcapTuner(sample, tcfg), o));
    for (int i = 0; i < 1800 && !engine.status().finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(engine.status().finished);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(10);
    auto r = cli.Get("/media/24/3/player.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"mode\":\"push\"") != std::string::npos);
    CHECK(r->body.find("\"indexUrl\":\"/media/24/3/mmt/index.json\"") != std::string::npos);
    r = cli.Get("/media/24/1/player.json");   // ROUTE service: dash mode
    REQUIRE(r);
    CHECK(r->body.find("\"mode\":\"dash\"") != std::string::npos);
    r = cli.Get("/media/24/3/mmt/index.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body.substr(0, 1500));
    CHECK(r->body.find("\"codecs\":\"hev1.2.4.L120.B0\"") != std::string::npos);
    CHECK(r->body.find("\"codecs\":\"mp4a.40.2\"") != std::string::npos);
    CHECK(r->body.find("\"url\":\"/media/24/3/mmt/20/init.mp4\"") != std::string::npos);
    CHECK(r->body.find("\"mode\":\"keep\"") != std::string::npos);
    CHECK(r->body.find(".m4s\"") != std::string::npos);
    // the newest fragment's seq filters everything out
    const auto next = r->body.find("\"nextSeq\":");
    REQUIRE(next != std::string::npos);
    const std::string seq = r->body.substr(next + 10, r->body.find_first_of(",}", next + 10) - next - 10);
    r = cli.Get("/media/24/3/mmt/index.json?since=" + seq);
    REQUIRE(r);
    CHECK(r->body.find("\"fragments\":[]") != std::string::npos);
    r = cli.Get("/media/24/3/mmt/20/init.mp4");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "video/mp4");
    CHECK(r->body.compare(4, 4, "ftyp") == 0);
    CHECK(r->body.find("mmpu") == std::string::npos);
    const auto idx = engine.mmtMedia(24, 3, 0);
    REQUIRE(idx);
    REQUIRE_FALSE(idx->fragments.empty());
    const auto& f = idx->fragments.back();
    r = cli.Get("/media/24/3/mmt/" + std::to_string(f.packet_id) + "/" + std::to_string(f.mpu) + ".m4s");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.size() == f.bytes);
    CHECK(r->body.compare(4, 4, "styp") == 0);
    r = cli.Get("/media/24/3/mmt/20/1.m4s");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/media/24/3/mmt/99/init.mp4");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/media/24/1/mmt/index.json");
    REQUIRE(r);
    CHECK(r->status == 404);
}

TEST_CASE("Application cache origin: /apps index, entry page with the receiver shim, files, misses", "[http][corpus][apps]") {
    const std::string sample = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(sample)) SKIP("corpus not available");
    service::Engine engine;
    web::ServerConfig cfg;
    cfg.port = 0;
    web::Server server(engine, cfg);
    std::string err;
    REQUIRE(server.start(&err));
    pcap::VirtualTunerConfig tcfg;
    service::OpenOptions o;
    tcfg.pacing = pcap::Pacing::NonRealTime;
    tcfg.duration_ns = 12'000'000'000;
    engine.select(24, 1, true);
    REQUIRE(engine.open(pcapTuner(sample, tcfg), o, &err));
    for (int i = 0; i < 3600 && !engine.status().finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(engine.status().finished);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(10);

    auto r = cli.Get("/apps/24/1/index.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    INFO(r->body);
    CHECK(r->body.find("\"appContextId\":\"uscapp\"") != std::string::npos);
    CHECK(r->body.find("\"ready\":true") != std::string::npos);
    CHECK(r->body.find("\"baseUrl\":\"/apps/24/1/0/\"") != std::string::npos);
    CHECK(r->body.find("\"entryUrl\":\"/apps/24/1/0/uscapp/home.html?wsURL=ws://127.0.0.1:") != std::string::npos);
    CHECK(r->body.find("&rev=20240403\"") != std::string::npos);
    CHECK(r->body.find("\"path\":\"uscapp/home.html\",\"contentType\":\"text/html\"") != std::string::npos);
    CHECK(r->body.find("\"package\":\"uscapp.zip.multipart\"") != std::string::npos);
    // the service list carries the apps too
    r = cli.Get("/api/v1/services/24/1");
    REQUIRE(r);
    CHECK(r->body.find("\"apps\":[{\"appContextId\":\"uscapp\"") != std::string::npos);

    // entry page: text/html with the shim injected right after <head>, the rest of the page intact
    r = cli.Get("/apps/24/1/0/uscapp/home.html?wsURL=ws%3A%2F%2F127.0.0.1%3A8080%2Fapi%2Fv1%2Fa344%2F24%2F1&rev=20240403");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "text/html; charset=utf-8");
    CHECK(r->get_header_value("Cache-Control") == "no-cache");
    const auto head = r->body.find("<head>");
    REQUIRE(head != std::string::npos);
    CHECK(r->body.find("<head><script src=\"/a344-shim.js\"></script>") == head);
    CHECK(r->body.find("<canvas") != std::string::npos);
    CHECK(r->body.find("<!DOCTYPE html") == 0);
    // another file: served as cached, no injection
    r = cli.Get("/apps/24/1/0/envelope.xml");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == "application/mbms-envelope+xml");
    CHECK(r->body.find("<script") == std::string::npos);
    CHECK(r->body.find("metadataEnvelope") != std::string::npos);
    // misses
    r = cli.Get("/apps/24/1/0/uscapp/missing.html");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/apps/24/1/1/uscapp/home.html");
    REQUIRE(r);
    CHECK(r->status == 404);
    r = cli.Get("/apps/24/2/index.json");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("\"apps\":[]") != std::string::npos);
    r = cli.Get("/apps/24/77/index.json");
    REQUIRE(r);
    CHECK(r->status == 404);
}
