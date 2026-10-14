#include "atsc3/service/engine.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <thread>

#include "atsc3/io/inet.hpp"
#include "atsc3/route/multipart.hpp"
#include "atsc3/sls/bundle.hpp"
#include "atsc3/media/isobmff.hpp"
#include "atsc3/sls/mpd.hpp"
#include "atsc3/util/gzip.hpp"

namespace atsc3::service {

namespace {
std::string kindFromCodec(const std::string& fourcc, std::uint8_t usbd_type, bool have_usbd_type) {
    if (have_usbd_type) {
        switch (usbd_type) {
            case 0: return "audio";
            case 1: return "video";
            case 2: return "subtitles";
            case 3: return "application";
            default: break;
        }
    }
    const std::string c = fourcc.substr(0, 4);
    if (c == "hev1" || c == "hvc1" || c == "avc1" || c == "avc3" || c == "av01" || c == "vvc1") return "video";
    if (c == "ac-4" || c == "mp4a" || c == "ec-3" || c == "ac-3" || c == "mhm1" || c == "mhm2" || c == "Opus") return "audio";
    if (c == "stpp" || c == "wvtt") return "subtitles";
    return "data";
}
/// $Number$ calibration (spec 5.17.3): where the numbering puts the newest cached segment, (number - startNumber) x
/// duration, against where its media really is, tfdt - presentationTimeOffset. Off by more than half a segment: the
/// difference, rounded to whole segments, becomes the shift and the effective startNumber moves by it. The offset is
/// never touched: a per-representation presentationTimeOffset would move audio and video apart, a startNumber shift
/// only changes which file the player fetches for a given time.
void calibrateNumbering(MediaRepresentationInfo& ri, std::optional<std::uint64_t> pto, std::optional<std::uint64_t> newest_tfdt,
                        std::optional<std::uint32_t> track_timescale) {
    ri.effective_start_number = ri.start_number;
    ri.number_shift = 0;
    if (ri.addressing != "number" || !ri.segment_duration || *ri.segment_duration == 0 || !ri.last_key || !newest_tfdt) return;
    const double tmpl_ts = static_cast<double>(ri.timescale.value_or(1));
    const double track_ts = static_cast<double>(track_timescale.value_or(ri.timescale.value_or(1)));
    if (tmpl_ts <= 0.0 || track_ts <= 0.0) return;
    const double d_s = static_cast<double>(*ri.segment_duration) / tmpl_ts;
    const double media_s = static_cast<double>(*newest_tfdt) / track_ts - static_cast<double>(pto.value_or(0)) / tmpl_ts;
    const double expected_s = static_cast<double>(static_cast<std::int64_t>(*ri.last_key) - static_cast<std::int64_t>(ri.start_number)) * d_s;
    const double delta = media_s - expected_s;
    if (std::fabs(delta) <= 0.5 * d_s) return;
    const auto shift = static_cast<std::int64_t>(std::llround(delta / d_s));
    if (shift == 0) return;
    const auto start = static_cast<std::int64_t>(ri.start_number);
    ri.number_shift = shift > start ? start : shift;   // never below number 0
    ri.effective_start_number = static_cast<std::uint64_t>(start - ri.number_shift);
}

std::string kindFromMpd(const std::optional<std::string>& content_type, const std::optional<std::string>& mime,
                        const std::optional<std::string>& codecs) {
    if (content_type && !content_type->empty()) return *content_type == "text" ? "subtitles" : *content_type;
    if (mime) {
        if (mime->rfind("video/", 0) == 0) return "video";
        if (mime->rfind("audio/", 0) == 0) return "audio";
    }
    if (codecs) return kindFromCodec(*codecs, 0, false);
    return "unknown";
}
std::int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string lowerMime(std::string s) {
    const auto semi = s.find(';');
    if (semi != std::string::npos) s.erase(semi);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool endsWithNoCase(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}
/// Media type of an application file from its name, for package parts that carry neither a part Content-Type nor an
/// envelope item (spec 5.18.4): what a browser needs to run the entry page and its assets.
std::string mimeFromName(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == "html" || e == "htm" || e == "xhtml") return "text/html";
    if (e == "js" || e == "mjs") return "text/javascript";
    if (e == "css") return "text/css";
    if (e == "json" || e == "map") return "application/json";
    if (e == "xml") return "application/xml";
    if (e == "txt") return "text/plain";
    if (e == "svg") return "image/svg+xml";
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "gif") return "image/gif";
    if (e == "webp") return "image/webp";
    if (e == "ico") return "image/x-icon";
    if (e == "woff2") return "font/woff2";
    if (e == "woff") return "font/woff";
    if (e == "ttf") return "font/ttf";
    if (e == "mp4" || e == "m4v") return "video/mp4";
    if (e == "m4s") return "video/iso.segment";
    if (e == "m4a") return "audio/mp4";
    if (e == "mpd") return "application/dash+xml";
    if (e == "ttml") return "application/ttml+xml";
    if (e == "vtt") return "text/vtt";
    if (e == "wasm") return "application/wasm";
    return "application/octet-stream";
}
/// Media types browsers reject or warn about, as broadcasters' envelopes write them (Sinclair: `application/js`).
std::string canonicalMime(std::string t) {
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "application/js" || t == "application/javascript" || t == "application/x-javascript" || t == "text/jscript") return "text/javascript";
    if (t == "text/json") return "application/json";
    if (t == "image/jpg") return "image/jpeg";
    return t;
}
/// Cache path of an application file: the Content-Location relative to the cache root ("./", "/" and a scheme-less
/// host prefix stripped), empty when it would escape the cache (".." segments).
std::string appPath(std::string p) {
    while (p.rfind("./", 0) == 0) p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    const auto q = p.find_first_of("?#");
    if (q != std::string::npos) p.erase(q);
    if (p == ".." || p.rfind("../", 0) == 0 || p.find("/../") != std::string::npos || (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0)) return "";
    return p;
}
constexpr const char* kSgddMime = "application/vnd.oma.bcast.sgdd+xml";
constexpr const char* kSgduMime = "application/vnd.oma.bcast.sgdu";
constexpr std::size_t kEsgFileCacheBytes = 48u << 20;   // icons of an ESG service: bounded, oldest evicted
}  // namespace

struct Engine::EsgStore {
    std::map<std::string, EsgFragment> fragments;                      // "type|id" -> latest
    std::vector<EsgDescriptorInfo> descriptors;                        // one per (service, location)
    std::map<std::string, std::pair<EsgFileInfo, EsgFile>> files;     // by Content-Location
    EsgStats stats;
};

struct Engine::Service {
    std::uint8_t lls_group = 0;
    std::uint16_t bsid = 0;
    lls::SltService slt;
    std::string protocol;
    std::unique_ptr<sls::RouteReceiver> route;
    std::unique_ptr<mmtp::Session> mmt;
    ip::SubscriptionId mmt_sub = 0;
    std::optional<sls::SlsBundle> sls;
    std::optional<mmtp::MmtUsbd> mmt_usbd;
    std::vector<std::pair<std::string, std::string>> mmt_fragments;  // name -> text
    std::uint64_t sls_updates = 0, objects = 0, object_errors = 0, bytes = 0;
    struct ChannelStats {
        std::uint64_t objects = 0, bytes = 0, errors = 0;
        std::optional<std::uint32_t> last_toi;
        std::optional<std::string> last_name;
    };
    std::map<std::uint32_t, ChannelStats> channels;  // by TSI
    std::vector<std::string> recent;
    std::vector<std::string> warnings;
    bool selected = false;
    // media segment store (spec 5.17.3)
    struct CachedSegment {
        MediaSegmentInfo info;
        std::shared_ptr<const route::DeliveredObject> obj;
    };
    std::deque<CachedSegment> media;   // arrival order; init segments pinned
    std::uint64_t media_seq = 0, media_evicted = 0;
    std::size_t media_bytes = 0;
    void clearMedia() {
        media.clear();
        media_bytes = 0;
    }
    // MMT media store (spec 5.17.4)
    struct MmtComp {
        std::shared_ptr<const mmtp::MpuInitSegment> init;
        std::uint64_t init_seq = 0;
        std::deque<std::pair<std::uint64_t, std::shared_ptr<const mmtp::MpuFragment>>> frags;
        std::uint64_t damaged = 0;
    };
    std::map<std::uint16_t, MmtComp> mmt_comps;
    std::uint64_t mmt_seq = 0, mmt_evicted = 0;
    std::size_t mmt_bytes = 0;
    std::vector<ip::SubscriptionId> mmt_extra_subs;   // asset flows announced by MMT_general_location_info type 1
    std::set<std::uint64_t> mmt_extra_keys;
    void clearMmt() {
        mmt_comps.clear();
        mmt_bytes = 0;
    }
    // Application Context Cache (spec 5.18.4): NRT files and unpacked package parts of a selected service
    struct CachedAppFile {
        AppFileInfo info;
        std::shared_ptr<const route::DeliveredObject> owner;
        std::size_t offset = 0, size = 0;
    };
    std::map<std::string, CachedAppFile> app_files;   // by cache path
    std::size_t app_bytes = 0;
    std::uint64_t app_objects = 0, app_packages = 0, app_rejected = 0;
    std::optional<sls::Held> mmt_held;                 // HELD of an MMTP service (mmt_atsc3_message 0x0003)
    // MMTP services: the ROUTE session named by the USBD ROUTEComponent (locally-cached content: app packages)
    std::unique_ptr<sls::RouteReceiver> route_nrt;
    std::string route_nrt_address;
    std::optional<sls::Stsid> route_nrt_stsid;
    std::vector<std::pair<std::string, std::string>> route_nrt_fragments;   // name -> text
};

class Engine::Sink final : public service::ITunerSink {
public:
    explicit Sink(Chain& c);
    void onBbp(const io::BasebandPacket& p) override;
    void onAlp(const io::AlpPacket& p) override;
    void onIp(const io::IpDatagram& p) override;
    void onTunerEvent(const service::TunerEvent& e) override;

private:
    Chain& chain_;
};

