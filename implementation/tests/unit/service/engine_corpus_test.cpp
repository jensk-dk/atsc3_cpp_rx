#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <chrono>
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
const service::ServiceInfo* find(const std::vector<service::ServiceInfo>& v, std::uint16_t id) {
    for (const auto& s : v)
        if (s.service_id == id) return &s;
    return nullptr;
}
}  // namespace

TEST_CASE("Engine: BWI feed -> six services with components; selection opens media sessions", "[service][corpus]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 6'000'000'000;
    e.select(1409, 5004, true);  // selection before the SLT is known
    std::string err;
    REQUIRE(e.open(pcapTuner(p, cfg), o, &err));
    REQUIRE(waitFinished(e, 120));
    const service::Status st = e.status();
    CHECK(st.open);
    CHECK(st.tuner == "pcap");
    CHECK(st.output == service::TunerOutput::Bbp);   // STLTP capture: the virtual tuner delivers BBPs
    CHECK(st.services == 6);
    CHECK(st.selected == 1);
    CHECK(st.lls_groups == std::vector<std::uint8_t>{1});
    CHECK(st.bsids == std::vector<std::uint16_t>{1409});
    const auto services = e.services();
    REQUIRE(services.size() == 6);
    const auto* wmar = find(services, 5004);
    REQUIRE(wmar);
    CHECK(wmar->protocol == "ROUTE");
    CHECK(wmar->short_name == "WMAR");
    CHECK(wmar->major == 2);
    CHECK(wmar->minor == 1);
    CHECK(wmar->selected);
    CHECK(wmar->sls_received);
    REQUIRE(wmar->components.size() == 4);
    CHECK(wmar->components[0].kind == "video");
    CHECK(wmar->components[0].codecs.rfind("hvc1", 0) == 0);
    CHECK(wmar->components[0].tsi == 100u);
    CHECK(wmar->components[0].width == 1280u);
    CHECK(wmar->components[0].objects > 0);
    CHECK(wmar->components[1].kind == "audio");
    CHECK(wmar->components[1].codecs.rfind("ac-4", 0) == 0);
    CHECK(wmar->components[1].lang == "eng");
    CHECK(wmar->components[3].kind == "subtitles");
    CHECK(wmar->objects > 20);
    const auto* wbal = find(services, 5003);
    REQUIRE(wbal);
    CHECK(wbal->protected_flag);
    CHECK_FALSE(wbal->selected);
    CHECK(wbal->objects == 0);  // SLS only
    CHECK(wbal->components.size() == 4);
    CHECK(wbal->components[0].content_protected);
    const auto* wnuv = find(services, 5007);
    REQUIRE(wnuv);
    CHECK(wnuv->protocol == "MMTP");
    CHECK(wnuv->sls_received);
    CHECK(wnuv->mmtp_packets > 100);
    std::size_t video = 0, audio = 0, subs = 0;
    for (const auto& c : wnuv->components) {
        if (c.kind == "video") {
            ++video;
            CHECK(c.codecs == "hev1");
            CHECK(c.packet_id == 100);
            CHECK(c.packets > 0);
        } else if (c.kind == "audio") {
            ++audio;
            CHECK(c.codecs == "ac-4");
        } else if (c.kind == "subtitles") {
            ++subs;
            CHECK(c.codecs == "stpp");
            CHECK(c.packet_id == 300);
        }
    }
    CHECK(video == 1);
    CHECK(audio == 2);
    CHECK(subs == 1);
    const auto detail = e.service(1409, 5007);
    REQUIRE(detail);
    bool have_usbd = false;
    for (const auto& [name, text] : detail->fragments) have_usbd |= name.rfind("USBD", 0) == 0 && text.find("BundleDescriptionMMT") != std::string::npos;
    CHECK(have_usbd);
    const auto esg = find(services, 5005);
    REQUIRE(esg);
    CHECK(esg->category == 4);
    CHECK(esg->components.size() == 3);
    CHECK(esg->components[0].kind == "nrt");
    // link + lls snapshots
    const service::LinkInfo link = e.link();
    CHECK(link.lmts.size() == 3);
    CHECK(link.plp_map.size() > 50);
    CHECK(link.l1);
    CHECK(e.llsTables().size() >= 3);
    // deselect closes media sessions; selection persists across reopen
    e.select(1409, 5004, false);
    CHECK(e.status().selected == 0);
    CHECK(e.selection().empty());
    e.close();
    CHECK_FALSE(e.status().open);
}

