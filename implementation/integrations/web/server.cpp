#include "atsc3/web/server.hpp"

#include <atomic>
#include <cctype>
#include <optional>
#include <cmath>
#include <ctime>
#include <limits>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <httplib.h>

#include "atsc3/esg/esg.hpp"
#include "atsc3/tuners/pcap/format_detector.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"
#include "atsc3/tuners/pcap/sidecar_xa3.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/version.hpp"
#include "atsc3/web/json.hpp"

namespace fs = std::filesystem;

namespace atsc3::web {

const char* name() noexcept { return "atsc3-web"; }

namespace {

const char* formatName(pcap::CaptureFormat f) {
    switch (f) {
        case pcap::CaptureFormat::Alp: return "ALP";
        case pcap::CaptureFormat::Bbp: return "BBP";
        case pcap::CaptureFormat::Stltp: return "STLTP";
        case pcap::CaptureFormat::IpBypass: return "IP";
        default: return "unsupported";
    }
}

std::string mimeFor(const fs::path& p) {
    const std::string e = p.extension().string();
    if (e == ".html") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs") return "text/javascript; charset=utf-8";
    if (e == ".css") return "text/css; charset=utf-8";
    if (e == ".json" || e == ".map") return "application/json";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".png") return "image/png";
    if (e == ".ico") return "image/x-icon";
    if (e == ".woff2") return "font/woff2";
    if (e == ".woff") return "font/woff";
    return "application/octet-stream";
}

void writeComponent(Json& j, const service::ComponentInfo& c) {
    j.beginObject();
    j.kv("kind", c.kind).kv("transport", c.transport).kv("tsi", c.tsi).kv("packetId", c.packet_id).kv("id", c.id);
    j.kv("codecs", c.codecs).kv("mimeType", c.mime_type).kv("lang", c.lang).kv("name", c.name).kvList("roles", c.roles);
    j.kv("bandwidth", c.bandwidth).kv("width", c.width).kv("height", c.height).kv("frameRate", c.frame_rate);
    j.kv("audioSamplingRate", c.audio_sampling_rate).kv("timescale", c.timescale);
    j.kv("protected", c.content_protected).kv("defaultAsset", c.default_asset).kv("realTime", c.real_time).kv("session", c.session);
    j.kv("objects", c.objects).kv("bytes", c.bytes).kv("packets", c.packets).kv("errors", c.errors);
    j.kv("lastToi", c.last_toi).kv("lastMpuSequence", c.last_mpu_sequence).kv("lastName", c.last_name);
    j.endObject();
}

void writeApp(Json& j, const service::AppInfo& a) {
    j.beginObject();
    j.kv("appContextId", a.app_context_id).kv("appId", a.app_id).kv("bcastEntryPackageUrl", a.bcast_entry_package_url);
    j.kv("bcastEntryPageUrl", a.bcast_entry_page_url).kv("bbandEntryPageUrl", a.bband_entry_page_url).kv("requiredCapabilities", a.required_capabilities);
    j.kv("appRendering", a.app_rendering);
    j.key("coupledServices").beginArray();
    for (const std::uint16_t v : a.coupled_services) j.value(static_cast<unsigned>(v));
    j.endArray();
    j.key("lctTsiRef").beginArray();
    for (const std::uint32_t v : a.lct_tsi_ref) j.value(static_cast<std::uint64_t>(v));
    j.endArray();
    j.kv("validFromNs", a.valid_from_ns).kv("validUntilNs", a.valid_until_ns);
    j.kv("ready", a.ready).kv("files", static_cast<std::uint64_t>(a.files)).kv("bytes", static_cast<std::uint64_t>(a.bytes));
    j.kv("packageTsi", a.package_tsi).kv("packageBytes", a.package_bytes).kv("packageReceived", a.package_received);
    j.endObject();
}

void writeService(Json& j, const service::ServiceInfo& s) {
    j.beginObject();
    j.kv("llsGroup", static_cast<unsigned>(s.lls_group)).kv("bsid", static_cast<unsigned>(s.bsid)).kv("serviceId", static_cast<unsigned>(s.service_id));
    j.kv("globalServiceId", s.global_service_id).kv("major", s.major).kv("minor", s.minor);
    j.kv("shortName", s.short_name).kv("usbdName", s.usbd_name).kv("category", static_cast<unsigned>(s.category)).kv("categoryName", s.category_name);
    j.kv("protocol", s.protocol).kv("protected", s.protected_flag).kv("hidden", s.hidden).kv("hideInGuide", s.hide_in_guide);
    j.kv("broadbandRequired", s.broadband_required).kv("essential", s.essential).kv("drmSystemId", s.drm_system_id);
    j.kv("slsAddress", s.sls_address).kv("slsReceived", s.sls_received).kv("slsUpdates", s.sls_updates).kv("slsToi", static_cast<std::uint64_t>(s.sls_toi));
    j.kv("selected", s.selected).kvList("languages", s.languages).kv("mmtPackageId", s.mmt_package_id);
    j.kv("objects", s.objects).kv("objectErrors", s.object_errors).kv("bytes", s.bytes).kv("mmtpPackets", s.mmtp_packets);
    j.key("mpd").beginObject();
    j.kv("profiles", s.mpd_profiles).kv("type", s.mpd_type).kv("minBufferTime", s.mpd_min_buffer_s).kv("maxSegmentDuration", s.mpd_max_segment_s);
    j.kv("timeShiftBufferDepth", s.mpd_time_shift_s).kv("publishTimeNs", s.mpd_publish_ns).kv("availabilityStartNs", s.mpd_availability_start_ns);
    j.endObject();
    j.key("components").beginArray();
    for (const auto& c : s.components) writeComponent(j, c);
    j.endArray();
    j.key("slsFragments").beginArray();
    for (const auto& f : s.sls_fragments) {
        j.beginObject().kv("kind", f.kind).kv("contentType", f.content_type).kv("location", f.location).kv("version", f.version).kv("bytes", static_cast<std::uint64_t>(f.bytes)).endObject();
    }
    j.endArray();
    j.key("apps").beginArray();
    for (const auto& a : s.apps) writeApp(j, a);
    j.endArray();
    j.kvList("warnings", s.warnings);
    j.endObject();
}

/// The A/344 revision the receiver API implements (A/344:2024-04, released 3 April 2024): the `rev` query term.
constexpr const char* kA344Rev = "20240403";

/// The entry page with the receiver's WebSocket shim as the first script in <head> (spec 5.18.4): a Broadcaster
/// Application's `new WebSocket(wsURL + "/atscCmd")` is bridged to the hosting page instead of a socket.
std::string injectShim(const std::string& html) {
    const std::string tag = "<script src=\"/a344-shim.js\"></script>";
    std::string lower = html;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto after = [&](const std::string& open) -> std::optional<std::size_t> {
        const auto p = lower.find(open);
        if (p == std::string::npos) return std::nullopt;
        const auto end = lower.find('>', p);
        if (end == std::string::npos) return std::nullopt;
        return end + 1;
    };
    std::optional<std::size_t> at = after("<head");
    if (!at) at = after("<html");
    if (!at) at = 0;
    return html.substr(0, *at) + tag + html.substr(*at);
}

template <class S> void writeStats(Json& j, const char* key, const S& s);

}  // namespace

struct Server::Impl {
    service::Engine& engine;
    ServerConfig cfg;
    httplib::Server svr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::uint16_t bound_port = 0;
    std::mutex cache_mu;
    std::map<std::string, CaptureEntry> capture_cache;  // key: path|mtime
    // ESG: the guide is rebuilt from the engine's fragment snapshot whenever the engine's ESG version moved.
    std::mutex guide_mu;
    esg::Guide guide;
    service::EsgStats guide_stats;
    std::uint64_t guide_version = ~std::uint64_t{0};

