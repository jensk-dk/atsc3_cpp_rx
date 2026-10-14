// Application Context Cache of selected services (spec 5.18.4): HELD apps and their broadcast packages on the corpus.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "atsc3/service/engine.hpp"
#include "corpus.hpp"
#include "tuner.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
bool waitFinished(const service::Engine& e, int seconds) {
    for (int i = 0; i < seconds * 20; ++i) {
        if (e.status().finished) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
const service::AppFileInfo* file(const service::AppIndex& idx, const std::string& path) {
    for (const auto& f : idx.files)
        if (f.path == path) return &f;
    return nullptr;
}
std::string text(const service::AppFile& f) { return std::string(reinterpret_cast<const char*>(f.data()), f.size); }
}  // namespace

TEST_CASE("Engine: UniSoft usc01 HELD app (uscapp) is cached from its multipart package when selected", "[service][corpus][apps]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 12'000'000'000;
    e.select(24, 1, true);
    std::string err;
    REQUIRE(e.open(pcapTuner(p, cfg), o, &err));
    REQUIRE(waitFinished(e, 180));

    const auto idx = e.apps(24, 1);
    REQUIRE(idx);
    CHECK(idx->selected);
    REQUIRE(idx->apps.size() == 1);
    const service::AppInfo& a = idx->apps[0];
    CHECK(a.app_context_id == "uscapp");
    CHECK(a.bcast_entry_package_url == "uscapp.zip.multipart");
    CHECK(a.bcast_entry_page_url == "uscapp/home.html");
    CHECK_FALSE(a.bband_entry_page_url);
    CHECK_FALSE(a.app_rendering);
    CHECK(a.ready);
    CHECK(a.files == 2);   // envelope.xml + uscapp/home.html
    CHECK(idx->packages >= 1);
    CHECK(idx->rejected == 0);
    CHECK(idx->cached_bytes > 0);
    CHECK(idx->cached_bytes <= idx->cache_limit);
    // the same package repeats on the carousel: unpacked once per arrival, never accumulating
    CHECK(idx->files.size() == 2);
    const service::AppFileInfo* home = file(*idx, "uscapp/home.html");
    REQUIRE(home);
    CHECK(home->content_type == "text/html");   // from the envelope item (the part has no Content-Type header)
    CHECK(home->package == "uscapp.zip.multipart");
    CHECK(home->version == 0u);
    CHECK(home->app_context_ids == std::vector<std::string>{"uscapp"});
    CHECK(home->tsi == 4u);
    const service::AppFileInfo* env = file(*idx, "envelope.xml");
    REQUIRE(env);
    CHECK(env->content_type == "application/mbms-envelope+xml");
    const auto f = e.appFile(24, 1, "uscapp/home.html");
    REQUIRE(f);
    CHECK(f->content_type == "text/html");
    CHECK(f->size == home->bytes);
    CHECK(text(*f).find("<canvas") != std::string::npos);
    CHECK(text(*f).find("ATSC 3.0") != std::string::npos);
    CHECK(e.appFile(24, 1, "./uscapp/home.html"));   // normalized lookup
    CHECK_FALSE(e.appFile(24, 1, "uscapp/../home.html"));
    CHECK_FALSE(e.appFile(24, 1, "nope.html"));
    // the service list mirrors the app with its readiness
    const auto services = e.services();
    const auto usc01 = std::find_if(services.begin(), services.end(), [](const service::ServiceInfo& s) { return s.service_id == 1; });
    REQUIRE(usc01 != services.end());
    REQUIRE(usc01->apps.size() == 1);
    CHECK(usc01->apps[0].ready);
    // usc02 has no HELD
    const auto idx2 = e.apps(24, 2);
    REQUIRE(idx2);
    CHECK(idx2->apps.empty());
    CHECK_FALSE(e.apps(24, 77));
}

TEST_CASE("Engine: BWI HELD apps - WBFF packages over ROUTE, WNUV packages through the MMT USBD ROUTEComponent", "[service][corpus][apps]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 10'000'000'000;
    e.select(1409, 5002, true);
    e.select(1409, 5007, true);
    std::string err;
    REQUIRE(e.open(pcapTuner(p, cfg), o, &err));
    REQUIRE(waitFinished(e, 240));

    // WBFF (ROUTE): HELD in the SLS bundle, five packages declared on five-digit TSIs, the small ones arrive within 10 s
    const auto wbff = e.apps(1409, 5002);
    REQUIRE(wbff);
    REQUIRE(wbff->apps.size() == 1);
    CHECK(wbff->apps[0].app_context_id == "tag:sinclairplatform.com,2020:WBFF:1407");
    CHECK(wbff->apps[0].bcast_entry_package_url == "App.pkg");
    CHECK(wbff->apps[0].bcast_entry_page_url == "index.html");
    CHECK(wbff->apps[0].bband_entry_page_url.value_or("").rfind("https://cdn.sinclairplatform.com/", 0) == 0);
    CHECK(wbff->apps[0].coupled_services == std::vector<std::uint16_t>{5002});
    CHECK_FALSE(wbff->apps[0].app_rendering);
    CHECK(wbff->packages >= 1);
    CHECK(wbff->rejected == 0);
    const service::AppFileInfo* trig = file(*wbff, "dynamic/trigger/trigger.json");
    REQUIRE(trig);
    CHECK(trig->package == "Alert.pkg");
    CHECK(trig->content_type == "application/json");
    CHECK(trig->app_context_ids == std::vector<std::string>{"tag:sinclairplatform.com,2020:WBFF:1407"});
    CHECK(wbff->apps[0].files >= 2);
    CHECK(wbff->apps[0].ready == (file(*wbff, "index.html") != nullptr));   // App.pkg (3.9 MB) needs a longer window
    CHECK(wbff->apps[0].package_tsi == 93412u);
    CHECK(wbff->apps[0].package_bytes == 3880784u);
    if (!wbff->apps[0].ready) {   // the copy under way after 10 s of a ≈127 s carousel cycle
        REQUIRE(wbff->apps[0].package_received);
        CHECK(*wbff->apps[0].package_received > 0);
        CHECK(*wbff->apps[0].package_received < 3880784u);
    }
    for (const auto& f : wbff->files) {   // the envelope says application/js; browsers want text/javascript
        CHECK(f.content_type != "application/js");
        if (f.path.size() > 3 && f.path.compare(f.path.size() - 3, 3, ".js") == 0) CHECK(f.content_type == "text/javascript");
    }
    CHECK(file(*wbff, "envelope.xml"));

    // WNUV (MMTP): HELD via mmt_atsc3_message 0x0003, packages on the ROUTE session named by the USBD ROUTEComponent
    const auto wnuv = e.apps(1409, 5007);
    REQUIRE(wnuv);
    REQUIRE(wnuv->apps.size() == 1);
    CHECK(wnuv->apps[0].app_context_id == "tag:sinclairplatform.com,2020:WNUV:1409");
    CHECK(wnuv->apps[0].bcast_entry_page_url == "index.html");
    CHECK(wnuv->apps[0].coupled_services == std::vector<std::uint16_t>{5007});
    CHECK(wnuv->apps[0].package_tsi == 93782u);
    CHECK(wnuv->apps[0].package_bytes == 3786958u);
    const auto detail = e.service(1409, 5007);
    REQUIRE(detail);
    bool stsid = false, app_channel = false;
    for (const auto& [name, xml] : detail->fragments) stsid |= name == "stsid.sls (ROUTEComponent)" && xml.find("App.pkg") != std::string::npos;
    for (const service::ComponentInfo& c : detail->info.components) app_channel |= c.transport == "ROUTE" && c.kind == "application" && c.name == "App.pkg";
    CHECK(stsid);
    CHECK(app_channel);
    CHECK(wnuv->packages >= 1);
    CHECK(wnuv->apps[0].files >= 2);
    REQUIRE(file(*wnuv, "dynamic/trigger/trigger.json"));
    CHECK(file(*wnuv, "dynamic/trigger/trigger.json")->app_context_ids == std::vector<std::string>{"tag:sinclairplatform.com,2020:WNUV:1409"});
    // WMAR's HELD is broadband-only: listed, never ready
    e.select(1409, 5004, true);
    const auto wmar = e.apps(1409, 5004);
    REQUIRE(wmar);
    REQUIRE(wmar->apps.size() == 1);
    CHECK(wmar->apps[0].app_context_id == "tv:ota-wmar.ewscloud.com");
    CHECK_FALSE(wmar->apps[0].bcast_entry_page_url);
    CHECK(wmar->apps[0].bband_entry_page_url == "https://ota.ewscloud.com/run3tv-common/index.html");
    CHECK_FALSE(wmar->apps[0].ready);
    // WBAL: no HELD
    const auto wbal = e.apps(1409, 5003);
    REQUIRE(wbal);
    CHECK(wbal->apps.empty());
}