TEST_CASE("Engine: UniSoft MMT capture -> usc03m assets from the MP table", "[service][mmtp][corpus]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 6'000'000'000;
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 120));
    CHECK(e.status().output == service::TunerOutput::Ip);
    const auto services = e.services();
    const auto* mmt = find(services, 3);
    REQUIRE(mmt);
    CHECK(mmt->protocol == "MMTP");
    CHECK(mmt->mmt_package_id == "usc03m-1");
    CHECK(mmt->usbd_name == "usc03m");
    std::vector<std::string> codecs;
    for (const auto& c : mmt->components)
        if (c.kind == "video" || c.kind == "audio") codecs.push_back(c.codecs + "@" + std::to_string(c.packet_id.value_or(0)));
    std::sort(codecs.begin(), codecs.end());
    CHECK(codecs == std::vector<std::string>{"ac-4@21", "hev1@20"});
    const auto* r1 = find(services, 1);
    REQUIRE(r1);
    CHECK(r1->protocol == "ROUTE");
    CHECK(r1->components.size() >= 2);
}

TEST_CASE("Engine: ESG service sessions open without selection; SGDD/SGDU become versioned fragments", "[service][corpus][esg]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 30'000'000'000;
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 120));
    const auto services = e.services();
    const auto* esg = find(services, 5005);
    REQUIRE(esg);
    CHECK(esg->category == 4);
    CHECK_FALSE(esg->selected);
    CHECK(esg->objects > 3);   // NRT sessions were opened by policy, not by selection
    const service::EsgSnapshot snap = e.esg();
    CHECK(snap.version > 0);
    CHECK(snap.stats.sgdd >= 1);
    CHECK(snap.stats.sgdu >= 2);
    CHECK(snap.stats.parse_errors == 0);
    CHECK(snap.stats.fragments_new == 468);
    CHECK(snap.fragments.size() == 468);
    CHECK(snap.stats.fragments_duplicate >= 1);   // the carousel repeats, versions do not move
    std::size_t svc = 0, sch = 0, cnt = 0;
    for (const auto& f : snap.fragments) {
        if (f.type == service::EsgFragmentType::Service) ++svc;
        else if (f.type == service::EsgFragmentType::Schedule) ++sch;
        else if (f.type == service::EsgFragmentType::Content) ++cnt;
        CHECK(f.bsid == 1409);
        CHECK(f.service_id == 5005);
        CHECK_FALSE(f.id.empty());
    }
    CHECK(svc == 5);
    CHECK(sch == 5);
    CHECK(cnt == 458);
    REQUIRE(snap.descriptors.size() == 1);
    CHECK(snap.descriptors[0].sgdd.id == "urn:digicap:sgdd:1409");
    CHECK(snap.descriptors[0].sgdd.version == 30u);
    CHECK(snap.descriptors[0].location == "sgdd_1919");
    CHECK(snap.files.size() == 5);
    const auto png = e.esgFile("s53098_h3_aa.png");
    REQUIRE(png);
    CHECK(png->content_type == "image/png");
    CHECK(png->bytes.size() == 6384);
    const std::int64_t capture_s = e.status().capture_unix_ns / 1'000'000'000;
    CHECK(capture_s >= 1678228500);   // 2023-03-07 17:55 UTC: the capture's own clock, used as "now" by the EPG
    CHECK(capture_s <= 1678228700);
}