    Impl(service::Engine& e, ServerConfig c) : engine(e), cfg(std::move(c)) {}

    bool authorized(const httplib::Request& req) const {
        if (cfg.token.empty()) return true;
        const std::string auth = req.get_header_value("Authorization");
        return auth == "Bearer " + cfg.token || req.get_param_value("token") == cfg.token;
    }

    static void json(httplib::Response& res, const Json& j, int status = 200) {
        res.status = status;
        res.set_header("Cache-Control", "no-store");
        res.set_content(j.str(), "application/json");
    }
    static void error(httplib::Response& res, int status, const std::string& message) {
        Json j;
        j.beginObject().kv("error", message).endObject();
        json(res, j, status);
    }

    bool allowedCapture(const std::string& path) const {
        std::error_code ec;
        const fs::path canon = fs::weakly_canonical(fs::path(path), ec);
        if (ec) return false;
        for (const std::string& d : cfg.captureDirs) {
            const fs::path base = fs::weakly_canonical(fs::path(d), ec);
            if (ec) continue;
            const std::string b = base.string(), c = canon.string();
            if (c.size() >= b.size() && c.compare(0, b.size(), b) == 0 && (c.size() == b.size() || c[b.size()] == '/')) return true;
        }
        return false;
    }

    void writeStatus(Json& j) {
        const service::Status st = engine.status();
        j.beginObject();
        j.kv("version", st.version).kv("esgVersion", st.esg_version).kv("stackVersion", std::string(atsc3::versionString()));
        j.kv("captureUnixMs", st.capture_unix_ns > 0 ? std::optional<std::int64_t>(st.capture_unix_ns / 1'000'000) : std::nullopt);
        j.key("source").beginObject();
        const auto vt = std::dynamic_pointer_cast<pcap::VirtualTuner>(engine.tuner());   // the daemon only opens captures
        const pcap::VirtualTuner::Stats ts = vt ? vt->stats() : pcap::VirtualTuner::Stats{};
        j.kv("open", st.open).kv("path", st.source).kv("name", fs::path(st.source).filename().string()).kv("error", st.error);
        j.kv("tuner", st.tuner).kv("output", service::toString(st.output));
        j.kv("format", vt ? formatName(vt->mode()) : "unsupported").kv("detection", st.detection);
        j.kv("pacing", vt && vt->config().pacing == pcap::Pacing::NonRealTime ? "nonrealtime" : "realtime").kv("speed", vt ? vt->config().speed : 1.0).kv("loop", vt && vt->config().loop);
        j.kv("paused", st.paused).kv("finished", st.finished).kv("positionNs", st.position_ns).kv("openedAtUnixMs", st.opened_at_unix_ms);
        j.endObject();
        j.key("llsGroups").beginArray();
        for (const std::uint8_t g : st.lls_groups) j.value(static_cast<unsigned>(g));
        j.endArray();
        j.key("bsids").beginArray();
        for (const std::uint16_t b : st.bsids) j.value(static_cast<unsigned>(b));
        j.endArray();
        j.kv("services", static_cast<std::uint64_t>(st.services)).kv("selected", static_cast<std::uint64_t>(st.selected));
        j.key("stats").beginObject();
        j.key("tuner").beginObject().kv("records", ts.records).kv("bbp", ts.delivered_bbp).kv("alp", ts.delivered_alp).kv("ip", ts.delivered_ip)
            .kv("droppedNonIp", ts.dropped_non_ip).kv("timeJumps", ts.time_jumps).kv("wraps", ts.wraps).kv("misrouted", st.misrouted_units).endObject();
        j.key("deframer").beginObject().kv("bbp", st.deframer.bbp).kv("alp", st.deframer.alp).kv("pendingMismatch", st.deframer.pending_mismatch)
            .kv("alpInvalid", st.deframer.alp_invalid).kv("counterJumps", st.deframer.counter_jumps).endObject();
        j.key("alp").beginObject().kv("packets", st.decap.packets).kv("ipv4", st.decap.ipv4).kv("signaling", st.decap.signaling).kv("segments", st.decap.segments)
            .kv("reassembled", st.decap.reassembled).kv("invalid", st.decap.invalid).kv("truncated", st.decap.truncated).endObject();
        j.key("ip").beginObject().kv("ipv4", st.demux.ipv4).kv("udp", st.demux.udp).kv("routed", st.demux.routed).kv("unrouted", st.demux.unrouted)
            .kv("fragments", st.demux.fragments).kv("reassembled", st.demux.reassembled).kv("badIpv4", st.demux.bad_ipv4).endObject();
        j.key("lls").beginObject().kv("datagrams", st.lls.datagrams).kv("tables", st.lls.tables).kv("newTables", st.lls.new_tables).kv("duplicates", st.lls.duplicates)
            .kv("parseFailed", st.lls.parse_failed).kv("gunzipFailed", st.lls.gunzip_failed).endObject();
        j.key("link").beginObject().kv("lmtNew", st.link.lmt_new).kv("lmt", st.link.lmt).kv("invalid", st.link.invalid).kv("rdtNew", st.link.rdt_new).endObject();
        j.endObject();
        j.endObject();
    }

    /// Runs `fn(guide)` under the guide lock with an up-to-date guide.
    template <class F> void withGuide(F&& fn) {
        std::lock_guard<std::mutex> lock(guide_mu);
        const std::uint64_t v = engine.esgVersion();
        if (v != guide_version) {
            const service::EsgSnapshot snap = engine.esg();
            guide = esg::Guide::fromFragments(snap.fragments);
            guide_stats = snap.stats;
            guide_version = snap.version;
        }
        fn(guide);
    }

    struct SltMatch {
        std::optional<std::uint16_t> bsid, service_id;
        std::string short_name;
        bool selected = false;
    };
    /// Joins an ESG Service fragment with the SLT service table: globalServiceID first, then major.minor.
    static SltMatch matchSlt(const esg::Service& es, const std::vector<service::ServiceInfo>& services) {
        SltMatch m;
        for (const service::ServiceInfo& s : services) {
            if (es.global_service_id && s.global_service_id && *es.global_service_id == *s.global_service_id) {
                m.bsid = s.bsid;
                m.service_id = s.service_id;
                m.short_name = s.short_name;
                m.selected = s.selected;
                return m;
            }
        }
        if (es.major && es.minor) {
            for (const service::ServiceInfo& s : services) {
                if (s.major == es.major && s.minor == es.minor) {
                    m.bsid = s.bsid;
                    m.service_id = s.service_id;
                    m.short_name = s.short_name;
                    m.selected = s.selected;
                    return m;
                }
            }
        }
        return m;
    }

    static std::string iconUrl(const std::vector<esg::Icon>& icons) {
        for (const esg::Icon& i : icons) {
            if (i.uri.empty()) continue;
            if (i.uri.rfind("http://", 0) == 0 || i.uri.rfind("https://", 0) == 0) return i.uri;
            return "/api/v1/esg/icons/" + httplib::detail::encode_url(i.uri);
        }
        return "";
    }

    static void writeRatings(Json& j, const std::vector<esg::Rating>& ratings) {
        j.key("ratings").beginArray();
        for (const esg::Rating& r : ratings) {
            j.beginObject().kv("description", r.description).kv("lang", r.lang).kv("scheme", r.scheme);
            j.key("region");
            if (r.region) j.value(static_cast<unsigned>(*r.region));
            else j.null();
            j.key("dimensions").beginArray();
            for (const auto& [dim, val] : r.dimensions) j.beginObject().kv("dimension", static_cast<unsigned>(dim)).kv("value", val).endObject();
            j.endArray().endObject();
        }
        j.endArray();
    }

    static void writeLangTexts(Json& j, const char* key, const std::vector<esg::LangText>& texts) {
        j.key(key).beginArray();
        for (const esg::LangText& t : texts) j.beginObject().kv("lang", t.lang).kv("text", t.text).endObject();
        j.endArray();
    }