struct Engine::Chain {
    Engine& engine;
    OpenOptions opts;
    std::shared_ptr<ITuner> tuner;
    TunerOutput output = TunerOutput::Ip;   // the layer the tuner is coupled at; the layers below it are not built
    std::uint64_t misrouted = 0;            // units delivered for a layer that is not built
    std::unique_ptr<Sink> sink;
    io::UnitTime time;
    std::string detection;
    ip::Demux demux;
    std::unique_ptr<link::LinkSignaling> link;
    std::unique_ptr<alp::Decap> decap;
    std::unique_ptr<phy::BbpDeframer> deframer;
    std::unique_ptr<lls::Processor> lls;
    std::map<std::uint32_t, std::unique_ptr<Service>> services;  // (bsid<<16 | service_id)
    EsgStore esg;

    Chain(Engine& e, TunerOutput out) : engine(e), output(out) {
        link = std::make_unique<link::LinkSignaling>(link::LinkSignaling::Callbacks{});
        if (output != TunerOutput::Ip) {   // ALP and BBP tuners: ALP decapsulation feeds the IP demux and the link signaling
            alp::Callbacks acb;
            acb.onIpv4 = [this](std::uint8_t plp, ByteView ip, std::optional<std::uint8_t> sid) { demux.onIpv4(plp, ip, time, sid); };
            acb.onSignaling = [this](std::uint8_t plp, const alp::SignalingInfo& i, ByteView t) { link->onSignaling(plp, i, t); };
            decap = std::make_unique<alp::Decap>(std::move(acb));
        }
        if (output == TunerOutput::Bbp)    // BBP tuners: the deframer in front of it
            deframer = std::make_unique<phy::BbpDeframer>([this](std::uint8_t plp, ByteView alp) { decap->feed(plp, alp); });
        lls::Callbacks lcb;
        lcb.onSlt = [this](const lls::TableInfo& info, const lls::Slt& slt) { engine.onSlt(info, slt); };
        lls = std::make_unique<lls::Processor>(std::move(lcb));
        demux.tapAll([this](const ip::UdpDatagram& d) { lls->onDatagram(d); });
        sink = std::make_unique<Sink>(*this);
    }
};

Engine::Sink::Sink(Chain& c) : chain_(c) {}
void Engine::yieldToApi() const {
    if (api_waiters_.load(std::memory_order_relaxed) > 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
}
void Engine::Sink::onBbp(const io::BasebandPacket& p) {
    {
        std::lock_guard<std::mutex> lock(chain_.engine.mu_);
        chain_.time = p.time;
        if (chain_.deframer) chain_.deframer->feed(p.plp_id, p.bytes, p.error);
        else ++chain_.misrouted;
    }
    chain_.engine.yieldToApi();
}
void Engine::Sink::onAlp(const io::AlpPacket& p) {
    {
        std::lock_guard<std::mutex> lock(chain_.engine.mu_);
        chain_.time = p.time;
        if (chain_.decap) chain_.decap->feed(p.plp_id, p.bytes);
        else ++chain_.misrouted;
    }
    chain_.engine.yieldToApi();
}
void Engine::Sink::onIp(const io::IpDatagram& p) {
    {
        std::lock_guard<std::mutex> lock(chain_.engine.mu_);
        chain_.time = p.time;
        chain_.demux.onIpv4(p.plp_id, p.bytes, p.time);
    }
    chain_.engine.yieldToApi();
}
void Engine::Sink::onTunerEvent(const service::TunerEvent& e) {
    std::lock_guard<std::mutex> lock(chain_.engine.mu_);
    if (e.type == service::TunerEventType::FormatDetected) chain_.detection = e.message;
    if (e.type == service::TunerEventType::EndOfStream) {
        chain_.engine.status_.finished = true;
        chain_.engine.onEndOfStream();
    }
    if (e.type == service::TunerEventType::Wrap) {  // loop wrap: the capture's timelines start over
        chain_.engine.status_.finished = false;
        chain_.engine.onWrap();
    }
    chain_.engine.bump();
}

Engine::Engine() = default;
Engine::~Engine() { close(); }

void Engine::bump() {
    ++status_.version;
    cv_.notify_all();
}

std::uint64_t Engine::waitVersion(std::uint64_t since, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, timeout, [&] { return status_.version != since; });
    return status_.version;
}

namespace {
/// The layer to couple at: the caller's choice when the tuner offers it, else the highest layer offered (each layer
/// above BBP is work the tuner has already done).
std::optional<TunerOutput> chooseOutput(const std::set<TunerOutput>& offered, std::optional<TunerOutput> wanted) {
    if (wanted) return offered.count(*wanted) ? wanted : std::nullopt;
    for (const TunerOutput o : {TunerOutput::Ip, TunerOutput::Alp, TunerOutput::Bbp})
        if (offered.count(o)) return o;
    return std::nullopt;
}
}  // namespace

bool Engine::open(std::shared_ptr<ITuner> tuner, const OpenOptions& opts, std::string* error) {
    close();
    auto fail = [&](const std::string& msg) {
        ApiLock lock(*this);
        status_.open = false;
        status_.error = msg;
        if (error) *error = msg;
        bump();
        return false;
    };
    {
        ApiLock lock(*this);
        const std::uint64_t esg_version = status_.esg_version + 1;   // stays monotonic across sources (ESG caches key on it)
        const std::uint64_t generation = status_.media_generation + 1;   // the push player restarts on a new generation
        status_ = Status{};
        status_.esg_version = esg_version;
        status_.media_generation = generation;
        if (tuner) {
            status_.tuner = tuner->name();
            status_.source = tuner->source();
        }
    }
    if (!tuner) return fail("no tuner");
    const TunerState ts = tuner->state();
    if (!ts.ready) return fail(ts.error.empty() ? "tuner not ready" : ts.error);
    const std::optional<TunerOutput> output = chooseOutput(tuner->outputs(), opts.output);
    if (!output) return fail(opts.output ? std::string("tuner does not deliver ") + toString(*opts.output) : "tuner offers no output the stack can take");
    auto chain = std::make_unique<Chain>(*this, *output);
    chain->tuner = std::move(tuner);
    chain->opts = opts;
    {
        ApiLock lock(*this);
        status_.open = true;
        status_.output = *output;
        status_.opened_at_unix_ms = nowUnixMs();
        if (const std::optional<io::L1Info> l1 = chain->tuner->l1()) {  // keep the optional alive for the loop
            std::vector<io::LmtEntry> entries;
            for (const auto& l : l1->lmt) entries.push_back(l);
            chain->link->plpMap().applySidecar(entries);
        }
        chain_ = std::move(chain);
        bump();
    }
    const TunerError err = chain_->tuner->start(*chain_->sink, *output);
    if (err != TunerError::None) {
        {
            ApiLock lock(*this);
            chain_.reset();   // nothing is running: the chain can go under the lock
        }
        return fail(std::string("cannot start the tuner: ") + toString(err));
    }
    return true;
}

void Engine::close() {
    std::unique_ptr<Chain> old;
    {
        ApiLock lock(*this);
        old = std::move(chain_);
    }
    if (old && old->tuner) old->tuner->stop();  // joins the delivery thread; must not hold mu_
    ApiLock lock(*this);
    status_.open = false;
    status_.finished = false;
    bump();
}

bool Engine::pause(bool paused) {
    ApiLock lock(*this);
    const bool ok = chain_ && chain_->tuner && chain_->tuner->pause(paused);
    if (ok) status_.paused = paused;
    bump();
    return ok;
}

std::shared_ptr<ITuner> Engine::tuner() const {
    ApiLock lock(*this);
    return chain_ ? chain_->tuner : nullptr;
}

std::set<std::pair<std::uint16_t, std::uint16_t>> Engine::selection() const {
    ApiLock lock(*this);
    return selection_;
}

bool Engine::select(std::uint16_t bsid, std::uint16_t service_id, bool selected) {
    ApiLock lock(*this);
    const auto key = std::make_pair(bsid, service_id);
    if (selected) selection_.insert(key);
    else selection_.erase(key);
    bool found = false;
    if (chain_) {
        for (auto& [k, s] : chain_->services) {
            if (s->bsid != bsid || s->slt.service_id != service_id) continue;
            found = true;
            s->selected = selected;
            if (s->route) s->route->setOpenMediaSessions(sessionsWanted(*s));
            if (s->route_nrt) s->route_nrt->setOpenMediaSessions(sessionsWanted(*s));
            if (s->mmt) s->mmt->setMpuAssembly(selected);
            if (!selected) {
                s->clearMedia();
                s->clearMmt();
            }
        }
    }
    bump();
    return found || !chain_;
}

std::string Engine::categoryName(std::uint8_t c) {
    switch (c) {
        case 1: return "linear A/V";
        case 2: return "linear audio";
        case 3: return "app-based";
        case 4: return "ESG";
        case 5: return "EAS";
        case 6: return "DRM data";
        default: return "unknown";
    }
}