TEST_CASE("Engine: media segment store of a selected service feeds the local DASH origin", "[service][corpus][media]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 8'000'000'000;
    o.media_cache_bytes = 3u << 20;   // small on purpose: eviction must kick in on the 4.5 Mbit/s video
    e.select(1409, 5004, true);
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 120));
    const auto idx = e.media(1409, 5004, 0);
    REQUIRE(idx);
    CHECK(idx->selected);
    CHECK(idx->mpd_type == "dynamic");
    CHECK(idx->max_segment_duration_s == 1.001);
    CHECK(idx->cached_bytes <= 3u << 20);
    CHECK(idx->evicted > 0);
    REQUIRE(idx->representations.size() == 4);
    const auto& video = idx->representations[0];
    CHECK(video.id == "Video1_1");
    CHECK(video.kind == "video");
    CHECK(video.init);
    CHECK(video.init_name == "video-init.mp4");
    CHECK(video.cached_segments >= 2);
    REQUIRE(video.last_key);
    CHECK(*video.last_key > 1676000000u);   // numbers in units of 1.001 s since the 1970 epoch (Sinclair)
    CHECK(video.number_shift == 0);         // epoch-anchored numbering agrees with the tfdt: no calibration
    CHECK(video.effective_start_number == video.start_number);
    const auto& audio = idx->representations[1];
    CHECK(audio.kind == "audio");
    CHECK(audio.init);
    CHECK(audio.cached_segments >= 2);
    bool init_seen = false;
    for (const auto& sg : idx->segments) {
        CHECK_FALSE(sg.name.empty());
        if (sg.is_init) init_seen = true;
        if (sg.rep_id == "Video1_1" && !sg.is_init) CHECK(sg.content_type == "video/mp4");
    }
    CHECK(init_seen);
    const auto init = e.mediaObject(1409, 5004, "video-init.mp4");
    REQUIRE(init);
    REQUIRE(init->data.size() > 8);
    CHECK(std::string(reinterpret_cast<const char*>(init->data.data()) + 4, 4) == "ftyp");
    CHECK(e.mediaObject(1409, 5004, "video-0.mp4v") == nullptr);
    const std::string newest = "video-" + std::to_string(*video.last_key) + ".mp4v";
    REQUIRE(e.mediaObject(1409, 5004, newest));
    CHECK(e.media(1409, 5004, idx->next_seq)->segments.empty());   // ?since= semantics
    const auto origin = e.originMpd(1409, 5004, "/media/1409/5004/", "/api/v1/time/xsdate", "/media/1409/5004/mpd.mpd");
    REQUIRE(origin);
    CHECK(origin->find("<BaseURL>/media/1409/5004/</BaseURL>") != std::string::npos);
    CHECK(origin->find("availabilityStartTime=\"1970-01-01T00:00:00Z\"") != std::string::npos);
    const auto offline = e.staticMpd(1409, 5004);
    REQUIRE(offline);
    CHECK(offline->find("type=\"static\"") != std::string::npos);
    // deselecting drops the store; an unselected service never fills it
    e.select(1409, 5004, false);
    CHECK(e.media(1409, 5004, 0)->segments.empty());
    CHECK(e.media(1409, 5003, 0)->segments.empty());
    CHECK_FALSE(e.media(1409, 1, 0));
}

TEST_CASE("Engine: $Number$ startNumber is calibrated to the media timeline (UniSoft usc01 off by 67/65 segments, usc02 by one)",
          "[service][corpus][media]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 20'000'000'000;
    e.select(24, 1, true);
    e.select(24, 2, true);
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 180));
    // usc01: Period@start 0, no presentationTimeOffset, startNumber 67 (video) / 65 (audio), but tfdt = number x 2.002 s
    const auto idx = e.media(24, 1, 0);
    REQUIRE(idx);
    REQUIRE(idx->representations.size() == 2);
    const auto& video = idx->representations[0];
    const auto& audio = idx->representations[1];
    CHECK(video.kind == "video");
    CHECK(video.start_number == 67);
    CHECK(video.number_shift == 67);
    CHECK(video.effective_start_number == 0);
    CHECK(audio.kind == "audio");
    CHECK(audio.start_number == 65);
    CHECK(audio.number_shift == 65);
    CHECK(audio.effective_start_number == 0);
    REQUIRE(video.last_key);
    REQUIRE(audio.last_key);
    REQUIRE(idx->availability_start_ns);
    REQUIRE(idx->live_edge_unix_ms);
    // segment n covers [n x 2.002, (n + 1) x 2.002) on the media timeline: the service clock is the end of the newest one
    const double newest = static_cast<double>(std::max(*video.last_key, *audio.last_key));
    const double expect_ms = static_cast<double>(*idx->availability_start_ns) / 1e6 + (newest + 1.0) * 2002.0;
    CHECK(std::fabs(static_cast<double>(*idx->live_edge_unix_ms) - expect_ms) < 50.0);
    bool tfdt_seen = false, timescale_seen = false;
    for (const auto& sg : idx->segments) {
        if (!sg.is_init && sg.tfdt) tfdt_seen = true;
        if (sg.is_init && sg.track_timescale == 1000000u) timescale_seen = true;
    }
    CHECK(tfdt_seen);
    CHECK(timescale_seen);
    const auto origin = e.originMpd(24, 1, "/media/24/1/", "/media/24/1/time.xsdate", "/media/24/1/mpd.mpd");
    REQUIRE(origin);
    INFO(*origin);
    CHECK(origin->find("startNumber=\"67\"") == std::string::npos);
    CHECK(origin->find("startNumber=\"65\"") == std::string::npos);
    std::size_t zeros = 0;
    for (std::size_t q = origin->find("startNumber=\"0\""); q != std::string::npos; q = origin->find("startNumber=\"0\"", q + 1)) ++zeros;
    CHECK(zeros == 2);
    // usc02 declares Period@start and presentationTimeOffset and is still one segment (2.1 s) early
    const auto idx2 = e.media(24, 2, 0);
    REQUIRE(idx2);
    REQUIRE(idx2->representations.size() >= 2);
    for (const auto& r : idx2->representations) {
        if (r.kind != "video" && r.kind != "audio") continue;
        INFO(r.id);
        CHECK(r.start_number == 16229);
        CHECK(r.number_shift == -1);
        CHECK(r.effective_start_number == 16230);
    }
}