    static void writeIcons(Json& j, const std::vector<esg::Icon>& icons) {
        j.key("icons").beginArray();
        for (const esg::Icon& i : icons) {
            const std::string url = i.uri.rfind("http://", 0) == 0 || i.uri.rfind("https://", 0) == 0 ? i.uri : "/api/v1/esg/icons/" + httplib::detail::encode_url(i.uri);
            j.beginObject().kv("uri", i.uri).kv("url", url).kv("mime", i.mime).kv("width", i.width).kv("height", i.height).kv("dataSize", i.data_size).endObject();
        }
        j.endArray();
    }

    void writeEsgService(Json& j, const esg::Service& es, const std::vector<service::ServiceInfo>& services, std::size_t programmes) {
        const SltMatch m = matchSlt(es, services);
        j.beginObject();
        j.kv("id", es.id).kv("version", static_cast<std::uint64_t>(es.version)).kv("globalServiceId", es.global_service_id);
        j.kv("name", esg::pickText(es.names)).kv("description", esg::pickText(es.descriptions));
        j.kv("serviceType", es.service_type).kv("major", es.major).kv("minor", es.minor).kv("emergency", es.emergency);
        j.kv("validFrom", es.valid_from_unix).kv("validTo", es.valid_to_unix);
        j.kv("icon", iconUrl(es.icons));
        writeIcons(j, es.icons);
        j.kvList("genres", es.genres).kvList("audioLanguages", es.audio_languages).kvList("textLanguages", es.text_languages);
        writeRatings(j, es.ratings);
        j.kv("bsid", m.bsid).kv("serviceId", m.service_id).kv("shortName", m.short_name).kv("selected", m.selected);
        j.kv("programmes", static_cast<std::uint64_t>(programmes));
        j.endObject();
    }

    void writeContent(Json& j, const esg::Content& c, bool with_xml) {
        j.beginObject();
        j.kv("id", c.id).kv("version", static_cast<std::uint64_t>(c.version)).kv("validFrom", c.valid_from_unix).kv("validTo", c.valid_to_unix);
        j.kv("title", esg::pickText(c.names)).kv("description", esg::pickText(c.descriptions));
        writeLangTexts(j, "names", c.names);
        writeLangTexts(j, "descriptions", c.descriptions);
        j.kv("lengthS", c.length_s).kvList("serviceIds", c.service_ids).kvList("genres", c.genres);
        j.kvList("audioLanguages", c.audio_languages).kvList("textLanguages", c.text_languages);
        j.kv("icon", iconUrl(c.icons));
        writeIcons(j, c.icons);
        writeRatings(j, c.ratings);
        j.key("components").beginArray();
        for (const esg::Component& comp : c.components) j.beginObject().kv("kind", comp.kind).kv("language", comp.language).kv("text", comp.text).endObject();
        j.endArray();
        j.kvList("capabilities", c.capabilities);
        if (with_xml) j.kv("xml", c.xml);
        j.endObject();
    }

    void writeProgramme(Json& j, const esg::Programme& p, const std::map<std::string, SltMatch>& matches) {
        j.beginObject();
        j.kv("serviceId", p.service_id).kv("contentId", p.content_id).kv("scheduleId", p.schedule_id).kv("start", p.start_unix).kv("end", p.end_unix);
        const auto m = matches.find(p.service_id);
        j.kv("bsid", m == matches.end() ? std::optional<std::uint16_t>{} : m->second.bsid);
        j.kv("sltServiceId", m == matches.end() ? std::optional<std::uint16_t>{} : m->second.service_id);
        if (p.content) {
            j.kv("title", esg::pickText(p.content->names)).kv("description", esg::pickText(p.content->descriptions));
            j.kv("lengthS", p.content->length_s).kvList("genres", p.content->genres).kv("icon", iconUrl(p.content->icons));
            std::string rating;
            for (const esg::Rating& r : p.content->ratings) {
                if (!r.description.empty()) {
                    rating = r.description;
                    break;
                }
            }
            j.kv("rating", rating);
        } else {
            j.kv("title", "").kv("description", "").kv("lengthS", std::optional<double>{}).kvList("genres", std::vector<std::string>{}).kv("icon", "").kv("rating", "");
        }
        j.endObject();
    }

    static std::optional<std::int64_t> paramInt(const httplib::Request& req, const char* k) {
        if (!req.has_param(k)) return std::nullopt;
        const std::string v = req.get_param_value(k);
        if (v.empty()) return std::nullopt;
        return static_cast<std::int64_t>(std::strtoll(v.c_str(), nullptr, 10));
    }