void Engine::onSlt(const lls::TableInfo& info, const lls::Slt& slt) {
    // called with mu_ held (tuner thread)
    if (!chain_) return;
    // Services are keyed by (bsid, serviceId): the corpus has two broadcasters sharing LLS group 0, so the
    // group alone would make their SLTs replace each other.
    const std::uint16_t bsid = slt.bsid.empty() ? 0 : slt.bsid.front();
    std::set<std::uint32_t> present;
    for (const lls::SltService& svc : slt.services) {
        const std::uint32_t key = (static_cast<std::uint32_t>(bsid) << 16) | svc.service_id;
        present.insert(key);
        auto it = chain_->services.find(key);
        if (it != chain_->services.end()) {
            Service& s = *it->second;
            const bool same_sls = s.slt.bss.has_value() == svc.bss.has_value() &&
                                  (!svc.bss || (s.slt.bss->sls_destination_ip == svc.bss->sls_destination_ip &&
                                                s.slt.bss->sls_destination_port == svc.bss->sls_destination_port &&
                                                s.slt.bss->sls_protocol == svc.bss->sls_protocol));
            s.slt = svc;
            s.bsid = bsid;
            if (same_sls) continue;
            if (s.mmt_sub) chain_->demux.unsubscribe(s.mmt_sub);
            for (const ip::SubscriptionId id : s.mmt_extra_subs) chain_->demux.unsubscribe(id);
            s.mmt_extra_subs.clear();
            s.mmt_extra_keys.clear();
            s.clearMmt();
            s.route.reset();
            s.mmt.reset();
            s.mmt_sub = 0;
            s.sls.reset();
        } else {
            auto s = std::make_unique<Service>();
            s->lls_group = info.header.group_id;
            s->bsid = bsid;
            s->slt = svc;
            it = chain_->services.emplace(key, std::move(s)).first;
        }
        Service& s = *it->second;
        s.selected = selection_.count({bsid, svc.service_id}) > 0;
        Service* raw = &s;
        if (!svc.bss) {
            s.protocol = svc.configuration == lls::SltService::Configuration::Broadband ? "broadband" : "none";
        } else if (svc.bss->sls_protocol == 1) {
            s.protocol = "ROUTE";
            sls::RouteReceiverCallbacks cb;
            cb.onSls = [this, raw](const sls::SlsBundle& b) {
                raw->sls = b;
                ++raw->sls_updates;
                for (const std::string& w : b.warnings) {
                    if (std::find(raw->warnings.begin(), raw->warnings.end(), w) == raw->warnings.end()) raw->warnings.push_back(w);
                }
                bump();
            };
            cb.onObject = [this, raw](std::shared_ptr<const route::DeliveredObject> o, const sls::SessionAddress&) { onRouteObject(*raw, std::move(o)); };
            cb.onObjectError = [this, raw](const route::ObjectError& e, const sls::SessionAddress&) {
                ++raw->object_errors;
                ++raw->channels[e.tsi].errors;
                (void)this;
            };
            cb.onError = [this, raw](const std::string& m) {
                if (raw->warnings.size() < 20) raw->warnings.push_back(m);
                bump();
            };
            sls::RouteReceiverConfig rc;
            rc.open_media_sessions = sessionsWanted(s);
            s.route = sls::RouteReceiver::fromSlt(chain_->demux, svc, std::move(cb), rc);
            if (!s.route) s.warnings.push_back("incomplete BroadcastSvcSignaling");
        } else if (svc.bss->sls_protocol == 2) {
            s.protocol = "MMTP";
            mmtp::SessionCallbacks mcb;
            mcb.onMpTable = [this, raw](const mmtp::MpTable& t) {
                ++raw->sls_updates;
                onMmtTable(*raw, t);
                bump();
            };
            mcb.onMpuInit = [this, raw](std::shared_ptr<const mmtp::MpuInitSegment> i) { onMmtInit(*raw, std::move(i)); };
            mcb.onMpuFragment = [this, raw](std::shared_ptr<const mmtp::MpuFragment> f) { onMmtFragment(*raw, std::move(f)); };
            mcb.onAtsc3Message = [this, raw](std::uint16_t, const mmtp::Atsc3Message& m) {
                const std::string name = std::string(mmtp::atsc3ContentTypeName(m.content_type)) + (m.uri.empty() ? "" : " " + m.uri);
                std::string text(m.content.begin(), m.content.end());
                const bool is_xml = !text.empty() && text.find('<') != std::string::npos && text.find('<') < 16;
                if (!is_xml) {  // binary descriptors (VSPD, CAD, ASPD, ...): hex dump
                    std::string hex = "binary " + std::to_string(m.content.size()) + " bytes\n";
                    char buf[4];
                    for (std::size_t k = 0; k < m.content.size(); ++k) {
                        std::snprintf(buf, sizeof buf, "%02x", m.content[k]);
                        hex += buf;
                        hex += (k % 32 == 31) ? '\n' : ' ';
                    }
                    text = hex;
                }
                bool replaced = false;
                for (auto& f : raw->mmt_fragments) {
                    if (f.first == name) {
                        f.second = text;
                        replaced = true;
                    }
                }
                if (!replaced) raw->mmt_fragments.emplace_back(name, text);
                if (m.content_type == 0x0001) {
                    mmtp::MmtUsbd u;
                    if (mmtp::parseMmtUsbd(text, u)) {
                        raw->mmt_usbd = std::move(u);
                        openMmtRouteComponent(*raw);
                    }
                } else if (m.content_type == 0x0003) {   // HELD: the service's broadcaster applications (spec 5.18.4)
                    sls::Held h;
                    if (sls::parseHeld(text, h)) raw->mmt_held = std::move(h);
                }
                ++raw->sls_updates;
                bump();
            };
            s.mmt = std::make_unique<mmtp::Session>(std::move(mcb));
            s.mmt->setMpuAssembly(s.selected);
            ip::FlowKey fk;
            fk.dst_ip = svc.bss->sls_destination_ip;
            fk.dst_port = svc.bss->sls_destination_port;
            fk.src_ip = svc.bss->sls_source_ip;
            mmtp::Session* ms = s.mmt.get();
            s.mmt_sub = chain_->demux.subscribe(fk, [ms](const ip::UdpDatagram& d) { ms->onDatagram(d); });
        } else {
            s.protocol = "unknown";
        }
    }
    // services dropped from this broadcaster's SLT
    for (auto it = chain_->services.begin(); it != chain_->services.end();) {
        if (it->second->bsid == bsid && !present.count(it->first)) {
            if (it->second->mmt_sub) chain_->demux.unsubscribe(it->second->mmt_sub);
            for (const ip::SubscriptionId id : it->second->mmt_extra_subs) chain_->demux.unsubscribe(id);
            it = chain_->services.erase(it);
        } else {
            ++it;
        }
    }
    bump();
}