TEST_CASE("Engine: MMT service reconstructs MPUs into MSE init segments and fragments (UniSoft, tfdt kept)", "[service][corpus][mmt]") {
    const std::string p = corpus("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 20'000'000'000;
    e.select(24, 3, true);
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 180));
    const auto idx = e.mmtMedia(24, 3, 0);
    REQUIRE(idx);
    CHECK(idx->selected);
    REQUIRE(idx->anchor_ntp);
    REQUIRE(idx->anchor_unix_ms);
    CHECK(*idx->anchor_unix_ms / 1000 == 1573003709);   // 2019-11-06 01:28:29 UTC, the first MPU_timestamp_descriptor
    REQUIRE(idx->components.size() == 2);
    const service::MmtComponentInfo* video = nullptr;
    const service::MmtComponentInfo* audio = nullptr;
    for (const auto& c : idx->components) {
        if (c.packet_id == 20) video = &c;
        if (c.packet_id == 21) audio = &c;
    }
    REQUIRE(video);
    REQUIRE(audio);
    CHECK(video->kind == "video");
    CHECK(video->sample_entry == "hev1");
    CHECK(video->codecs == "hev1.2.4.L120.B0");   // from hvcC; the MPD-less MMT path has no other source
    CHECK(video->mime == "video/mp4");
    CHECK(video->timescale == 1000000u);
    CHECK(video->width == 1920);
    CHECK(video->height == 1080);
    CHECK(video->timeline_mode == "keep");
    CHECK(video->fragments >= 6);
    CHECK(video->init_seq);
    CHECK(audio->asset_type == "ac-4");             // the MP table lies: the sample entry is AAC-LC
    CHECK(audio->kind == "audio");
    CHECK(audio->sample_entry == "mp4a");
    CHECK(audio->codecs == "mp4a.40.2");
    CHECK(audio->sample_rate == 48000u);
    CHECK(audio->fragments >= 6);
    REQUIRE(video->timestamp_offset_s);
    REQUIRE(audio->timestamp_offset_s);
    CHECK(std::abs(*video->timestamp_offset_s - *audio->timestamp_offset_s) < 0.01);   // one shared service timeline
    // fragments: styp + moof + mdat, contiguous start times per component, no damage on a loss-free capture
    std::map<std::uint16_t, double> last_end;
    std::size_t checked = 0;
    for (const auto& f : idx->fragments) {
        CHECK_FALSE(f.damaged);
        CHECK(f.duration_s > 1.9);
        CHECK(f.duration_s < 2.1);
        const auto it = last_end.find(f.packet_id);
        if (it != last_end.end()) CHECK(std::abs(f.start_s - it->second) < 0.02);
        last_end[f.packet_id] = f.start_s + f.duration_s;
        const auto frag = e.mmtFragment(24, 3, f.packet_id, f.mpu);
        REQUIRE(frag);
        REQUIRE(frag->bytes.size() > 32);
        CHECK(std::string(reinterpret_cast<const char*>(frag->bytes.data()) + 4, 4) == "styp");
        CHECK(std::string(reinterpret_cast<const char*>(frag->bytes.data()) + 28, 4) == "moof");
        CHECK_FALSE(frag->tfdt_rewritten);
        CHECK(frag->presentation_ntp);
        ++checked;
    }
    CHECK(checked == idx->fragments.size());
    CHECK(idx->live_edge_s);
    const auto init = e.mmtInit(24, 3, 20);
    REQUIRE(init);
    const std::string head(reinterpret_cast<const char*>(init->bytes.data()), init->bytes.size());
    CHECK(head.compare(4, 4, "ftyp") == 0);
    CHECK(head.find("mmpu") == std::string::npos);
    CHECK(head.find("moov") != std::string::npos);
    CHECK(head.find("mmth") == std::string::npos);    // hint track removed
    CHECK(init->hint_track_id == 2u);
    CHECK(init->hint_sample_size == 34u);
    // the service detail shows the init-derived codecs on the components
    const auto services = e.services();
    const auto* svc = find(services, 3);
    REQUIRE(svc);
    bool saw_aac = false;
    for (const auto& c : svc->components)
        if (c.packet_id == 21 && c.codecs == "mp4a.40.2") saw_aac = true;
    CHECK(saw_aac);
    // deselecting drops the store and stops assembly
    e.select(24, 3, false);
    CHECK(e.mmtMedia(24, 3, 0)->fragments.empty());
    CHECK_FALSE(e.mmtMedia(24, 1, 0));   // ROUTE service
}