    void esgRoutes() {
        svr.Get("/api/v1/time", [this](const httplib::Request&, httplib::Response& res) {
            const service::Status st = engine.status();
            Json j;
            j.beginObject();
            j.kv("wallUnixMs", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            j.kv("captureUnixMs", st.capture_unix_ns > 0 ? std::optional<std::int64_t>(st.capture_unix_ns / 1'000'000) : std::nullopt);
            j.kv("positionNs", st.position_ns).kv("open", st.open).kv("paused", st.paused).kv("finished", st.finished);
            j.endObject();
            json(res, j);
        });
        svr.Get("/api/v1/esg", [this](const httplib::Request&, httplib::Response& res) {
            const service::EsgSnapshot snap = engine.esg();
            const auto services = engine.services();
            Json j;
            withGuide([&](const esg::Guide& g) {
                j.beginObject();
                j.kv("version", snap.version);
                j.key("stats").beginObject();
                j.kv("objects", snap.stats.objects).kv("sgdd", snap.stats.sgdd).kv("sgdu", snap.stats.sgdu).kv("files", snap.stats.files).kv("fileBytes", static_cast<std::uint64_t>(snap.stats.file_bytes));
                j.kv("ignored", snap.stats.ignored).kv("parseErrors", snap.stats.parse_errors).kv("fragmentsSeen", snap.stats.fragments_seen);
                j.kv("fragmentsNew", snap.stats.fragments_new).kv("fragmentsUpdated", snap.stats.fragments_updated).kv("fragmentsDuplicate", snap.stats.fragments_duplicate);
                j.kv("fragments", static_cast<std::uint64_t>(snap.fragments.size()));
                const esg::GuideStats& gs = g.stats();
                j.kv("services", static_cast<std::uint64_t>(gs.services)).kv("schedules", static_cast<std::uint64_t>(gs.schedules)).kv("contents", static_cast<std::uint64_t>(gs.contents));
                j.kv("programmes", static_cast<std::uint64_t>(gs.programmes)).kv("failed", static_cast<std::uint64_t>(gs.failed)).kv("unresolvedContents", static_cast<std::uint64_t>(gs.unresolved_contents));
                j.endObject();
                const auto span = g.span();
                j.kv("spanStart", span ? std::optional<std::int64_t>(span->first) : std::nullopt).kv("spanEnd", span ? std::optional<std::int64_t>(span->second) : std::nullopt);
                std::map<std::string, std::size_t> per_service;
                for (const esg::Programme& p : g.programmes()) ++per_service[p.service_id];
                j.key("services").beginArray();
                for (const esg::Service* es : g.services()) writeEsgService(j, *es, services, per_service[es->id]);
                j.endArray();
                j.key("esgServices").beginArray();   // the SLT services that carry the guide
                for (const service::ServiceInfo& s : services) {
                    if (s.category != 4) continue;
                    j.beginObject().kv("bsid", static_cast<unsigned>(s.bsid)).kv("serviceId", static_cast<unsigned>(s.service_id)).kv("shortName", s.short_name)
                        .kv("slsReceived", s.sls_received).kv("objects", s.objects).kv("bytes", s.bytes).endObject();
                }
                j.endArray();
                j.key("descriptors").beginArray();
                for (const service::EsgDescriptorInfo& d : snap.descriptors) {
                    j.beginObject().kv("id", d.sgdd.id).kv("version", d.sgdd.version).kv("location", d.location).kv("bsid", static_cast<unsigned>(d.bsid))
                        .kv("serviceId", static_cast<unsigned>(d.service_id)).kv("tsi", static_cast<std::uint64_t>(d.tsi)).kv("toi", static_cast<std::uint64_t>(d.toi)).kv("receivedNs", d.received_ns);
                    j.key("entries").beginArray();
                    for (const service::SgddEntry& e : d.sgdd.entries) {
                        j.beginObject().kv("start", e.start_unix).kv("end", e.end_unix).kv("tsi", e.tsi).kv("ip", e.ip).kv("port", e.port);
                        j.key("units").beginArray();
                        for (const service::SgddUnit& u : e.units) {
                            std::size_t svc = 0, sch = 0, cnt = 0, other = 0;
                            for (const service::SgddFragmentRef& r : u.fragments) {
                                if (r.type == service::EsgFragmentType::Service) ++svc;
                                else if (r.type == service::EsgFragmentType::Schedule) ++sch;
                                else if (r.type == service::EsgFragmentType::Content) ++cnt;
                                else ++other;
                            }
                            j.beginObject().kv("toi", u.toi).kv("contentLocation", u.content_location).kv("fragments", static_cast<std::uint64_t>(u.fragments.size()))
                                .kv("service", static_cast<std::uint64_t>(svc)).kv("schedule", static_cast<std::uint64_t>(sch)).kv("content", static_cast<std::uint64_t>(cnt)).kv("other", static_cast<std::uint64_t>(other)).endObject();
                        }
                        j.endArray().endObject();
                    }
                    j.endArray().endObject();
                }
                j.endArray();
                j.key("files").beginArray();
                for (const service::EsgFileInfo& f : snap.files) {
                    j.beginObject().kv("name", f.name).kv("url", "/api/v1/esg/icons/" + httplib::detail::encode_url(f.name)).kv("contentType", f.content_type)
                        .kv("bytes", static_cast<std::uint64_t>(f.bytes)).kv("tsi", static_cast<std::uint64_t>(f.tsi)).kv("toi", static_cast<std::uint64_t>(f.toi)).kv("receivedNs", f.received_ns).endObject();
                }
                j.endArray();
                j.endObject();
            });
            json(res, j);
        });
        svr.Get("/api/v1/esg/services", [this](const httplib::Request&, httplib::Response& res) {
            const auto services = engine.services();
            Json j;
            withGuide([&](const esg::Guide& g) {
                std::map<std::string, std::size_t> per_service;
                for (const esg::Programme& p : g.programmes()) ++per_service[p.service_id];
                j.beginArray();
                for (const esg::Service* es : g.services()) writeEsgService(j, *es, services, per_service[es->id]);
                j.endArray();
            });
            json(res, j);
        });
        svr.Get("/api/v1/esg/schedule", [this](const httplib::Request& req, httplib::Response& res) {
            // ?from=<unix s>&to=<unix s>[&service=<ESG service id>]; defaults: everything
            const std::int64_t from = paramInt(req, "from").value_or(std::numeric_limits<std::int64_t>::min());
            const std::int64_t to = paramInt(req, "to").value_or(std::numeric_limits<std::int64_t>::max());
            std::optional<std::string> service_id;
            if (req.has_param("service") && !req.get_param_value("service").empty()) service_id = req.get_param_value("service");
            const auto services = engine.services();
            Json j;
            withGuide([&](const esg::Guide& g) {
                std::map<std::string, SltMatch> matches;
                for (const esg::Service* es : g.services()) matches[es->id] = matchSlt(*es, services);
                j.beginObject().kv("from", from).kv("to", to);
                j.key("programmes").beginArray();
                for (const esg::Programme& p : g.programmes(from, to, service_id ? &*service_id : nullptr)) writeProgramme(j, p, matches);
                j.endArray().endObject();
            });
            json(res, j);
        });
        svr.Get(R"(/api/v1/esg/content/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1];
            bool found = false;
            Json j;
            withGuide([&](const esg::Guide& g) {
                const esg::Content* c = g.content(id);
                if (!c) return;
                found = true;
                writeContent(j, *c, true);
            });
            if (!found) return error(res, 404, "unknown content");
            json(res, j);
        });
        svr.Get(R"(/api/v1/esg/icons/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string name = req.matches[1];
            const auto f = engine.esgFile(name);
            if (!f) return error(res, 404, "unknown icon");
            res.set_header("Cache-Control", "max-age=60");
            res.set_content(std::string(f->bytes.begin(), f->bytes.end()), f->content_type);
        });
        svr.Get("/api/v1/esg/fragments", [this](const httplib::Request& req, httplib::Response& res) {
            // ?type=Service|Schedule|Content&limit=N&xml=1 (raw fragments for the signaling inspector)
            const service::EsgSnapshot snap = engine.esg();
            const std::string type = req.get_param_value("type");
            const std::int64_t limit = paramInt(req, "limit").value_or(200);
            const bool with_xml = req.get_param_value("xml") == "1";
            Json j;
            j.beginObject().kv("version", snap.version).kv("total", static_cast<std::uint64_t>(snap.fragments.size()));
            j.key("fragments").beginArray();
            std::int64_t n = 0;
            for (const service::EsgFragment& f : snap.fragments) {
                if (!type.empty() && type != service::toString(f.type)) continue;
                if (n++ >= limit) break;
                j.beginObject().kv("type", std::string(service::toString(f.type))).kv("id", f.id).kv("version", static_cast<std::uint64_t>(f.version));
                j.kv("validFrom", f.valid_from_unix).kv("validTo", f.valid_to_unix).kv("bsid", static_cast<unsigned>(f.bsid)).kv("serviceId", static_cast<unsigned>(f.service_id));
                j.kv("toi", static_cast<std::uint64_t>(f.toi)).kv("transportId", static_cast<std::uint64_t>(f.transport_id)).kv("bytes", static_cast<std::uint64_t>(f.xml.size())).kv("receivedNs", f.received_ns);
                if (with_xml) j.kv("xml", f.xml);
                j.endObject();
            }
            j.endArray().endObject();
            json(res, j);
        });
    }

    static std::string xsDate(std::int64_t unix_ms) {
        const std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(unix_ms % 1000));
        return buf;
    }
    std::int64_t nowUnixMs() const {
        // the capture's own clock while a capture is open (dash.js takes "now" from here), else wall clock
        const service::Status st = engine.status();
        if (st.open && st.capture_unix_ns > 0) return st.capture_unix_ns / 1'000'000;
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Local DASH origin (spec 5.17.3): /media/{bsid}/{sid}/{mpd.mpd | offline.mpd | player.json | index.json | <segment>}.
    void mediaRoutes() {
        svr.Get("/api/v1/time/xsdate", [this](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "no-store");
            res.set_content(xsDate(nowUnixMs()), "text/plain; charset=utf-8");
        });
        auto ids = [](const httplib::Request& req, std::uint16_t& bsid, std::uint16_t& sid) {
            bsid = static_cast<std::uint16_t>(std::stoul(req.matches[1]));
            sid = static_cast<std::uint16_t>(std::stoul(req.matches[2]));
        };
        // Per-service "now" (spec 5.17.3): the availability time of the newest cached segment plus a small margin,
        // so dash.js's live edge is always inside the cache; the capture clock until segments exist.
        svr.Get(R"(/media/(\d+)/(\d+)/time\.xsdate)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto idx = engine.media(bsid, sid, 0);
            if (!idx) return error(res, 404, "unknown service");
            const std::int64_t now = idx->live_edge_unix_ms ? *idx->live_edge_unix_ms + 100 : nowUnixMs();
            res.set_header("Cache-Control", "no-store");
            res.set_content(xsDate(now), "text/plain; charset=utf-8");
        });
        svr.Get(R"(/media/(\d+)/(\d+)/mpd\.mpd)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const std::string base = "/media/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/";
            const auto mpd = engine.originMpd(bsid, sid, base, base + "time.xsdate", base + "mpd.mpd");
            if (!mpd) return error(res, 404, "no MPD for this service yet");
            res.set_header("Cache-Control", "no-store");
            res.set_content(*mpd, "application/dash+xml");
        });
        svr.Get(R"(/media/(\d+)/(\d+)/offline\.mpd)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto mpd = engine.staticMpd(bsid, sid);
            if (!mpd) return error(res, 404, "no cached segments for this service");
            res.set_header("Cache-Control", "no-store");
            res.set_content(*mpd, "application/dash+xml");
        });
        svr.Get(R"(/media/(\d+)/(\d+)/player\.json)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            if (const auto mmt = engine.mmtMedia(bsid, sid, 0)) {   // MMTP service: the push player (spec 5.17.4, HTTP variant)
                const std::string base = "/media/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/";
                double longest = 0.0;
                for (const auto& f : mmt->fragments) longest = std::max(longest, f.duration_s);
                // the cache may still be empty when the panel asks: never go below the 2 s-MPU value (4.5 s)
                const double live_delay = std::max(2.0 * longest + 0.5, 4.5);
                Json j;
                j.beginObject().kv("mode", "push").kv("indexUrl", base + "mmt/index.json").kv("liveDelay", live_delay).kv("minFragments", std::uint64_t{2})
                    .kv("protected", false).kv("selected", mmt->selected).endObject();
                return json(res, j);
            }
            const auto idx = engine.media(bsid, sid, 0);
            if (!idx) return error(res, 404, "unknown service");
            if (idx->mpd_type.empty()) return error(res, 404, "no MPD for this service yet");
            const double seg = idx->max_segment_duration_s.value_or(2.0);
            bool prot = false;
            for (const auto& r : idx->representations) prot = prot || r.content_protected;
            const std::string base = "/media/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/";
            Json j;
            // the player should hold liveDelay plus one segment before starting (startup gate in PlayerPanel)
            std::size_t min_segments = static_cast<std::size_t>(std::ceil((2.0 * seg + 0.5) / seg)) + 1;
            j.beginObject().kv("mode", "dash").kv("mpdUrl", base + "mpd.mpd").kv("timeUrl", base + "time.xsdate").kv("liveDelay", 2.0 * seg + 0.5).kv("liveCatchup", false)
                .kv("autoSwitchBitrate", false).kv("protected", prot).kv("selected", idx->selected).kv("minSegments", static_cast<std::uint64_t>(min_segments)).endObject();
            json(res, j);
        });
        svr.Get(R"(/media/(\d+)/(\d+)/index\.json)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const std::uint64_t since = req.has_param("since") ? std::strtoull(req.get_param_value("since").c_str(), nullptr, 10) : 0;
            const auto idx = engine.media(bsid, sid, since);
            if (!idx) return error(res, 404, "unknown service");
            const std::string base = "/media/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/";
            Json j;
            j.beginObject().kv("bsid", static_cast<unsigned>(bsid)).kv("serviceId", static_cast<unsigned>(sid)).kv("selected", idx->selected).kv("nextSeq", idx->next_seq);
            j.kv("mpdType", idx->mpd_type).kv("maxSegmentDuration", idx->max_segment_duration_s).kv("timeShiftBufferDepth", idx->time_shift_buffer_depth_s);
            j.kv("cachedBytes", static_cast<std::uint64_t>(idx->cached_bytes)).kv("cacheLimit", static_cast<std::uint64_t>(idx->cache_limit)).kv("evicted", idx->evicted);
            j.kv("availabilityStartMs", idx->availability_start_ns ? std::optional<std::int64_t>(*idx->availability_start_ns / 1'000'000) : std::nullopt);
            j.kv("periodStartS", idx->period_start_s).kv("liveEdgeMs", idx->live_edge_unix_ms).kv("captureMs", nowUnixMs());
            j.key("representations").beginArray();
            for (const auto& r : idx->representations) {
                j.beginObject().kv("id", r.id).kv("kind", r.kind).kv("codecs", r.codecs).kv("mimeType", r.mime_type).kv("lang", r.lang).kv("bandwidth", r.bandwidth);
                j.kv("width", r.width).kv("height", r.height).kv("timescale", r.timescale).kv("segmentDuration", r.segment_duration).kv("initName", r.init_name);
                j.kv("startNumber", r.start_number).kv("effectiveStartNumber", r.effective_start_number).kv("numberShift", r.number_shift).kv("addressing", r.addressing);
                j.kv("mediaTemplate", r.media_template).kv("protected", r.content_protected).kv("init", r.init).kv("cachedSegments", static_cast<std::uint64_t>(r.cached_segments));
                j.kv("firstKey", r.first_key).kv("lastKey", r.last_key).endObject();
            }
            j.endArray();
            j.key("segments").beginArray();
            for (const auto& sg : idx->segments) {
                j.beginObject().kv("seq", sg.seq).kv("name", sg.name).kv("repId", sg.rep_id).kv("init", sg.is_init).kv("key", sg.key).kv("bytes", static_cast<std::uint64_t>(sg.bytes));
                j.kv("tsi", static_cast<std::uint64_t>(sg.tsi)).kv("toi", static_cast<std::uint64_t>(sg.toi)).kv("receivedNs", sg.received_ns).kv("contentType", sg.content_type);
                j.kv("url", base + httplib::detail::encode_url(sg.name)).endObject();
            }
            j.endArray().endObject();
            json(res, j);
        });
        // MMT push path (spec 5.17.4, HTTP variant): index of reconstructed MPU segments, init and fragments per packet_id
        svr.Get(R"(/media/(\d+)/(\d+)/mmt/index\.json)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const std::uint64_t since = req.has_param("since") ? std::strtoull(req.get_param_value("since").c_str(), nullptr, 10) : 0;
            const auto idx = engine.mmtMedia(bsid, sid, since);
            if (!idx) return error(res, 404, "not an MMTP service");
            const std::string base = "/media/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/mmt/";
            Json j;
            j.beginObject().kv("bsid", static_cast<unsigned>(bsid)).kv("serviceId", static_cast<unsigned>(sid)).kv("selected", idx->selected).kv("nextSeq", idx->next_seq);
            j.kv("generation", idx->generation);
            j.kv("anchorNtp", idx->anchor_ntp).kv("anchorUnixMs", idx->anchor_unix_ms).kv("liveEdgeS", idx->live_edge_s);
            j.kv("cachedBytes", static_cast<std::uint64_t>(idx->cached_bytes)).kv("cacheLimit", static_cast<std::uint64_t>(idx->cache_limit)).kv("evicted", idx->evicted);
            j.key("components").beginArray();
            for (const auto& c : idx->components) {
                j.beginObject().kv("packetId", static_cast<unsigned>(c.packet_id)).kv("assetId", c.asset_id).kv("assetType", c.asset_type).kv("defaultAsset", c.default_asset);
                j.kv("kind", c.kind).kv("sampleEntry", c.sample_entry).kv("codecs", c.codecs).kv("mime", c.mime).kv("lang", c.lang).kv("timescale", static_cast<std::uint64_t>(c.timescale));
                j.kv("width", c.width).kv("height", c.height).kv("channels", c.channels).kv("sampleRate", c.sample_rate);
                j.key("init");
                if (c.init_seq) {
                    j.beginObject().kv("seq", *c.init_seq).kv("bytes", static_cast<std::uint64_t>(c.init_bytes)).kv("url", base + std::to_string(c.packet_id) + "/init.mp4").endObject();
                } else {
                    j.null();
                }
                j.kv("timestampOffsetS", c.timestamp_offset_s);   // null until the assembler froze it (first fragment)
                j.kv("mode", c.timeline_mode).kv("fragments", static_cast<std::uint64_t>(c.fragments));
                j.kv("lastMpu", c.last_mpu).kv("lastEndS", c.last_end_s).kv("damaged", c.damaged).kv("dropped", c.dropped).kv("mpuGaps", c.mpu_gaps).endObject();
            }
            j.endArray();
            j.key("fragments").beginArray();
            for (const auto& f : idx->fragments) {
                j.beginObject().kv("seq", f.seq).kv("packetId", static_cast<unsigned>(f.packet_id)).kv("mpu", static_cast<std::uint64_t>(f.mpu));
                j.kv("url", base + std::to_string(f.packet_id) + "/" + std::to_string(f.mpu) + ".m4s").kv("bytes", static_cast<std::uint64_t>(f.bytes));
                j.kv("startS", f.start_s).kv("durationS", f.duration_s).kv("endS", f.start_s + f.duration_s);
                j.kv("damaged", f.damaged).kv("discontinuity", f.discontinuity).kv("tfdtRewritten", f.tfdt_rewritten).kv("presentationUnixMs", f.presentation_unix_ms).endObject();
            }
            j.endArray().endObject();
            json(res, j);
        });
        svr.Get(R"(/media/(\d+)/(\d+)/mmt/(\d+)/init\.mp4)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto pid = static_cast<std::uint16_t>(std::stoul(req.matches[3]));
            const auto init = engine.mmtInit(bsid, sid, pid);
            if (!init) return error(res, 404, "no init segment for this packet id");
            res.set_header("Cache-Control", "no-store");
            res.set_content(reinterpret_cast<const char*>(init->bytes.data()), init->bytes.size(), init->media.containerMime());
        });
        svr.Get(R"(/media/(\d+)/(\d+)/mmt/(\d+)/(\d+)\.m4s)", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto pid = static_cast<std::uint16_t>(std::stoul(req.matches[3]));
            const auto mpu = static_cast<std::uint32_t>(std::stoul(req.matches[4]));
            const auto f = engine.mmtFragment(bsid, sid, pid, mpu);
            if (!f) return error(res, 404, "fragment not available");
            const auto init = engine.mmtInit(bsid, sid, pid);
            res.set_header("Cache-Control", "max-age=3600");
            res.set_content(reinterpret_cast<const char*>(f->bytes.data()), f->bytes.size(), init ? init->media.containerMime() : "application/mp4");
        });
        svr.Get(R"(/media/(\d+)/(\d+)/(.+))", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const std::string name = req.matches[3];
            // a segment that is still being received may land within a moment: short long-poll (spec 5.17.3)
            std::shared_ptr<const route::DeliveredObject> obj;
            for (int i = 0; i < 15; ++i) {
                obj = engine.mediaObject(bsid, sid, name);
                if (obj) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!obj) return error(res, 404, "segment not available");
            const auto idx = engine.media(bsid, sid, 0);
            std::string type = "application/mp4";
            if (idx) {
                for (const auto& sg : idx->segments)
                    if (sg.name == name) type = sg.content_type;
            }
            res.set_header("Cache-Control", "max-age=3600");
            res.set_header("Accept-Ranges", "bytes");
            // cpp-httplib serves Range requests itself for content given with set_content
            res.set_content(reinterpret_cast<const char*>(obj->data.data()), obj->data.size(), type);
        });
    }

    // Application Context Cache origin (spec 5.18.4): /apps/{bsid}/{sid}/index.json and
    // /apps/{bsid}/{sid}/{app}/{path}; outside the bearer check like /media (the app's iframe sends no header).
    void appsRoutes() {
        auto ids = [](const httplib::Request& req, std::uint16_t& bsid, std::uint16_t& sid) {
            bsid = static_cast<std::uint16_t>(std::stoul(req.matches[1]));
            sid = static_cast<std::uint16_t>(std::stoul(req.matches[2]));
        };
        auto wsUrl = [this](const httplib::Request& req, std::uint16_t bsid, std::uint16_t sid) {
            std::string host = req.get_header_value("Host");
            if (host.empty()) host = cfg.bindAddress + ":" + std::to_string(bound_port);
            return "ws://" + host + "/api/v1/a344/" + std::to_string(bsid) + "/" + std::to_string(sid);
        };
        auto entryUrl = [](const std::string& base, std::size_t n, const service::AppInfo& a, const std::string& ws) -> std::optional<std::string> {
            if (!a.bcast_entry_page_url) return std::nullopt;
            std::string page = *a.bcast_entry_page_url;
            while (page.rfind("./", 0) == 0) page.erase(0, 2);
            while (!page.empty() && page.front() == '/') page.erase(0, 1);
            return base + std::to_string(n) + "/" + page + "?wsURL=" + httplib::detail::encode_url(ws) + "&rev=" + kA344Rev;
        };
        svr.Get(R"(/apps/(\d+)/(\d+)/index\.json)", [this, ids, wsUrl, entryUrl](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto idx = engine.apps(bsid, sid);
            if (!idx) return error(res, 404, "unknown service");
            const std::string base = "/apps/" + std::to_string(bsid) + "/" + std::to_string(sid) + "/";
            const std::string ws = wsUrl(req, bsid, sid);
            Json j;
            j.beginObject();
            j.kv("bsid", static_cast<unsigned>(bsid)).kv("serviceId", static_cast<unsigned>(sid)).kv("selected", idx->selected);
            j.kv("wsUrl", ws).kv("rev", kA344Rev);
            j.key("apps").beginArray();
            for (std::size_t n = 0; n < idx->apps.size(); ++n) {
                const service::AppInfo& a = idx->apps[n];
                j.beginObject();
                j.kv("index", static_cast<std::uint64_t>(n)).kv("appContextId", a.app_context_id).kv("appId", a.app_id);
                j.kv("bcastEntryPackageUrl", a.bcast_entry_package_url).kv("bcastEntryPageUrl", a.bcast_entry_page_url).kv("bbandEntryPageUrl", a.bband_entry_page_url);
                j.kv("requiredCapabilities", a.required_capabilities).kv("appRendering", a.app_rendering);
                j.key("coupledServices").beginArray();
                for (const std::uint16_t v : a.coupled_services) j.value(static_cast<unsigned>(v));
                j.endArray();
                j.kv("validFromNs", a.valid_from_ns).kv("validUntilNs", a.valid_until_ns);
                j.kv("ready", a.ready).kv("files", static_cast<std::uint64_t>(a.files)).kv("bytes", static_cast<std::uint64_t>(a.bytes));
                j.kv("packageTsi", a.package_tsi).kv("packageBytes", a.package_bytes).kv("packageReceived", a.package_received);
                j.kv("baseUrl", base + std::to_string(n) + "/").kv("entryUrl", entryUrl(base, n, a, ws));
                j.endObject();
            }
            j.endArray();
            j.key("files").beginArray();
            for (const service::AppFileInfo& f : idx->files) {
                j.beginObject().kv("path", f.path).kv("contentType", f.content_type).kv("bytes", static_cast<std::uint64_t>(f.bytes)).kv("version", f.version);
                j.kv("package", f.package).kvList("appContextIds", f.app_context_ids).kv("tsi", static_cast<std::uint64_t>(f.tsi)).kv("toi", static_cast<std::uint64_t>(f.toi));
                j.kv("receivedNs", f.received_ns).endObject();
            }
            j.endArray();
            j.kv("cachedBytes", static_cast<std::uint64_t>(idx->cached_bytes)).kv("cacheLimit", static_cast<std::uint64_t>(idx->cache_limit));
            j.kv("objects", idx->objects).kv("packages", idx->packages).kv("rejected", idx->rejected);
            j.endObject();
            json(res, j);
        });
        svr.Get(R"(/apps/(\d+)/(\d+)/(\d+)/(.+))", [this, ids](const httplib::Request& req, httplib::Response& res) {
            std::uint16_t bsid, sid;
            ids(req, bsid, sid);
            const auto n = static_cast<std::size_t>(std::stoul(req.matches[3]));
            const std::string path = req.matches[4];
            const auto idx = engine.apps(bsid, sid);
            if (!idx) return error(res, 404, "unknown service");
            if (n >= idx->apps.size()) return error(res, 404, "no such application");
            const auto f = engine.appFile(bsid, sid, path);
            if (!f) return error(res, 404, "not in the application context cache");
            std::string type = f->content_type;
            if (type.rfind("text/", 0) == 0 || type == "application/json" || type == "application/xml" || type == "image/svg+xml") type += "; charset=utf-8";
            res.set_header("Cache-Control", "no-cache");
            const service::AppInfo& a = idx->apps[n];
            std::string entry = a.bcast_entry_page_url.value_or("");
            while (entry.rfind("./", 0) == 0) entry.erase(0, 2);
            while (!entry.empty() && entry.front() == '/') entry.erase(0, 1);
            if (!entry.empty() && path == entry) {   // the entry page: bridge the receiver API (spec 5.18.4)
                res.set_content(injectShim(std::string(reinterpret_cast<const char*>(f->data()), f->size)), "text/html; charset=utf-8");
                return;
            }
            res.set_content(reinterpret_cast<const char*>(f->data()), f->size, type);
        });
    }

    void routes() {
        svr.set_default_headers({{"Access-Control-Allow-Origin", "*"}, {"Access-Control-Allow-Headers", "Authorization, Content-Type"},
                                 {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}});
        svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
        svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0 && req.method != "OPTIONS" && !authorized(req)) {
                error(res, 401, "unauthorized");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        svr.Get("/api/v1/status", [this](const httplib::Request& req, httplib::Response& res) {
            // optional long-poll: ?since=<version>&wait=<ms>
            if (req.has_param("since")) {
                const std::uint64_t since = std::strtoull(req.get_param_value("since").c_str(), nullptr, 10);
                int wait = req.has_param("wait") ? std::atoi(req.get_param_value("wait").c_str()) : 1000;
                wait = std::max(0, std::min(wait, 20000));
                engine.waitVersion(since, std::chrono::milliseconds(wait));
            }
            Json j;
            writeStatus(j);
            json(res, j);
        });
        svr.Get("/api/v1/services", [this](const httplib::Request&, httplib::Response& res) {
            Json j;
            j.beginArray();
            for (const auto& s : engine.services()) writeService(j, s);
            j.endArray();
            json(res, j);
        });
        svr.Get(R"(/api/v1/services/(\d+)/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            const auto d = engine.service(static_cast<std::uint16_t>(std::stoul(req.matches[1])), static_cast<std::uint16_t>(std::stoul(req.matches[2])));
            if (!d) return error(res, 404, "unknown service");
            Json j;
            j.beginObject();
            j.key("service");
            writeService(j, d->info);
            j.key("fragments").beginArray();
            for (const auto& [k, text] : d->fragments) j.beginObject().kv("name", k).kv("text", text).endObject();
            j.endArray();
            j.kvList("recentObjects", d->recent_objects);
            j.endObject();
            json(res, j);
        });
        svr.Get(R"(/api/v1/services/(\d+)/(\d+)/sls/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            const auto d = engine.service(static_cast<std::uint16_t>(std::stoul(req.matches[1])), static_cast<std::uint16_t>(std::stoul(req.matches[2])));
            if (!d) return error(res, 404, "unknown service");
            const std::string want = req.matches[3];
            for (const auto& [k, text] : d->fragments) {
                if (k == want) {
                    res.set_header("Cache-Control", "no-store");
                    res.set_content(text, "application/xml; charset=utf-8");
                    return;
                }
            }
            error(res, 404, "unknown fragment");
        });
        auto select = [this](const httplib::Request& req, httplib::Response& res, bool on) {
            const auto bsid = static_cast<std::uint16_t>(std::stoul(req.matches[1]));
            const auto sid = static_cast<std::uint16_t>(std::stoul(req.matches[2]));
            engine.select(bsid, sid, on);
            Json j;
            j.beginObject().kv("bsid", static_cast<unsigned>(bsid)).kv("serviceId", static_cast<unsigned>(sid)).kv("selected", on).endObject();
            json(res, j);
        };
        svr.Post(R"(/api/v1/services/(\d+)/(\d+)/select)", [select](const httplib::Request& req, httplib::Response& res) { select(req, res, true); });
        svr.Delete(R"(/api/v1/services/(\d+)/(\d+)/select)", [select](const httplib::Request& req, httplib::Response& res) { select(req, res, false); });

        svr.Get("/api/v1/lls", [this](const httplib::Request&, httplib::Response& res) {
            Json j;
            j.beginArray();
            for (const auto& t : engine.llsTables()) {
                j.beginObject().kv("group", static_cast<unsigned>(t.group)).kv("tableId", static_cast<unsigned>(t.table_id)).kv("name", t.name)
                    .kv("version", static_cast<unsigned>(t.version)).kv("sourceIp", t.source_ip).kv("rootNamespace", t.root_namespace)
                    .kv("bytes", static_cast<std::uint64_t>(t.bytes)).kv("timeNs", t.t_ns).kv("xml", t.xml).endObject();
            }
            j.endArray();
            json(res, j);
        });
        svr.Get("/api/v1/link", [this](const httplib::Request&, httplib::Response& res) {
            const service::LinkInfo l = engine.link();
            Json j;
            j.beginObject();
            j.key("lmts").beginArray();
            for (const auto& [carrier, lmt] : l.lmts) {
                j.beginObject().kv("carrierPlp", static_cast<unsigned>(carrier)).kv("version", static_cast<unsigned>(lmt.version));
                j.key("plps").beginArray();
                for (const auto& p : lmt.plps) {
                    j.beginObject().kv("plpId", static_cast<unsigned>(p.plp_id));
                    j.key("multicasts").beginArray();
                    for (const auto& m : p.multicasts) {
                        j.beginObject().kv("src", io::ipv4ToString(m.src_ip)).kv("dst", io::ipv4ToString(m.dst_ip)).kv("srcPort", static_cast<unsigned>(m.src_port))
                            .kv("dstPort", static_cast<unsigned>(m.dst_port)).kv("sid", m.sid).kv("contextId", m.context_id).endObject();
                    }
                    j.endArray().endObject();
                }
                j.endArray().endObject();
            }
            j.endArray();
            j.key("plpMap").beginArray();
            for (const auto& e : l.plp_map) {
                j.beginObject().kv("plpId", static_cast<unsigned>(e.plp_id)).kv("dst", io::ipv4ToString(e.dst_ip)).kv("dstPort", static_cast<unsigned>(e.dst_port))
                    .kv("src", e.src_ip ? io::ipv4ToString(e.src_ip) : std::string("*")).kv("origin", e.origin == io::LmtOrigin::Sidecar ? "sidecar" : "broadcast").endObject();
            }
            j.endArray();
            j.key("l1");
            if (l.l1) {
                j.beginObject().kv("lmtOrigin", l.l1->lmt_origin == io::LmtOrigin::Sidecar ? "sidecar" : "broadcast");
                j.key("plps").beginArray();
                for (const auto& p : l.l1->plps) {
                    j.beginObject().kv("plpId", static_cast<unsigned>(p.plp_id)).kv("llsFlag", p.lls_flag).kv("modulation", p.modulation).kv("codeRate", p.code_rate)
                        .kv("fecCodeLength", p.fec_code_length).kv("fecOuterCode", p.fec_outer_code).kv("layer", p.layer).endObject();
                }
                j.endArray().endObject();
            } else {
                j.null();
            }
            j.key("unrouted").beginArray();
            for (const auto& u : l.unrouted) {
                j.beginObject().kv("dst", io::ipv4ToString(u.dst_ip)).kv("dstPort", static_cast<unsigned>(u.dst_port)).kv("src", io::ipv4ToString(u.src_ip))
                    .kv("packets", u.packets).kv("bytes", u.bytes).kv("plp", static_cast<unsigned>(u.plp)).endObject();
            }
            j.endArray();
            j.endObject();
            json(res, j);
        });

        esgRoutes();
        mediaRoutes();
        appsRoutes();

        svr.Get("/api/v1/source", [this](const httplib::Request&, httplib::Response& res) {
            Json j;
            j.beginObject();
            j.kvList("directories", cfg.captureDirs);
            j.key("captures").beginArray();
            for (const CaptureEntry& c : list()) {
                j.beginObject().kv("path", c.path).kv("name", c.name).kv("dir", c.dir).kv("size", c.size).kv("mtime", c.mtime_unix).kv("format", c.format)
                    .kv("detection", c.detection).kv("sidecar", c.sidecar).endObject();
            }
            j.endArray();
            j.endObject();
            json(res, j);
        });
        svr.Post("/api/v1/source", [this](const httplib::Request& req, httplib::Response& res) {
            // body: application/x-www-form-urlencoded or query params: path, pacing, speed, loop, start
            auto param = [&](const char* k) -> std::string {
                if (req.has_param(k)) return req.get_param_value(k);
                return "";
            };
            const std::string action = param("action");
            if (action == "close") {
                engine.close();
            } else if (action == "pause" || action == "resume") {
                engine.pause(action == "pause");
            } else {
                const std::string path = param("path");
                if (path.empty()) return error(res, 400, "path required");
                if (!allowedCapture(path)) return error(res, 403, "path is outside the capture directories");
                pcap::VirtualTunerConfig tuner_cfg;
                tuner_cfg.path = path;
                tuner_cfg.pacing = param("pacing") == "nonrealtime" ? pcap::Pacing::NonRealTime : pcap::Pacing::RealTime;
                if (!param("speed").empty()) tuner_cfg.speed = std::max(0.1, std::min(100.0, std::atof(param("speed").c_str())));
                tuner_cfg.loop = param("loop") == "1" || param("loop") == "true";
                if (!param("start").empty()) tuner_cfg.start_offset_ns = static_cast<std::int64_t>(std::atof(param("start").c_str()) * 1e9);
                std::string err;
                if (!engine.open(std::make_shared<pcap::VirtualTuner>(tuner_cfg), service::OpenOptions{}, &err)) return error(res, 422, err);
            }
            Json j;
            writeStatus(j);
            json(res, j);
        });

        if (!cfg.staticDir.empty()) {
            svr.Get(R"(/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
                if (req.path.rfind("/api/", 0) == 0) return error(res, 404, "not found");
                std::string rel = req.matches[1];
                if (rel.empty()) rel = "index.html";
                if (rel.find("..") != std::string::npos) return error(res, 403, "forbidden");
                fs::path p = fs::path(cfg.staticDir) / rel;
                std::error_code ec;
                if (!fs::is_regular_file(p, ec)) p = fs::path(cfg.staticDir) / "index.html";  // SPA fallback
                std::ifstream f(p, std::ios::binary);
                if (!f) return error(res, 404, "frontend not built");
                std::stringstream ss;
                ss << f.rdbuf();
                res.set_header("Cache-Control", rel == "index.html" ? "no-store" : "max-age=3600");
                res.set_content(ss.str(), mimeFor(p));
            });
        } else {
            svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
                res.set_content("<html><body><h1>atsc3d</h1><p>API at <a href=\"/api/v1/status\">/api/v1/status</a>; frontend not configured (--www).</p></body></html>",
                                "text/html; charset=utf-8");
            });
        }
    }

    std::vector<CaptureEntry> list() {
        std::vector<CaptureEntry> out;
        for (const std::string& d : cfg.captureDirs) {
            std::error_code ec;
            if (!fs::is_directory(d, ec)) continue;
            for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = it->path().extension().string();
                if (ext != ".pcap" && ext != ".pcapng" && ext != ".cap") continue;
                CaptureEntry e;
                e.path = it->path().string();
                e.name = it->path().filename().string();
                e.dir = fs::relative(it->path().parent_path(), d, ec).string();
                if (e.dir == ".") e.dir.clear();
                e.size = static_cast<std::uint64_t>(it->file_size(ec));
                const auto mt = fs::last_write_time(it->path(), ec);
                e.mtime_unix = std::chrono::duration_cast<std::chrono::seconds>(mt.time_since_epoch()).count();
                const std::string key = e.path + "|" + std::to_string(e.mtime_unix) + "|" + std::to_string(e.size);
                {
                    std::lock_guard<std::mutex> lock(cache_mu);
                    const auto c = capture_cache.find(key);
                    if (c != capture_cache.end()) {
                        out.push_back(c->second);
                        continue;
                    }
                }
                std::string err;
                auto reader = pcap::PcapReader::open(e.path, &err);
                if (reader) {
                    pcap::DetectorConfig dc;
                    const pcap::Detection det = pcap::detectCaptureFormat(*reader, dc);
                    e.format = formatName(det.format);
                    e.detection = det.summary();
                } else {
                    e.format = "unreadable";
                    e.detection = err;
                }
                e.sidecar = pcap::findXa3Sidecar(e.path).has_value();
                {
                    std::lock_guard<std::mutex> lock(cache_mu);
                    capture_cache[key] = e;
                }
                out.push_back(e);
            }
        }
        std::sort(out.begin(), out.end(), [](const CaptureEntry& a, const CaptureEntry& b) { return a.path < b.path; });
        return out;
    }
};