ServiceInfo Engine::buildInfo(const Service& s) const {
    ServiceInfo i;
    // S-TSID channels (NRT, app packages, ESG, AEA media) as components; `covered` = TSIs the MPD already described
    auto addStsidChannels = [&s](ServiceInfo& info, const sls::Stsid& st, const std::set<std::uint32_t>& covered, std::uint32_t def_ip, std::uint16_t def_port) {
        for (const sls::Rs& rs : st.rs) {
            const std::string sess = io::ipv4ToString(rs.dst_ip.value_or(def_ip)) + ":" + std::to_string(rs.dst_port.value_or(def_port));
            for (const sls::Ls& ls : rs.ls) {
                if (covered.count(ls.tsi)) continue;
                ComponentInfo c;
                c.transport = "ROUTE";
                c.tsi = ls.tsi;
                c.session = sess;
                c.kind = "nrt";
                if (ls.src_flow) {
                    c.real_time = ls.src_flow->rt;
                    if (ls.src_flow->media_info) {
                        c.kind = ls.src_flow->media_info->content_type.value_or("unknown");
                        if (c.kind == "text") c.kind = "subtitles";
                        c.id = ls.src_flow->media_info->rep_id.value_or("");
                        c.lang = ls.src_flow->media_info->lang.value_or("");
                    } else if (ls.src_flow->has_aea_media) {
                        c.kind = "aea-media";
                    }
                    for (const route::PayloadRule& p : ls.src_flow->payloads) {
                        if (p.format_id == route::FormatId::UnsignedPackage || p.format_id == route::FormatId::SignedPackage) c.kind = "application";
                    }
                    if (ls.src_flow->efdt) {
                        std::string names;
                        for (const route::FdtFile& f : ls.src_flow->efdt->files) {
                            if (!names.empty()) names += ", ";
                            names += f.content_location;
                            if (f.content_type) c.mime_type = *f.content_type;
                        }
                        if (ls.src_flow->efdt->file_template) names = names.empty() ? *ls.src_flow->efdt->file_template : names + ", " + *ls.src_flow->efdt->file_template;
                        c.name = names;
                    }
                }
                const auto chs = s.channels.find(ls.tsi);
                if (chs != s.channels.end()) {
                    c.objects = chs->second.objects;
                    c.bytes = chs->second.bytes;
                    c.errors = chs->second.errors;
                    c.last_toi = chs->second.last_toi;
                    c.last_name = chs->second.last_name;
                }
                info.components.push_back(std::move(c));
            }
        }
    };
    i.lls_group = s.lls_group;
    i.bsid = s.bsid;
    i.service_id = s.slt.service_id;
    i.global_service_id = s.slt.global_service_id;
    i.major = s.slt.major_channel_no;
    i.minor = s.slt.minor_channel_no;
    i.short_name = s.slt.short_service_name.value_or("");
    i.category = s.slt.service_category;
    i.category_name = categoryName(i.category);
    i.protocol = s.protocol;
    i.protected_flag = s.slt.protected_flag;
    i.hidden = s.slt.hidden;
    i.hide_in_guide = s.slt.hide_in_guide;
    i.broadband_required = s.slt.broadband_access_required;
    i.essential = s.slt.essential.value_or(false);
    i.drm_system_id = s.slt.drm_system_id;
    if (s.slt.bss) {
        i.sls_address = io::ipv4ToString(s.slt.bss->sls_destination_ip) + ":" + std::to_string(s.slt.bss->sls_destination_port);
        if (s.slt.bss->sls_source_ip) i.sls_address = io::ipv4ToString(s.slt.bss->sls_source_ip) + "->" + i.sls_address;
    }
    i.selected = s.selected;
    i.sls_updates = s.sls_updates;
    i.objects = s.objects;
    i.object_errors = s.object_errors;
    i.bytes = s.bytes;
    i.warnings = s.warnings;
    if (s.sls) {
        const sls::SlsBundle& b = *s.sls;
        i.sls_received = true;
        i.sls_toi = b.toi;
        for (const sls::SlsFragment& f : b.fragments) i.sls_fragments.push_back(SlsFragmentInfo{sls::toString(f.kind), f.content_type, f.location, f.version, f.text.size()});
        if (b.usbd) {
            if (!b.usbd->names.empty()) i.usbd_name = b.usbd->names.front().text;
            i.languages = b.usbd->service_languages;
        }
        // components: MPD Representations joined with S-TSID channels
        std::map<std::string, std::pair<std::uint32_t, const sls::Ls*>> rep_to_ls;
        std::map<std::string, std::string> rep_session;
        if (b.stsid) {
            for (const sls::Rs& rs : b.stsid->rs) {
                const std::string sess = io::ipv4ToString(rs.dst_ip.value_or(s.slt.bss ? s.slt.bss->sls_destination_ip : 0)) + ":" +
                                         std::to_string(rs.dst_port.value_or(s.slt.bss ? s.slt.bss->sls_destination_port : 0));
                for (const sls::Ls& ls : rs.ls) {
                    if (ls.src_flow && ls.src_flow->media_info && ls.src_flow->media_info->rep_id) {
                        rep_to_ls[*ls.src_flow->media_info->rep_id] = {ls.tsi, &ls};
                        rep_session[*ls.src_flow->media_info->rep_id] = sess;
                    }
                }
            }
        }
        std::set<std::uint32_t> covered_tsi;
        if (b.mpd) {
            const sls::Mpd& m = *b.mpd;
            i.mpd_profiles = m.profiles;
            i.mpd_type = m.type;
            i.mpd_min_buffer_s = m.min_buffer_time_s;
            i.mpd_max_segment_s = m.max_segment_duration_s;
            i.mpd_time_shift_s = m.time_shift_buffer_depth_s;
            i.mpd_publish_ns = m.publish_time_ns;
            i.mpd_availability_start_ns = m.availability_start_ns;
            for (const sls::Period& p : m.periods) {
                for (const sls::AdaptationSet& a : p.adaptation_sets) {
                    for (const sls::Representation& r : a.representations) {
                        ComponentInfo c;
                        c.transport = "ROUTE";
                        c.id = r.id;
                        c.kind = kindFromMpd(a.content_type, r.mime_type, r.codecs);
                        c.codecs = r.codecs.value_or("");
                        c.mime_type = r.mime_type.value_or("");
                        c.lang = a.lang.value_or("");
                        for (const sls::Descriptor& d : a.roles) c.roles.push_back(d.value);
                        c.bandwidth = r.bandwidth;
                        c.width = r.width;
                        c.height = r.height;
                        c.frame_rate = r.frame_rate;
                        c.audio_sampling_rate = r.audio_sampling_rate;
                        c.timescale = r.segment_template.timescale;
                        c.content_protected = !a.content_protection.empty() || !r.content_protection.empty();
                        const auto ls = rep_to_ls.find(r.id);
                        if (ls != rep_to_ls.end()) {
                            c.tsi = ls->second.first;
                            c.real_time = ls->second.second->src_flow && ls->second.second->src_flow->rt;
                            c.session = rep_session[r.id];
                            covered_tsi.insert(ls->second.first);
                            if (c.lang.empty() && ls->second.second->src_flow->media_info->lang) c.lang = *ls->second.second->src_flow->media_info->lang;
                            const auto st = s.channels.find(ls->second.first);
                            if (st != s.channels.end()) {
                                c.objects = st->second.objects;
                                c.bytes = st->second.bytes;
                                c.errors = st->second.errors;
                                c.last_toi = st->second.last_toi;
                                c.last_name = st->second.last_name;
                            }
                        }
                        i.components.push_back(std::move(c));
                    }
                }
            }
        }
        // S-TSID channels not described by the MPD (NRT, app packages, ESG, AEA media)
        if (b.stsid) addStsidChannels(i, *b.stsid, covered_tsi, s.slt.bss ? s.slt.bss->sls_destination_ip : 0, s.slt.bss ? s.slt.bss->sls_destination_port : 0);
    }
    if (s.mmt) {
        i.sls_received = s.mmt->mpTable() != nullptr || !s.mmt_fragments.empty();
        i.mmtp_packets = s.mmt->stats().packets;
        for (const auto& f : s.mmt_fragments) i.sls_fragments.push_back(SlsFragmentInfo{"mmt", f.first, "", std::nullopt, f.second.size()});
        if (s.mmt_usbd) {
            if (!s.mmt_usbd->names.empty()) i.usbd_name = s.mmt_usbd->names.front().second;
            i.languages = s.mmt_usbd->service_languages;
            if (s.mmt_usbd->mmt_package_id) i.mmt_package_id = *s.mmt_usbd->mmt_package_id;
        }
        const auto pids = s.mmt->packetIds();
        std::set<std::uint16_t> covered;
        if (const mmtp::MpTable* t = s.mmt->mpTable()) {
            if (!t->package_id.empty()) i.mmt_package_id = t->package_id;
            for (const mmtp::Asset& a : t->assets) {
                ComponentInfo c;
                c.transport = "MMTP";
                c.id = a.assetIdText();
                c.codecs = a.asset_type;
                c.default_asset = a.default_asset;
                c.timescale = a.timescale;
                c.packet_id = a.packetId();
                c.session = i.sls_address;
                bool have_type = false;
                std::uint8_t utype = 0;
                if (s.mmt_usbd) {
                    for (const mmtp::MmtComponentInfo& ci : s.mmt_usbd->components) {
                        if (ci.component_id == c.id) {
                            have_type = true;
                            utype = ci.component_type;
                            c.name = ci.component_name.value_or("");
                            c.content_protected = ci.protected_flag;
                        }
                    }
                }
                c.kind = kindFromCodec(a.asset_type, utype, have_type);
                c.real_time = true;
                if (c.packet_id) {
                    const auto mc = s.mmt_comps.find(*c.packet_id);
                    if (mc != s.mmt_comps.end() && mc->second.init) {   // the sample entry is authoritative (the MP table's asset_type lies on air)
                        const media::TrackInfo& mt = mc->second.init->media;
                        if (!mt.codecs.empty()) c.codecs = mt.codecs;
                        c.kind = mt.kind();
                        c.mime_type = mt.containerMime();
                        c.width = mt.width ? std::optional<std::uint32_t>(*mt.width) : std::nullopt;
                        c.height = mt.height ? std::optional<std::uint32_t>(*mt.height) : std::nullopt;
                        c.audio_sampling_rate = mt.sample_rate;
                        c.timescale = mt.timescale;
                        if (c.lang.empty() && mt.language != "und") c.lang = mt.language;
                        c.last_name = "mpu " + std::to_string(mc->second.frags.empty() ? 0u : mc->second.frags.back().second->mpu_sequence_number);
                    }
                }
                if (c.packet_id) {
                    covered.insert(*c.packet_id);
                    for (const mmtp::PacketIdStats& ps : pids) {
                        if (ps.packet_id == *c.packet_id) {
                            c.packets = ps.packets;
                            c.bytes = ps.bytes;
                            c.errors = ps.sequence_gaps;
                            c.objects = ps.mpus;
                            c.last_mpu_sequence = ps.last_mpu_sequence;
                        }
                    }
                }
                i.components.push_back(std::move(c));
            }
        }
        for (const mmtp::PacketIdStats& ps : pids) {
            if (covered.count(ps.packet_id)) continue;
            ComponentInfo c;
            c.transport = "MMTP";
            c.packet_id = ps.packet_id;
            c.kind = ps.signaling_packets > 0 && ps.mpu_packets == 0 ? "signaling" : "unknown";
            c.session = i.sls_address;
            c.packets = ps.packets;
            c.bytes = ps.bytes;
            c.errors = ps.sequence_gaps;
            c.objects = ps.mpus;
            i.components.push_back(std::move(c));
        }
        // the ROUTE session of the USBD ROUTEComponent: its S-TSID channels (app packages, NRT files)
        for (const auto& f : s.route_nrt_fragments) i.sls_fragments.push_back(SlsFragmentInfo{"route", f.first, "", std::nullopt, f.second.size()});
        if (s.route_nrt_stsid && s.mmt_usbd && s.mmt_usbd->route_component) {
            const std::set<std::uint32_t> none;
            addStsidChannels(i, *s.route_nrt_stsid, none, s.mmt_usbd->route_component->dst_ip, s.mmt_usbd->route_component->dst_port);
        }
    }
    i.apps = buildApps(s);
    return i;
}