TEST_CASE("Engine: MMT service with restarting tfdt is rewritten onto the presentation clock (Sinclair WNUV)", "[service][corpus][mmt]") {
    const std::string p = corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap");
    if (!exists(p)) SKIP("corpus not available");
    service::Engine e;
    pcap::VirtualTunerConfig cfg;
    service::OpenOptions o;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 10'000'000'000;
    e.select(1409, 5007, true);
    REQUIRE(e.open(pcapTuner(p, cfg), o));
    REQUIRE(waitFinished(e, 180));
    const auto idx = e.mmtMedia(1409, 5007, 0);
    REQUIRE(idx);
    REQUIRE(idx->components.size() == 4);   // video 100, audio 200/201, captions 300
    std::map<std::uint16_t, const service::MmtComponentInfo*> by_pid;
    for (const auto& c : idx->components) by_pid[c.packet_id] = &c;
    REQUIRE(by_pid.count(100));
    REQUIRE(by_pid.count(200));
    REQUIRE(by_pid.count(300));
    CHECK(by_pid[100]->codecs == "hev1.2.4.L93.B0");
    CHECK(by_pid[100]->width == 1280);
    CHECK(by_pid[100]->timeline_mode == "rewrite");
    CHECK(by_pid[100]->fragments >= 5);
    CHECK(by_pid[200]->sample_entry == "ac-4");
    CHECK(by_pid[200]->codecs.rfind("ac-4.", 0) == 0);
    CHECK(by_pid[200]->lang == "eng");
    CHECK(by_pid[201]->lang == "spa");
    CHECK(by_pid[300]->kind == "subtitles");
    CHECK(by_pid[300]->sample_entry == "stpp");
    std::map<std::uint16_t, double> last_end;
    bool any_rewritten = false;
    for (const auto& f : idx->fragments) {
        if (f.tfdt_rewritten) any_rewritten = true;
        CHECK(std::abs(f.duration_s - 1.001) < 0.001);
        const auto it = last_end.find(f.packet_id);
        if (it != last_end.end()) CHECK(std::abs(f.start_s - it->second) < 0.005);   // 1.001 s steps from the descriptor
        last_end[f.packet_id] = f.start_s + f.duration_s;
    }
    CHECK(any_rewritten);
    // audio sits a constant 16.683 ms after video on this feed; both start near the service anchor
    REQUIRE(last_end.count(100));
    REQUIRE(last_end.count(200));
    CHECK(std::abs(last_end[200] - last_end[100]) < 1.1);
}