Server::Server(service::Engine& engine, ServerConfig cfg) : impl_(std::make_unique<Impl>(engine, std::move(cfg))) { impl_->routes(); }
Server::~Server() { stop(); }

bool Server::start(std::string* error) {
    if (impl_->running) return true;
    if (!impl_->cfg.allowRemote && impl_->cfg.bindAddress != "127.0.0.1" && impl_->cfg.bindAddress != "localhost" && impl_->cfg.bindAddress != "::1") {
        if (error) *error = "binding to " + impl_->cfg.bindAddress + " requires allowRemote";
        return false;
    }
    int port = impl_->cfg.port;
    if (port == 0) {
        port = impl_->svr.bind_to_any_port(impl_->cfg.bindAddress);
        if (port < 0) {
            if (error) *error = "cannot bind";
            return false;
        }
    } else if (!impl_->svr.bind_to_port(impl_->cfg.bindAddress, port)) {
        if (error) *error = "cannot bind " + impl_->cfg.bindAddress + ":" + std::to_string(port);
        return false;
    }
    impl_->bound_port = static_cast<std::uint16_t>(port);
    impl_->running = true;
    impl_->thread = std::thread([this] {
        impl_->svr.listen_after_bind();
        impl_->running = false;
    });
    // wait until the listener is up
    for (int i = 0; i < 200 && !impl_->svr.is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return true;
}

void Server::stop() {
    if (impl_->thread.joinable()) {
        impl_->svr.stop();
        impl_->thread.join();
    }
    impl_->running = false;
}

bool Server::running() const { return impl_->running; }
std::uint16_t Server::port() const noexcept { return impl_->bound_port; }
std::vector<CaptureEntry> Server::listCaptures() { return impl_->list(); }

}  // namespace atsc3::web