std::vector<AppInfo> Engine::buildApps(const Service& s) const {
    // called with mu_ held. One entry per HELD HTMLEntryPackage; readiness and size from the Application Context Cache.
    const sls::Held* held = nullptr;
    if (s.sls && s.sls->held) held = &*s.sls->held;
    else if (s.mmt_held) held = &*s.mmt_held;
    std::vector<AppInfo> out;
    if (!held) return out;
    for (const sls::HtmlEntryPackage& p : held->packages) {
        AppInfo a;
        a.app_context_id = p.app_context_id;
        a.app_id = p.app_id.value_or("");
        a.bcast_entry_package_url = p.bcast_entry_package_url;
        a.bcast_entry_page_url = p.bcast_entry_page_url;
        a.bband_entry_page_url = p.bband_entry_page_url;
        a.required_capabilities = p.required_capabilities;
        a.app_rendering = p.app_rendering && (*p.app_rendering == "true" || *p.app_rendering == "1");
        a.coupled_services = p.coupled_services;
        a.lct_tsi_ref = p.lct_tsi_ref;
        a.valid_from_ns = p.valid_from_ns;
        a.valid_until_ns = p.valid_until_ns;
        for (const auto& [path, f] : s.app_files) {
            const auto& ids = f.info.app_context_ids;
            if (ids.empty() || std::find(ids.begin(), ids.end(), a.app_context_id) != ids.end()) {
                ++a.files;
                a.bytes += f.size;
            }
        }
        if (p.bcast_entry_page_url) a.ready = s.app_files.count(appPath(*p.bcast_entry_page_url)) > 0;
        // progress of the entry package: its EFDT entry names the channel and size, the session the bytes held so far
        if (p.bcast_entry_package_url) {
            const std::string pkg = appPath(*p.bcast_entry_package_url);
            const sls::Stsid* st = s.sls && s.sls->stsid ? &*s.sls->stsid : (s.route_nrt_stsid ? &*s.route_nrt_stsid : nullptr);
            const sls::RouteReceiver* rr = s.route ? s.route.get() : s.route_nrt.get();
            std::optional<std::uint32_t> toi;
            if (st) {
                for (const sls::Rs& rs : st->rs) {
                    for (const sls::Ls& ls : rs.ls) {
                        if (!ls.src_flow || !ls.src_flow->efdt) continue;
                        for (const route::FdtFile& f : ls.src_flow->efdt->files) {
                            if (appPath(f.content_location) != pkg) continue;
                            a.package_tsi = ls.tsi;
                            toi = f.toi;
                            a.package_bytes = f.content_length;
                        }
                    }
                }
            }
            if (rr && a.package_tsi && toi) {
                for (const sls::SessionAddress& addr : rr->sessions()) {
                    const route::Session* ss = rr->session(addr);
                    if (!ss) continue;
                    for (const route::Session::PendingObject& o : ss->pending()) {
                        if (o.tsi == *a.package_tsi && o.toi == *toi) {
                            a.package_received = o.received;
                            if (!a.package_bytes && o.expected) a.package_bytes = o.expected;
                        }
                    }
                }
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::optional<AppIndex> Engine::apps(std::uint16_t bsid, std::uint16_t service_id) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return std::nullopt;
    AppIndex idx;
    idx.bsid = bsid;
    idx.service_id = service_id;
    idx.selected = s->selected;
    idx.apps = buildApps(*s);
    for (const auto& [path, f] : s->app_files) idx.files.push_back(f.info);
    idx.cached_bytes = s->app_bytes;
    idx.cache_limit = chain_ ? chain_->opts.app_cache_bytes : 0;
    idx.objects = s->app_objects;
    idx.packages = s->app_packages;
    idx.rejected = s->app_rejected;
    return idx;
}

std::optional<AppFile> Engine::appFile(std::uint16_t bsid, std::uint16_t service_id, const std::string& path) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return std::nullopt;
    const auto it = s->app_files.find(appPath(path));
    if (it == s->app_files.end()) return std::nullopt;
    AppFile f;
    f.content_type = it->second.info.content_type;
    f.owner = it->second.owner;
    f.offset = it->second.offset;
    f.size = it->second.size;
    return f;
}

void Engine::onRouteObject(Service& s, std::shared_ptr<const route::DeliveredObject> o) {
    // called with mu_ held (tuner thread): every delivery object of the service's ROUTE sessions other than the SLS
    ++s.objects;
    s.bytes += o->data.size();
    auto& ch = s.channels[o->tsi];
    ++ch.objects;
    ch.bytes += o->data.size();
    ch.last_toi = o->toi;
    ch.last_name = o->content_location;
    std::string line = "tsi " + std::to_string(o->tsi) + " toi " + std::to_string(o->toi) + " " + o->content_location.value_or("-") +
                       " " + std::to_string(o->data.size()) + " B";
    s.recent.push_back(std::move(line));
    if (s.recent.size() > 30) s.recent.erase(s.recent.begin());
    const std::string mime = lowerMime(o->content_type.value_or(""));
    const bool package = o->format == route::FormatId::UnsignedPackage || o->format == route::FormatId::SignedPackage || mime.rfind("multipart/", 0) == 0;
    if (s.slt.service_category == 4 || mime == kSgddMime || mime == kSgduMime) onEsgObject(s, *o);
    else if (s.selected && (o->rep_id || o->media_content_type)) onMediaObject(s, o);
    else if (s.selected && (package || !o->app_context_id_list.empty())) onAppObject(s, std::move(o));
    else if ((s.objects % 8) == 1) bump();
}

void Engine::onAppObject(Service& s, std::shared_ptr<const route::DeliveredObject> o) {
    // called with mu_ held (tuner thread). NRT files tagged with an appContextIdList and app packages (A/331 A.3.3.4
    // package mode: multipart/related with a metadataEnvelope) go to the service's Application Context Cache, packages
    // unpacked into their parts, keyed by Content-Location; a newer object replaces the files it carries (spec 5.18.4).
    ++s.app_objects;
    const std::string name = appPath(o->content_location.value_or(""));
    if (name.empty()) {
        ++s.app_rejected;
        return;
    }
    const std::size_t limit = chain_ ? chain_->opts.app_cache_bytes : (64u << 20);
    const std::string mime = lowerMime(o->content_type.value_or(""));
    const bool package = o->format == route::FormatId::UnsignedPackage || o->format == route::FormatId::SignedPackage || mime.rfind("multipart/", 0) == 0;
    auto put = [&](const std::string& path, Service::CachedAppFile f) {
        const auto it = s.app_files.find(path);
        if (it != s.app_files.end()) {
            s.app_bytes -= it->second.size;
            s.app_files.erase(it);
        }
        s.app_bytes += f.size;
        s.app_files[path] = std::move(f);
    };
    auto fill = [&](Service::CachedAppFile& f, const std::string& path, std::size_t offset, std::size_t size) {
        f.info.path = path;
        f.info.bytes = size;
        f.info.app_context_ids = o->app_context_id_list;
        f.info.tsi = o->tsi;
        f.info.toi = o->toi;
        f.info.received_ns = o->last_ns;
        f.owner = o;
        f.offset = offset;
        f.size = size;
    };
    // bytes the files of an older version of this object still hold
    std::size_t held_by_same = 0;
    for (const auto& [path, f] : s.app_files)
        if (f.info.package == name || (f.info.package.empty() && path == name)) held_by_same += f.size;
    if (!package) {
        if (s.app_bytes - held_by_same + o->data.size() > limit) {
            ++s.app_rejected;
            return;
        }
        Service::CachedAppFile f;
        fill(f, name, 0, o->data.size());
        f.info.content_type = canonicalMime(mime.empty() ? mimeFromName(name) : mime);
        put(name, std::move(f));
        bump();
        return;
    }
    route::Multipart mp;
    if (!route::parseMultipart(ByteView(o->data.data(), o->data.size()), mp) || mp.parts.empty()) {
        ++s.app_rejected;
        return;
    }
    std::size_t total = 0;
    for (const route::MimePart& p : mp.parts) total += p.body.size();
    if (s.app_bytes - held_by_same + total > limit) {
        ++s.app_rejected;
        return;
    }
    // the envelope lists every part with its media type and version
    std::map<std::string, const sls::EnvelopeItem*> items;
    sls::Envelope env;
    for (const route::MimePart& p : mp.parts) {
        if (p.headers.contentType() == "application/mbms-envelope+xml") {
            if (sls::parseEnvelope(std::string(p.body.begin(), p.body.end()), env)) {
                for (const sls::EnvelopeItem& it : env.items) items[appPath(it.metadata_uri)] = &it;
            }
            break;
        }
    }
    // drop the previous version of the package first (parts it no longer carries must not linger)
    for (auto it = s.app_files.begin(); it != s.app_files.end();) {
        if (it->second.info.package == name) {
            s.app_bytes -= it->second.size;
            it = s.app_files.erase(it);
        } else {
            ++it;
        }
    }
    ++s.app_packages;
    std::size_t n = 0;
    for (const route::MimePart& p : mp.parts) {
        std::string path = appPath(p.headers.contentLocation().value_or(""));
        if (path.empty()) path = name + "/part-" + std::to_string(n);
        Service::CachedAppFile f;
        fill(f, path, static_cast<std::size_t>(p.body.data() - o->data.data()), p.body.size());
        f.info.package = name;
        const auto item = items.find(path);
        f.info.content_type = p.headers.contentType();
        if (item != items.end()) {
            f.info.version = item->second->version;
            if (f.info.content_type.empty()) f.info.content_type = item->second->content_type;
        }
        if (f.info.content_type.empty()) f.info.content_type = mimeFromName(path);
        f.info.content_type = canonicalMime(f.info.content_type);
        put(path, std::move(f));
        ++n;
    }
    bump();
}

void Engine::openMmtRouteComponent(Service& s) {
    // called with mu_ held. The USBD of an MMT service may name a ROUTE session that carries its locally-cached content
    // (WNUV: the Sinclair app packages on 239.255.54.1:15007); its SLS bundle holds an S-TSID only. Opened like a ROUTE
    // service's sessions: SLS always, the NRT channels while the service is selected.
    if (!chain_ || !s.mmt_usbd || !s.mmt_usbd->route_component) return;
    const mmtp::MmtRouteComponent& rc = *s.mmt_usbd->route_component;
    sls::SessionAddress addr;
    addr.dst_ip = rc.dst_ip;
    addr.dst_port = rc.dst_port;
    addr.src_ip = rc.src_ip;
    const std::string key = addr.toString();
    if (s.route_nrt && s.route_nrt_address == key) return;
    Service* raw = &s;
    sls::RouteReceiverCallbacks cb;
    cb.onSls = [this, raw](const sls::SlsBundle& b) {
        ++raw->sls_updates;
        if (b.stsid) raw->route_nrt_stsid = *b.stsid;
        if (b.held) raw->mmt_held = *b.held;
        raw->route_nrt_fragments.clear();
        for (const sls::SlsFragment& f : b.fragments)
            raw->route_nrt_fragments.emplace_back((f.location.empty() ? std::string(sls::toString(f.kind)) : f.location) + " (ROUTEComponent)", f.text);
        bump();
    };
    cb.onObject = [this, raw](std::shared_ptr<const route::DeliveredObject> o, const sls::SessionAddress&) { onRouteObject(*raw, std::move(o)); };
    cb.onObjectError = [this, raw](const route::ObjectError& e, const sls::SessionAddress&) {
        ++raw->object_errors;
        ++raw->channels[e.tsi].errors;
        (void)this;
    };
    cb.onError = [this, raw](const std::string& m) {
        if (raw->warnings.size() < 20) raw->warnings.push_back("ROUTEComponent: " + m);
        bump();
    };
    sls::RouteReceiverConfig cfg;
    cfg.open_media_sessions = sessionsWanted(s);
    s.route_nrt = std::make_unique<sls::RouteReceiver>(chain_->demux, addr, std::move(cb), cfg);
    s.route_nrt_address = key;
}

Status Engine::status() const {
    ApiLock lock(*this);
    Status st = status_;
    if (chain_) {
        if (chain_->tuner) {
            const TunerState ts = chain_->tuner->state();
            st.position_ns = ts.position_ns;
            st.duration_ns = ts.duration_ns;
            st.paused = ts.paused;
            st.finished = ts.finished;
            st.signal = ts.signal;
        }
        st.misrouted_units = chain_->misrouted;
        if (chain_->deframer) st.deframer = chain_->deframer->stats();
        if (chain_->decap) st.decap = chain_->decap->stats();
        st.demux = chain_->demux.stats();
        st.lls = chain_->lls->stats();
        st.link = chain_->link->stats();
        st.lls_groups = chain_->lls->groups();
        std::set<std::uint16_t> bsids;
        for (const auto& [k, s] : chain_->services) bsids.insert(s->bsid);
        st.bsids.assign(bsids.begin(), bsids.end());
        st.services = chain_->services.size();
        st.capture_unix_ns = chain_->time.capture_ns;
        st.selected = 0;
        for (const auto& [k, s] : chain_->services) if (s->selected) ++st.selected;
    }
    return st;
}

std::vector<ServiceInfo> Engine::services() const {
    ApiLock lock(*this);
    std::vector<ServiceInfo> v;
    if (!chain_) return v;
    for (const auto& [k, s] : chain_->services) v.push_back(buildInfo(*s));
    std::sort(v.begin(), v.end(), [](const ServiceInfo& a, const ServiceInfo& b) {
        return std::tie(a.bsid, a.major, a.minor, a.service_id) < std::tie(b.bsid, b.major, b.minor, b.service_id);
    });
    return v;
}

std::optional<ServiceDetail> Engine::service(std::uint16_t bsid, std::uint16_t service_id) const {
    ApiLock lock(*this);
    if (!chain_) return std::nullopt;
    for (const auto& [k, s] : chain_->services) {
        if (s->bsid != bsid || s->slt.service_id != service_id) continue;
        ServiceDetail d;
        d.info = buildInfo(*s);
        if (s->sls) {
            for (const sls::SlsFragment& f : s->sls->fragments) d.fragments.emplace_back(f.location.empty() ? std::string(sls::toString(f.kind)) : f.location, f.text);
        }
        for (const auto& f : s->mmt_fragments) d.fragments.push_back(f);
        for (const auto& f : s->route_nrt_fragments) d.fragments.push_back(f);
        d.fragments.emplace_back("SLT entry", s->slt.raw_xml);
        d.recent_objects = s->recent;
        return d;
    }
    return std::nullopt;
}

std::vector<LlsTableInfo> Engine::llsTables() const {
    ApiLock lock(*this);
    std::vector<LlsTableInfo> v;
    if (!chain_) return v;
    for (const lls::TableRecord* r : chain_->lls->tables()) {
        LlsTableInfo t;
        t.group = r->info.header.group_id;
        t.table_id = r->info.header.table_id;
        t.version = r->info.header.version;
        t.name = lls::tableName(t.table_id);
        t.source_ip = io::ipv4ToString(r->info.source_ip);
        t.root_namespace = r->info.root_namespace;
        t.xml = r->info.raw_xml;
        t.bytes = r->payload.size();
        t.t_ns = r->info.t_ns;
        v.push_back(std::move(t));
    }
    return v;
}

LinkInfo Engine::link() const {
    ApiLock lock(*this);
    LinkInfo l;
    if (!chain_) return l;
    for (std::uint8_t plp = 0; plp < 64; ++plp) {
        if (const auto lmt = chain_->link->lmt(plp)) l.lmts.emplace_back(plp, *lmt);
    }
    l.plp_map = chain_->link->plpMap().entries();
    if (chain_->tuner) l.l1 = chain_->tuner->l1();
    l.unrouted = chain_->demux.unroutedFlows();
    return l;
}

bool Engine::sessionsWanted(const Service& s) const {
    // called with mu_ held
    if (s.selected) return true;
    return chain_ && chain_->opts.auto_open_esg && s.slt.service_category == 4;
}

void Engine::onEsgObject(Service& s, const route::DeliveredObject& o) {
    // called with mu_ held (tuner thread). ESG objects: SGDD / SGDU by Content-Type (EFDT), else by name or
    // content; anything else on an ESG service is kept as a file (icons referenced from the fragments).
    EsgStore& st = chain_->esg;
    ++st.stats.objects;
    const std::string name = o.content_location.value_or("");
    const std::string mime = lowerMime(o.content_type.value_or(""));
    ByteView data(o.data.data(), o.data.size());
    std::vector<std::uint8_t> inflated;
    if (util::isGzip(data)) {   // declared Content-Encoding is already undone by the ROUTE layer; this catches undeclared gzip
        if (auto g = util::gunzip(data)) {
            inflated = std::move(*g);
            data = ByteView(inflated.data(), inflated.size());
        }
    }
    auto startsWithXml = [&](const char* root) {
        if (data.empty() || data[0] != '<') return false;
        const std::string head(reinterpret_cast<const char*>(data.data()), std::min<std::size_t>(data.size(), 512));
        return head.find(root) != std::string::npos;
    };
    enum class Kind { Sgdd, Sgdu, File, Other } kind = Kind::Other;
    if (mime == kSgddMime) kind = Kind::Sgdd;
    else if (mime == kSgduMime) kind = Kind::Sgdu;
    else if (startsWithXml("ServiceGuideDeliveryDescriptor")) kind = Kind::Sgdd;
    else if (mime.rfind("image/", 0) == 0 || endsWithNoCase(name, ".png") || endsWithNoCase(name, ".jpg") || endsWithNoCase(name, ".jpeg") ||
             endsWithNoCase(name, ".gif") || endsWithNoCase(name, ".svg") || endsWithNoCase(name, ".webp"))
        kind = Kind::File;
    else if (name.rfind("sgdu", 0) == 0 || name.rfind("SGDU", 0) == 0) kind = Kind::Sgdu;
    else if (!data.empty() && data[0] != '<' && data.size() >= 9) kind = Kind::Sgdu;   // binary on an ESG service: try the unit header

    bool changed = false;
    if (kind == Kind::Sgdd) {
        Sgdd d;
        std::string err;
        if (!parseSgdd(data, d, &err)) {
            ++st.stats.parse_errors;
            if (s.warnings.size() < 20) s.warnings.push_back("SGDD " + name + ": " + err);
        } else {
            ++st.stats.sgdd;
            EsgDescriptorInfo info;
            info.sgdd = std::move(d);
            info.bsid = s.bsid;
            info.service_id = s.slt.service_id;
            info.tsi = o.tsi;
            info.toi = o.toi;
            info.location = name;
            info.received_ns = o.last_ns;
            bool replaced = false;
            for (EsgDescriptorInfo& e : st.descriptors) {
                if (e.bsid == info.bsid && e.service_id == info.service_id && e.location == info.location) {
                    if (e.sgdd.version != info.sgdd.version || e.sgdd.id != info.sgdd.id || e.toi != info.toi) changed = true;
                    e = info;
                    replaced = true;
                }
            }
            if (!replaced) {
                st.descriptors.push_back(std::move(info));
                changed = true;
            }
        }
    } else if (kind == Kind::Sgdu) {
        Sgdu u;
        std::string err;
        if (!parseSgdu(data, u, &err)) {
            ++st.stats.parse_errors;
            if (s.warnings.size() < 20) s.warnings.push_back("SGDU " + name + ": " + err);
        } else {
            ++st.stats.sgdu;
            for (const SgduFragment& sf : u.fragments) {
                if (sf.xml.empty()) continue;
                ++st.stats.fragments_seen;
                EsgFragment f;
                std::string ferr;
                if (!parseEsgFragmentHeader(sf.xml, f, &ferr)) {
                    ++st.stats.parse_errors;
                    continue;
                }
                if (f.type == EsgFragmentType::Unknown) f.type = sf.type;
                if (f.id.empty()) f.id = name + "#" + std::to_string(sf.transport_id);
                if (f.version == 0) f.version = sf.version;
                f.xml = sf.xml;
                f.bsid = s.bsid;
                f.service_id = s.slt.service_id;
                f.toi = o.toi;
                f.transport_id = sf.transport_id;
                f.received_ns = o.last_ns;
                const std::string key = std::string(toString(f.type)) + "|" + f.id;
                auto it = st.fragments.find(key);
                if (it == st.fragments.end()) {
                    st.fragments.emplace(key, std::move(f));
                    ++st.stats.fragments_new;
                    changed = true;
                } else if (it->second.version == f.version && it->second.xml == f.xml) {
                    ++st.stats.fragments_duplicate;
                    it->second.received_ns = f.received_ns;
                } else if (it->second.version <= f.version) {
                    it->second = std::move(f);
                    ++st.stats.fragments_updated;
                    changed = true;
                } else {
                    ++st.stats.fragments_duplicate;   // older version repeated on the carousel
                }
            }
            for (const std::string& w : u.warnings) {
                if (s.warnings.size() < 20 && std::find(s.warnings.begin(), s.warnings.end(), w) == s.warnings.end()) s.warnings.push_back(w);
            }
        }
    } else if (kind == Kind::File && !name.empty()) {
        EsgFileInfo info;
        info.name = name;
        info.content_type = mime.empty() ? (endsWithNoCase(name, ".png") ? "image/png" : endsWithNoCase(name, ".svg") ? "image/svg+xml" : endsWithNoCase(name, ".gif") ? "image/gif" : "image/jpeg") : mime;
        info.bytes = data.size();
        info.bsid = s.bsid;
        info.service_id = s.slt.service_id;
        info.tsi = o.tsi;
        info.toi = o.toi;
        info.received_ns = o.last_ns;
        auto it = st.files.find(name);
        if (it == st.files.end() || it->second.first.bytes != info.bytes || it->second.first.toi != info.toi) {
            if (it != st.files.end()) st.stats.file_bytes -= it->second.first.bytes;
            while (st.stats.file_bytes + info.bytes > kEsgFileCacheBytes && !st.files.empty()) {
                auto oldest = st.files.begin();
                for (auto f = st.files.begin(); f != st.files.end(); ++f) {
                    if (f->second.first.received_ns < oldest->second.first.received_ns) oldest = f;
                }
                st.stats.file_bytes -= oldest->second.first.bytes;
                st.files.erase(oldest);
            }
            EsgFile file;
            file.content_type = info.content_type;
            file.bytes.assign(data.data(), data.data() + data.size());
            st.stats.file_bytes += info.bytes;
            st.files[name] = std::make_pair(std::move(info), std::move(file));
            ++st.stats.files;
            changed = true;
        } else {
            it->second.first.received_ns = info.received_ns;
        }
    } else {
        ++st.stats.ignored;
    }
    if (changed) {
        ++status_.esg_version;
        bump();
    }
}

EsgSnapshot Engine::esg() const {
    ApiLock lock(*this);
    EsgSnapshot snap;
    snap.version = status_.esg_version;
    if (!chain_) return snap;
    const EsgStore& st = chain_->esg;
    snap.stats = st.stats;
    snap.fragments.reserve(st.fragments.size());
    for (const auto& [k, f] : st.fragments) snap.fragments.push_back(f);
    snap.descriptors = st.descriptors;
    for (const auto& [k, f] : st.files) snap.files.push_back(f.first);
    return snap;
}

std::uint64_t Engine::esgVersion() const {
    ApiLock lock(*this);
    return status_.esg_version;
}

std::optional<EsgFile> Engine::esgFile(const std::string& name) const {
    ApiLock lock(*this);
    if (!chain_) return std::nullopt;
    const auto it = chain_->esg.files.find(name);
    if (it == chain_->esg.files.end()) return std::nullopt;
    return it->second.second;
}

// ---- media segment store ----

namespace {
std::string segmentContentType(const std::string& kind, const std::string& name, const std::optional<std::string>& mime) {
    if (mime && (mime->rfind("video/", 0) == 0 || mime->rfind("audio/", 0) == 0 || mime->rfind("application/", 0) == 0)) return *mime;
    if (kind == "video") return "video/mp4";
    if (kind == "audio") return "audio/mp4";
    if (kind == "subtitles" || kind == "text") return "application/mp4";
    if (endsWithNoCase(name, ".mp4v") || endsWithNoCase(name, ".m4pv") || endsWithNoCase(name, ".m4v")) return "video/mp4";
    if (endsWithNoCase(name, ".mp4a") || endsWithNoCase(name, ".m4pa") || endsWithNoCase(name, ".m4a")) return "audio/mp4";
    return "application/mp4";
}
std::string repKind(const sls::Mpd& mpd, const std::string& rep_id) {
    for (const sls::Period& p : mpd.periods)
        for (const sls::AdaptationSet& a : p.adaptation_sets)
            for (const sls::Representation& r : a.representations)
                if (r.id == rep_id) return kindFromMpd(a.content_type, r.mime_type, r.codecs);
    return "unknown";
}
}  // namespace

void Engine::onMediaObject(Service& s, std::shared_ptr<const route::DeliveredObject> o) {
    // called with mu_ held (tuner thread)
    if (!s.sls || !s.sls->mpd || !o->content_location) return;
    const std::string name = *o->content_location;
    const auto m = sls::matchSegment(*s.sls->mpd, name, o->rep_id, o->toi);
    if (!m) return;
    Service::CachedSegment c;
    c.info.seq = ++s.media_seq;
    c.info.name = name;
    c.info.rep_id = m->rep_id;
    c.info.key = m->key;
    // init segments start with ftyp; an object the templates did not place but that is an ftyp still counts as init
    const bool ftyp = o->data.size() >= 8 && std::memcmp(o->data.data() + 4, "ftyp", 4) == 0;
    c.info.is_init = m->is_init || (ftyp && m->key == o->toi);
    c.info.bytes = o->data.size();
    c.info.tsi = o->tsi;
    c.info.toi = o->toi;
    c.info.received_ns = o->last_ns;
    c.info.content_type = segmentContentType(repKind(*s.sls->mpd, m->rep_id), name, o->content_type);
    // the media timeline, for the $Number$ calibration of media(): the init's track timescale and each segment's tfdt
    const ByteView bytes(o->data.data(), o->data.size());
    if (c.info.is_init) {
        media::InitSegmentInfo ii;
        if (media::parseInitSegment(bytes, ii)) {
            for (const media::TrackInfo& t : ii.tracks) {
                if (t.isHint() || t.timescale == 0) continue;
                c.info.track_timescale = t.timescale;
                break;
            }
        }
    } else {
        media::MoofInfo mi;
        if (media::parseMoof(bytes, mi) && !mi.trafs.empty()) c.info.tfdt = mi.trafs.front().tfdt;
    }
    c.obj = std::move(o);
    // replace an existing entry with the same name (carousel / repeated init)
    for (auto it = s.media.begin(); it != s.media.end(); ++it) {
        if (it->info.name == name) {
            if (!it->info.is_init) s.media_bytes -= it->info.bytes;
            s.media.erase(it);
            break;
        }
    }
    if (!c.info.is_init) {
        s.media_bytes += c.info.bytes;
        const std::size_t limit = chain_ ? chain_->opts.media_cache_bytes : (96u << 20);
        for (auto it = s.media.begin(); it != s.media.end() && s.media_bytes > limit;) {
            if (it->info.is_init) {
                ++it;
                continue;
            }
            s.media_bytes -= it->info.bytes;
            it = s.media.erase(it);
            ++s.media_evicted;
        }
    }
    s.media.push_back(std::move(c));
    if ((s.media_seq % 4) == 1) bump();
}

const Engine::Service* Engine::findService(std::uint16_t bsid, std::uint16_t service_id) const {
    // called with mu_ held
    if (!chain_) return nullptr;
    const auto it = chain_->services.find((static_cast<std::uint32_t>(bsid) << 16) | service_id);
    return it == chain_->services.end() ? nullptr : it->second.get();
}

std::optional<MediaIndex> Engine::media(std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return std::nullopt;
    return buildMediaIndex(*s, bsid, service_id, since);
}

MediaIndex Engine::buildMediaIndex(const Service& s, std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since) const {
    // called with mu_ held
    MediaIndex idx;
    idx.bsid = bsid;
    idx.service_id = service_id;
    idx.selected = s.selected;
    idx.next_seq = s.media_seq;
    idx.cached_bytes = s.media_bytes;
    idx.cache_limit = chain_ ? chain_->opts.media_cache_bytes : 0;
    idx.evicted = s.media_evicted;
    if (s.sls && s.sls->mpd) {
        const sls::Mpd& mpd = *s.sls->mpd;
        idx.mpd_type = mpd.type;
        idx.max_segment_duration_s = mpd.max_segment_duration_s;
        idx.time_shift_buffer_depth_s = mpd.time_shift_buffer_depth_s;
        idx.availability_start_ns = mpd.availability_start_ns;
        if (!mpd.periods.empty()) idx.period_start_s = mpd.periods.front().start_s.value_or(0.0);
        for (const sls::Period& p : mpd.periods) {
            for (const sls::AdaptationSet& a : p.adaptation_sets) {
                for (const sls::Representation& r : a.representations) {
                    MediaRepresentationInfo ri;
                    ri.id = r.id;
                    ri.kind = kindFromMpd(a.content_type, r.mime_type, r.codecs);
                    ri.codecs = r.codecs.value_or(a.codecs.value_or(""));
                    ri.mime_type = r.mime_type.value_or(a.mime_type.value_or(""));
                    ri.lang = a.lang.value_or("");
                    ri.bandwidth = r.bandwidth;
                    ri.width = r.width;
                    ri.height = r.height;
                    ri.timescale = r.segment_template.timescale;
                    ri.segment_duration = r.segment_template.duration;
                    ri.start_number = r.segment_template.start_number;
                    switch (sls::addressingOf(r.segment_template)) {
                        case sls::Addressing::Number: ri.addressing = "number"; break;
                        case sls::Addressing::Time: ri.addressing = "time"; break;
                        default: ri.addressing = "none"; break;
                    }
                    if (r.segment_template.initialization) ri.init_name = sls::resolveTemplate(*r.segment_template.initialization, r.id, 0, 0, r.bandwidth.value_or(0));
                    ri.media_template = r.segment_template.media.value_or("");
                    ri.content_protected = !a.content_protection.empty() || !r.content_protection.empty();
                    std::optional<std::uint64_t> newest_tfdt;
                    std::optional<std::uint32_t> track_timescale;
                    for (const Service::CachedSegment& c : s.media) {
                        if (c.info.rep_id != r.id) continue;
                        if (c.info.is_init) {
                            ri.init = true;
                            if (c.info.track_timescale) track_timescale = c.info.track_timescale;
                            continue;
                        }
                        ++ri.cached_segments;
                        if (!ri.first_key || c.info.key < *ri.first_key) ri.first_key = c.info.key;
                        if (!ri.last_key || c.info.key > *ri.last_key) {
                            ri.last_key = c.info.key;
                            newest_tfdt = c.info.tfdt;
                        }
                    }
                    calibrateNumbering(ri, r.segment_template.presentation_time_offset, newest_tfdt, track_timescale);
                    idx.representations.push_back(std::move(ri));
                }
            }
        }
    }
    for (const Service::CachedSegment& c : s.media)
        if (c.info.seq > since) idx.segments.push_back(c.info);
    // Live edge: the end of the availability window of the newest cached segment of any representation.
    // Broadcast transmits segments ahead of their nominal availability (about 7 s on the UniSoft feed), and a
    // receiver that just tuned in holds nothing older, so the player's clock has to follow what was received.
    if (idx.availability_start_ns) {
        std::optional<double> edge_s;
        for (const MediaRepresentationInfo& r : idx.representations) {
            if (!r.last_key || !r.timescale || *r.timescale == 0) continue;
            const double ts = static_cast<double>(*r.timescale);
            double end_s = 0.0;
            if (r.addressing == "number") {
                if (!r.segment_duration) continue;
                const std::uint64_t n = *r.last_key >= r.effective_start_number ? *r.last_key - r.effective_start_number + 1 : 1;
                end_s = idx.period_start_s + static_cast<double>(n) * static_cast<double>(*r.segment_duration) / ts;
            } else if (r.addressing == "time") {
                const double d = r.segment_duration ? static_cast<double>(*r.segment_duration) / ts : idx.max_segment_duration_s.value_or(2.0);
                end_s = idx.period_start_s + static_cast<double>(*r.last_key) / ts + d;
            } else {
                continue;
            }
            if (!edge_s || end_s > *edge_s) edge_s = end_s;
        }
        if (edge_s) idx.live_edge_unix_ms = *idx.availability_start_ns / 1'000'000 + static_cast<std::int64_t>(*edge_s * 1000.0);
    }
    return idx;
}

std::shared_ptr<const route::DeliveredObject> Engine::mediaObject(std::uint16_t bsid, std::uint16_t service_id, const std::string& name) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return nullptr;
    for (const Service::CachedSegment& c : s->media)
        if (c.info.name == name) return c.obj;
    return nullptr;
}

std::optional<std::string> Engine::originMpd(std::uint16_t bsid, std::uint16_t service_id, const std::string& base_url, const std::string& time_url,
                                             const std::string& location) const {
    std::string mpd_xml;
    std::map<std::string, std::uint64_t> start_numbers;   // $Number$ calibration (spec 5.17.3)
    {
        ApiLock lock(*this);
        const Service* s = findService(bsid, service_id);
        if (!s || !s->sls || !s->sls->mpd) return std::nullopt;
        mpd_xml = s->sls->mpd->raw_xml;
        for (const MediaRepresentationInfo& r : buildMediaIndex(*s, bsid, service_id, 0).representations)
            if (r.number_shift != 0) start_numbers[r.id] = r.effective_start_number;
    }
    std::string out, err;
    if (!sls::makeOriginMpd(mpd_xml, base_url, time_url, location, out, &err, start_numbers)) return std::nullopt;
    return out;
}

std::optional<std::string> Engine::staticMpd(std::uint16_t bsid, std::uint16_t service_id) const {
    std::string mpd_xml;
    std::map<std::string, std::vector<std::uint64_t>> keys;
    {
        ApiLock lock(*this);
        const Service* s = findService(bsid, service_id);
        if (!s || !s->sls || !s->sls->mpd) return std::nullopt;
        mpd_xml = s->sls->mpd->raw_xml;
        for (const Service::CachedSegment& c : s->media)
            if (!c.info.is_init) keys[c.info.rep_id].push_back(c.info.key);
    }
    if (keys.empty()) return std::nullopt;
    std::string out, err;
    if (!sls::makeStaticMpd(mpd_xml, keys, out, &err)) return std::nullopt;
    return out;
}

// ---- MMT media store ----

void Engine::onMmtTable(Service& s, const mmtp::MpTable& t) {
    // called with mu_ held: assets announced on other UDP flows (MMT_general_location_info type 1) join the session
    if (!chain_ || !s.mmt) return;
    for (const mmtp::Asset& a : t.assets) {
        for (const mmtp::AssetLocation& loc : a.locations) {
            if (loc.type != 1 || loc.dst_ip == 0) continue;
            // the SLS flow itself is already subscribed (a second subscription would deliver every packet twice)
            if (s.slt.bss && loc.dst_ip == s.slt.bss->sls_destination_ip && loc.dst_port == s.slt.bss->sls_destination_port &&
                (loc.src_ip == 0 || s.slt.bss->sls_source_ip == 0 || loc.src_ip == s.slt.bss->sls_source_ip))
                continue;
            const std::uint64_t key = (static_cast<std::uint64_t>(loc.dst_ip) << 16) ^ loc.dst_port ^ (static_cast<std::uint64_t>(loc.src_ip) << 32);
            if (!s.mmt_extra_keys.insert(key).second) continue;
            ip::FlowKey fk;
            fk.dst_ip = loc.dst_ip;
            fk.dst_port = loc.dst_port;
            fk.src_ip = loc.src_ip;
            mmtp::Session* ms = s.mmt.get();
            s.mmt_extra_subs.push_back(chain_->demux.subscribe(fk, [ms](const ip::UdpDatagram& d) { ms->onDatagram(d); }));
        }
    }
}

void Engine::onMmtInit(Service& s, std::shared_ptr<const mmtp::MpuInitSegment> i) {
    // called with mu_ held (tuner thread)
    Service::MmtComp& c = s.mmt_comps[i->packet_id];
    c.init = std::move(i);
    c.init_seq = ++s.mmt_seq;
    bump();
}

void Engine::onWrap() {
    // called with mu_ held (tuner thread): MPU numbering and presentation times repeat from the capture start, so the
    // assemblers and the media stores start over; the sequence counters stay monotonic for the pollers
    if (!chain_) return;
    ++status_.media_generation;
    for (auto& [k, s] : chain_->services) {
        if (s->mmt) {
            s->mmt->resetMpuTimeline();   // presentation times repeat: the first descriptor of the new pass re-anchors
            s->clearMmt();
        }
        s->clearMedia();
    }
}

void Engine::onEndOfStream() {
    // called with mu_ held. The assemblers are deliberately not flushed: the last MPU of a cut capture is incomplete
    // and would only be emitted zero-filled; it stays pending and is dropped when the capture is closed or wraps.
    (void)chain_;
}

void Engine::onMmtFragment(Service& s, std::shared_ptr<const mmtp::MpuFragment> f) {
    // called with mu_ held (tuner thread)
    if (f->restart) {   // encoder restart on this asset: the whole service timeline is suspect, start the store over
        ++status_.media_generation;
        s.clearMmt();
    }
    Service::MmtComp& c = s.mmt_comps[f->packet_id];
    s.mmt_bytes += f->bytes.size();
    if (f->damaged) ++c.damaged;
    c.frags.emplace_back(++s.mmt_seq, std::move(f));
    const std::size_t limit = chain_ ? chain_->opts.media_cache_bytes : (96u << 20);
    while (s.mmt_bytes > limit) {   // evict the globally oldest fragment of the service
        Service::MmtComp* victim = nullptr;
        std::uint64_t oldest = ~std::uint64_t{0};
        for (auto& [pid, comp] : s.mmt_comps) {
            if (!comp.frags.empty() && comp.frags.front().first < oldest) {
                oldest = comp.frags.front().first;
                victim = &comp;
            }
        }
        if (!victim) break;
        s.mmt_bytes -= std::min(s.mmt_bytes, victim->frags.front().second->bytes.size());
        victim->frags.pop_front();
        ++s.mmt_evicted;
    }
    if ((s.mmt_seq % 2) == 0) bump();
}

std::optional<MmtIndex> Engine::mmtMedia(std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s || !s->mmt) return std::nullopt;
    MmtIndex idx;
    idx.bsid = bsid;
    idx.service_id = service_id;
    idx.selected = s->selected;
    idx.generation = status_.media_generation;
    idx.next_seq = s->mmt_seq;
    idx.cached_bytes = s->mmt_bytes;
    idx.cache_limit = chain_ ? chain_->opts.media_cache_bytes : 0;
    idx.evicted = s->mmt_evicted;
    idx.anchor_ntp = s->mmt->anchorNtp();
    if (idx.anchor_ntp) idx.anchor_unix_ms = static_cast<std::int64_t>(mmtp::ntpToUnixSeconds(*idx.anchor_ntp) * 1000.0);
    // components: every asset of the MP table, enriched from the init segment and the assembler
    std::map<std::uint16_t, MmtComponentInfo> comps;
    if (const mmtp::MpTable* t = s->mmt->mpTable()) {
        for (const mmtp::Asset& a : t->assets) {
            const auto pid = a.packetId();
            if (!pid) continue;
            MmtComponentInfo& c = comps[*pid];
            c.packet_id = *pid;
            c.asset_id = a.assetIdText();
            c.asset_type = a.asset_type;
            c.default_asset = a.default_asset;
            c.kind = kindFromCodec(a.asset_type, 0, false);
        }
    }
    for (const auto& [pid, comp] : s->mmt_comps) {
        MmtComponentInfo& c = comps[pid];
        c.packet_id = pid;
        if (comp.init) {
            const media::TrackInfo& t = comp.init->media;
            c.kind = t.kind();
            c.sample_entry = t.sample_entry;
            c.codecs = t.codecs;
            c.mime = t.containerMime();
            c.lang = t.language;
            c.timescale = t.timescale;
            c.width = t.width;
            c.height = t.height;
            c.channels = t.channels;
            c.sample_rate = t.sample_rate;
            c.init_seq = comp.init_seq;
            c.init_bytes = comp.init->bytes.size();
        }
        c.fragments = comp.frags.size();
        c.damaged = comp.damaged;
        if (const mmtp::MpuAssembler* a = s->mmt->assembler(pid)) {
            c.timestamp_offset_s = a->timestampOffsetSeconds();
            c.timeline_mode = a->timelineMode() == mmtp::TimelineMode::Keep ? "keep" : a->timelineMode() == mmtp::TimelineMode::Rewrite ? "rewrite" : "undecided";
            c.dropped = a->stats().fragments_dropped;
            c.mpu_gaps = a->stats().mpu_gaps;
        }
        const double off = c.timestamp_offset_s.value_or(0.0);
        if (!comp.frags.empty()) {
            const mmtp::MpuFragment& last = *comp.frags.back().second;
            c.last_mpu = last.mpu_sequence_number;
            if (last.timescale) c.last_end_s = off + static_cast<double>(last.tfdt + last.duration) / static_cast<double>(last.timescale);
            if (c.last_end_s && (!idx.live_edge_s || *c.last_end_s > *idx.live_edge_s)) idx.live_edge_s = c.last_end_s;
        }
        for (const auto& [seq, f] : comp.frags) {
            if (seq <= since) continue;
            MmtFragmentInfo fi;
            fi.seq = seq;
            fi.packet_id = pid;
            fi.mpu = f->mpu_sequence_number;
            fi.bytes = f->bytes.size();
            const double ts = f->timescale ? static_cast<double>(f->timescale) : 1.0;
            fi.start_s = off + static_cast<double>(f->tfdt) / ts;
            fi.duration_s = static_cast<double>(f->duration) / ts;
            fi.damaged = f->damaged;
            fi.discontinuity = f->discontinuity;
            fi.tfdt_rewritten = f->tfdt_rewritten;
            if (f->presentation_ntp) fi.presentation_unix_ms = static_cast<std::int64_t>(mmtp::ntpToUnixSeconds(*f->presentation_ntp) * 1000.0);
            idx.fragments.push_back(fi);
        }
    }
    for (auto& [pid, c] : comps) idx.components.push_back(std::move(c));
    std::sort(idx.fragments.begin(), idx.fragments.end(), [](const MmtFragmentInfo& a, const MmtFragmentInfo& b) { return a.seq < b.seq; });
    return idx;
}

std::shared_ptr<const mmtp::MpuInitSegment> Engine::mmtInit(std::uint16_t bsid, std::uint16_t service_id, std::uint16_t packet_id) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return nullptr;
    const auto it = s->mmt_comps.find(packet_id);
    return it == s->mmt_comps.end() ? nullptr : it->second.init;
}

std::shared_ptr<const mmtp::MpuFragment> Engine::mmtFragment(std::uint16_t bsid, std::uint16_t service_id, std::uint16_t packet_id, std::uint32_t mpu) const {
    ApiLock lock(*this);
    const Service* s = findService(bsid, service_id);
    if (!s) return nullptr;
    const auto it = s->mmt_comps.find(packet_id);
    if (it == s->mmt_comps.end()) return nullptr;
    for (const auto& [seq, f] : it->second.frags)
        if (f->mpu_sequence_number == mpu) return f;
    return nullptr;
}

}  // namespace atsc3::service
