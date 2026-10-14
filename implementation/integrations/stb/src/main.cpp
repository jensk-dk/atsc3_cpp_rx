// atsc3stb — a set-top-box style player (spec 5.20): the stack on the pcap virtual tuner, GStreamer A/V decoding in a
// window, a vertical channel list on the left half of the screen and a channel banner (service name, present /
// following from the ESG) at the bottom on every channel change. Channel changes within the current mux only; no
// track selection, no broadcaster applications.
#include <glib-unix.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atsc3/esg/esg.hpp"
#include "atsc3/service/engine.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "model.hpp"
#include "overlay.hpp"
#include "player.hpp"

using namespace atsc3;
using namespace atsc3::stb;

namespace {

constexpr std::int64_t kNs = 1'000'000'000;
constexpr double kLiveDelayMinS = 1.5, kLiveDelayMaxS = 2.5;
constexpr double kInitialTuneFallbackS = 15.0;   // --service not seen within this time: tune the first channel

struct Options {
    std::string capture;
    std::string service;                 // "" | service id | major.minor
    std::string video_sink = "autovideosink", audio_sink = "autoaudiosink";
    std::string sidecar = "auto";
    std::string keys;                    // scripted key presses "Down@5,Return@6"
    int width = 1280, height = 720;
    double start_s = 0.0, duration_s = 0.0, banner_s = 5.0, live_delay_s = 0.0;
    bool loop = false, esg = true, show_hidden = false, verbose = false, hw_decode = false;
};

void usage(FILE* out) {
    std::fputs(
        "usage: atsc3stb [options] <capture.pcap>\n"
        "  --service ID|major.minor   channel to start on (default: the first channel of the list)\n"
        "  --size WxH                 output frame size (default 1280x720)\n"
        "  --video-sink NAME          GStreamer video sink (default autovideosink; fakevideosink for headless runs)\n"
        "  --audio-sink NAME          GStreamer audio sink (default autoaudiosink; fakesink for headless runs)\n"
        "  --loop                     replay the capture in a loop\n"
        "  --start S                  skip the first S seconds of the capture\n"
        "  --duration S               exit after S seconds (default: at the end of the capture)\n"
        "  --banner-seconds S         how long the channel banner stays (default 5)\n"
        "  --live-delay S             lead of the data over the clock (default: twice the segment duration, 1.5-2.5 s)\n"
        "  --sidecar auto|off|PATH    DekTec .xa3 sidecar for raw-IP captures (default auto)\n"
        "  --show-hidden              list hidden services too\n"
        "  --no-esg                   do not open the ESG sessions (no present/following)\n"
        "  --hw-decode                allow hardware decoders (VA-API, V4L2); software decoders only by default\n"
        "  --keys LIST                scripted keys, e.g. \"Down@5,Return@6,l@10\" (GstNavigation key names @ seconds)\n"
        "  --verbose                  timeline, GStreamer warnings and feeding details\n"
        "keys: Return (or l) opens the channel list: Up/Down highlight, Return tunes, Escape closes;\n"
        "      with the list closed Up/Down (or Page Up/Down) change channel, i shows the banner, q quits\n",
        out);
}

bool parseArgs(int argc, char** argv, Options& o, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](std::string& dst) {
            if (i + 1 >= argc) {
                *error = a + " needs a value";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-h" || a == "--help") {
            usage(stdout);
            std::exit(0);
        } else if (a == "--service") {
            if (!value(o.service)) return false;
        } else if (a == "--size") {
            if (!value(v)) return false;
            if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2 || o.width < 64 || o.height < 64) {
                *error = "--size wants WxH";
                return false;
            }
        } else if (a == "--video-sink") {
            if (!value(o.video_sink)) return false;
        } else if (a == "--audio-sink") {
            if (!value(o.audio_sink)) return false;
        } else if (a == "--sidecar") {
            if (!value(o.sidecar)) return false;
        } else if (a == "--keys") {
            if (!value(o.keys)) return false;
        } else if (a == "--start") {
            if (!value(v)) return false;
            o.start_s = std::atof(v.c_str());
        } else if (a == "--duration") {
            if (!value(v)) return false;
            o.duration_s = std::atof(v.c_str());
        } else if (a == "--banner-seconds") {
            if (!value(v)) return false;
            o.banner_s = std::atof(v.c_str());
        } else if (a == "--live-delay") {
            if (!value(v)) return false;
            o.live_delay_s = std::atof(v.c_str());
        } else if (a == "--loop") {
            o.loop = true;
        } else if (a == "--show-hidden") {
            o.show_hidden = true;
        } else if (a == "--no-esg") {
            o.esg = false;
        } else if (a == "--hw-decode") {
            o.hw_decode = true;
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (!a.empty() && a[0] == '-') {
            *error = "unknown option " + a;
            return false;
        } else if (o.capture.empty()) {
            o.capture = a;
        } else {
            *error = "only one capture can be given";
            return false;
        }
    }
    if (o.capture.empty()) {
        *error = "no capture given";
        return false;
    }
    return true;
}

struct ScriptedKey {
    double at_s = 0.0;
    std::string name;
};
std::vector<ScriptedKey> parseKeys(const std::string& list, std::string* error) {
    std::vector<ScriptedKey> out;
    std::size_t pos = 0;
    while (pos <= list.size() && !list.empty()) {
        const std::size_t comma = list.find(',', pos);
        const std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) {
            const std::size_t at = item.rfind('@');
            if (at == std::string::npos || at == 0) {
                if (error) *error = "--keys item \"" + item + "\" is not name@seconds";
                return {};
            }
            out.push_back(ScriptedKey{std::atof(item.c_str() + at + 1), item.substr(0, at)});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptedKey& a, const ScriptedKey& b) { return a.at_s < b.at_s; });
    return out;
}

double monoS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TuneRecord {
    Channel channel;
    double started_s = 0.0, ended_s = 0.0;
    std::uint64_t video_frames = 0, audio_buffers = 0;
};

class App {
public:
    explicit App(Options opts) : opts_(std::move(opts)), player_(playerConfig(opts_)) {}

    int run() {
        std::string err;
        keys_ = parseKeys(opts_.keys, &err);
        if (!err.empty()) {
            log(err);
            return 2;
        }
        pcap::VirtualTunerConfig cfg;
        cfg.path = opts_.capture;
        cfg.pacing = pcap::Pacing::RealTime;
        cfg.loop = opts_.loop;
        cfg.sidecar = opts_.sidecar;
        cfg.start_offset_ns = static_cast<std::int64_t>(opts_.start_s * 1e9);
        service::OpenOptions o;
        o.auto_open_esg = opts_.esg;
        if (!engine_.open(std::make_shared<pcap::VirtualTuner>(cfg), o, &err)) {
            log("cannot open " + opts_.capture + ": " + err);
            return 1;
        }
        log("opened " + opts_.capture + (opts_.loop ? " (loop)" : ""));
        player_.setMessage([this](const std::string& text, bool fatal) {
            if (fatal) {
                log("GStreamer error: " + text);
                exit_code_ = 1;
                quit();
            } else if (opts_.verbose) {
                log(text);
            }
        });
        player_.setDraw([this](cairo_t* cr, int w, int h) {
            UiSnapshot ui;
            {
                std::lock_guard<std::mutex> lock(ui_mu_);
                ui = ui_;
            }
            drawUi(cr, w, h, ui);
        });
        player_.setKey([this](const std::string& name) { onKey(name); });
        if (!player_.start(&err)) {
            log("cannot start playback: " + err);
            engine_.close();
            return 1;
        }
        model_.banner_seconds = opts_.banner_s;
        if (opts_.live_delay_s > 0.0) player_.setLiveDelay(opts_.live_delay_s);
        loop_ = g_main_loop_new(nullptr, FALSE);
        t0_ = monoS();
        g_timeout_add(20, [](gpointer user) -> gboolean { static_cast<App*>(user)->poll(); return G_SOURCE_CONTINUE; }, this);
        g_unix_signal_add(SIGINT, [](gpointer user) -> gboolean { static_cast<App*>(user)->quit(); return G_SOURCE_REMOVE; }, this);
        g_unix_signal_add(SIGTERM, [](gpointer user) -> gboolean { static_cast<App*>(user)->quit(); return G_SOURCE_REMOVE; }, this);
        g_main_loop_run(loop_);
        closeTune();
        player_.stop();
        engine_.close();
        g_main_loop_unref(loop_);
        summary();
        return exit_code_;
    }

private:
    static PlayerConfig playerConfig(const Options& o) {
        PlayerConfig c;
        c.width = o.width;
        c.height = o.height;
        c.video_sink = o.video_sink;
        c.audio_sink = o.audio_sink;
        c.live_delay_s = o.live_delay_s > 0.0 ? o.live_delay_s : kLiveDelayMinS;
        c.hw_decode = o.hw_decode;
        c.verbose = o.verbose;
        return c;
    }

    void log(const std::string& text) const {
        std::printf("atsc3stb: %s\n", text.c_str());
        std::fflush(stdout);
    }

    double elapsed() const { return monoS() - t0_; }

    void quit() {
        if (quitting_) return;
        quitting_ = true;
        if (loop_) g_main_loop_quit(loop_);
    }

    // ---- channel changes ----------------------------------------------------------------------------------------

    void closeTune() {
        if (tunes_.empty() || tunes_.back().ended_s > 0.0) return;
        TuneRecord& r = tunes_.back();
        r.ended_s = elapsed();
        r.video_frames = player_.videoFrames();
        r.audio_buffers = player_.audioBuffers();
        log("stats " + r.channel.label() + ": video frames " + std::to_string(r.video_frames) + ", audio buffers " + std::to_string(r.audio_buffers) + ", " +
            std::to_string(static_cast<int>(r.ended_s - r.started_s)) + " s");
    }

    void tune(const Channel& ch) {
        if (tuned_ && tuned_->sameService(ch)) {
            model_.setTuned(ch, elapsed());
            return;
        }
        closeTune();
        if (tuned_) engine_.select(tuned_->bsid, tuned_->service_id, false);
        player_.retune();
        verdicts_logged_.clear();
        media_since_ = mmt_since_ = 0;
        max_segment_s_ = 0.0;
        video_codec_.clear();
        audio_codec_.clear();
        video_verdict_.clear();
        tuned_ = ch;
        graph_dumped_ = false;
        epg_logged_.clear();
        engine_.select(ch.bsid, ch.service_id, true);
        model_.setTuned(ch, elapsed());
        ++banners_;
        tunes_.push_back(TuneRecord{ch, elapsed(), 0.0, 0, 0});
        log("tune " + ch.label() + " (bsid " + std::to_string(ch.bsid) + ", service " + std::to_string(ch.service_id) + ", " + ch.protocol + ")");
    }

    void onKey(const std::string& name) {
        const Key key = keyOf(name);
        if (key == Key::None) {
            if (opts_.verbose) log("key " + name + " ignored");
            return;
        }
        ++keys_applied_;
        const bool banner_before = model_.bannerVisible(elapsed());
        const Action a = model_.onKey(key, elapsed());
        if (opts_.verbose) log("key " + name);
        if (a.kind == Action::Kind::Quit) {
            log("quit requested");
            quit();
        } else if (a.kind == Action::Kind::Tune && a.channel) {
            tune(*a.channel);
        } else if (!banner_before && model_.bannerVisible(elapsed())) {
            ++banners_;
        }
    }

    // ---- polling --------------------------------------------------------------------------------------------------

    void poll() {
        if (quitting_) return;
        const double now_s = elapsed();
        while (next_key_ < keys_.size() && keys_[next_key_].at_s <= now_s) onKey(keys_[next_key_++].name);
        if (quitting_) return;
        const service::Status st = engine_.status();
        if (!st.open) {
            log("source closed: " + st.error);
            exit_code_ = 1;
            quit();
            return;
        }
        if (st.version != last_version_) {
            last_version_ = st.version;
            const std::vector<service::ServiceInfo> services = engine_.services();
            model_.setChannels(channelsOf(services, opts_.show_hidden));
            if (model_.channels().size() != channels_logged_) {
                channels_logged_ = model_.channels().size();
                log("channels " + std::to_string(channels_logged_));
                for (const Channel& c : model_.channels()) log("  " + c.label() + " (" + c.protocol + (c.protected_flag ? ", DRM" : "") + (c.hidden ? ", hidden" : "") + ")");
            }
        }
        if (!tuned_ && !model_.channels().empty()) {
            std::optional<Channel> initial = findChannel(model_.channels(), opts_.service);
            if (!initial && (opts_.service.empty() || now_s > kInitialTuneFallbackS)) {
                if (!opts_.service.empty()) log("service " + opts_.service + " not found in the mux, starting on the first channel");
                initial = model_.channels().front();
            }
            if (initial) tune(*initial);
        }
        if (opts_.esg && st.esg_version != last_esg_version_ && now_s - last_esg_poll_s_ > 3.0) {
            last_esg_version_ = st.esg_version;
            last_esg_poll_s_ = now_s;
            const service::EsgSnapshot snap = engine_.esg();
            if (!snap.fragments.empty()) {
                guide_ = esg::Guide::fromFragments(snap.fragments);
                if (opts_.verbose) {
                    std::string services;
                    for (const esg::Service* es : guide_.services()) {
                        if (!services.empty()) services += ", ";
                        services += es->id + " (" + es->global_service_id.value_or("-") + (es->major && es->minor ? " " + std::to_string(*es->major) + "." + std::to_string(*es->minor) : "") + ")";
                    }
                    log("guide: " + std::to_string(guide_.programmes().size()) + " programmes; services: " + services +
                        (tuned_ ? "; tuned " + tuned_->label() + " (" + tuned_->global_service_id.value_or("-") + ")" : ""));
                }
            }
        }
        if (st.media_generation != media_generation_) {
            if (media_generation_ != 0 && tuned_) {
                log("media timelines restarted (loop wrap): restarting the decoders");
                player_.retune();
                media_since_ = mmt_since_ = 0;
            }
            media_generation_ = st.media_generation;
        }
        if (player_.takeRebuild()) log("decoders restarted on a fresh timeline");
        if (tuned_) {
            const auto detail = engine_.service(tuned_->bsid, tuned_->service_id);
            if (detail) {
                if (detail->info.protocol == "MMTP") feedMmt(detail->info);
                else feedRoute(detail->info);
            }
        }
        updateStatus(now_s);
        updateUi(now_s, st);
        if (!graph_dumped_ && tuned_ && player_.videoFrames() > 120) {   // GST_DEBUG_DUMP_DOT_DIR: the graph once video flows
            graph_dumped_ = true;
            player_.dumpGraph("atsc3stb-playing");
        }
        if (opts_.duration_s > 0.0 && now_s >= opts_.duration_s) {
            log("duration reached");
            quit();
            return;
        }
        if (st.finished && !opts_.loop) {
            if (eof_since_s_ == 0.0) eof_since_s_ = now_s;
            if (now_s - eof_since_s_ > 2.0) {
                log("end of capture");
                quit();
            }
        }
    }

    void updateStatus(double now_s) {
        if (!tuned_) {
            status_ = model_.channels().empty() ? "Waiting for services" : "";
            return;
        }
        if (player_.videoFrames() > 0) {
            status_.clear();
            return;
        }
        if (!video_verdict_.empty()) status_ = video_verdict_;
        else if (video_codec_.empty()) status_ = now_s - tunes_.back().started_s > 8.0 ? "No video received" : "Tuning\xE2\x80\xA6";
        else status_ = "Decoding\xE2\x80\xA6";
    }

    void updateUi(double now_s, const service::Status& st) {
        UiSnapshot ui;
        ui.list_visible = model_.listVisible();
        ui.banner_visible = model_.bannerVisible(now_s);
        ui.highlight = model_.highlight();
        ui.tuned = model_.tunedIndex();
        ui.channels = model_.channels();
        ui.channel = tuned_;
        ui.now_unix = st.capture_unix_ns > 0 ? st.capture_unix_ns / kNs : 0;
        if (tuned_ && ui.now_unix > 0) {
            ui.epg = nowNext(guide_, *tuned_, ui.now_unix);
            const std::string epg = (ui.epg.now ? ui.epg.now->title + "@" + std::to_string(ui.epg.now->start_unix) : "-") + "/" + (ui.epg.next ? ui.epg.next->title : "-");
            if (epg != epg_logged_) {   // present/following changed: one line per change
                epg_logged_ = epg;
                log("epg " + tuned_->label() + ": now " + (ui.epg.now ? "\"" + ui.epg.now->title + "\" " + clockText(ui.epg.now->start_unix) + "-" + clockText(ui.epg.now->end_unix) : "-") +
                    ", next " + (ui.epg.next ? "\"" + ui.epg.next->title + "\" " + clockText(ui.epg.next->start_unix) : "-"));
            }
        }
        ui.status = status_;
        ui.video_codec = video_codec_;
        ui.audio_codec = audio_codec_;
        std::lock_guard<std::mutex> lock(ui_mu_);
        ui_ = std::move(ui);
    }

    // ---- feeding (the VLC plugin's scheme over the Player) ------------------------------------------------------------

    void addTrack(const std::string& key, ByteView init, const std::string& desc) {
        const TrackVerdict v = player_.addTrack(key, init);
        if (v.added) {
            if (v.kind == "video") video_codec_ = v.codecs;
            if (v.kind == "audio") audio_codec_ = v.codecs;
            log("track " + v.kind + " " + v.codecs + " (" + desc + ") decoding");
            return;
        }
        if (verdicts_logged_.insert(key).second) {
            log("track " + (v.kind.empty() ? std::string("?") : v.kind) + " " + v.codecs + " (" + desc + ") not played: " + v.reason);
            if (v.kind == "video") video_verdict_ = v.codecs.empty() ? "No decodable video" : "No decoder for " + v.codecs;
            if (v.kind == "audio" && audio_codec_.empty()) audio_codec_ = v.codecs + " (no decoder)";
        }
    }

    void feedRoute(const service::ServiceInfo& info) {
        const auto idx = engine_.media(info.bsid, info.service_id, media_since_);
        if (!idx || !idx->selected) return;
        if (idx->max_segment_duration_s && *idx->max_segment_duration_s > max_segment_s_) {
            max_segment_s_ = *idx->max_segment_duration_s;
            if (opts_.live_delay_s <= 0.0) player_.setLiveDelay(std::min(kLiveDelayMaxS, std::max(kLiveDelayMinS, 2.0 * max_segment_s_)));
        }
        for (const service::MediaRepresentationInfo& r : idx->representations) {
            if (!r.init || player_.hasTrack(r.id) || verdicts_logged_.count(r.id)) continue;
            if (r.kind != "video" && r.kind != "audio") continue;
            const auto obj = engine_.mediaObject(info.bsid, info.service_id, r.init_name);
            if (!obj) continue;
            addTrack(r.id, ByteView(obj->data.data(), obj->data.size()), r.id);
        }
        for (const service::MediaSegmentInfo& seg : idx->segments) {
            media_since_ = seg.seq;
            if (seg.is_init || !player_.hasTrack(seg.rep_id)) continue;
            const auto obj = engine_.mediaObject(info.bsid, info.service_id, seg.name);
            if (!obj) continue;
            player_.feedFragment(seg.rep_id, ByteView(obj->data.data(), obj->data.size()), std::nullopt, false);
        }
    }

    void feedMmt(const service::ServiceInfo& info) {
        const auto mi = engine_.mmtMedia(info.bsid, info.service_id, mmt_since_);
        if (!mi || !mi->selected) return;
        if (mi->generation != mmt_generation_) {
            if (mmt_generation_ != 0) {
                log("MMT timeline restarted: restarting the decoders");
                player_.retune();
            }
            mmt_generation_ = mi->generation;
            mmt_since_ = 0;
            return;
        }
        for (const service::MmtComponentInfo& c : mi->components) {
            const std::string key = "mmt:" + std::to_string(c.packet_id);
            if (!c.init_seq || player_.hasTrack(key) || verdicts_logged_.count(key)) continue;
            if (c.kind != "video" && c.kind != "audio") continue;
            const auto init = engine_.mmtInit(info.bsid, info.service_id, c.packet_id);
            if (!init) continue;
            addTrack(key, ByteView(init->bytes.data(), init->bytes.size()), "packet_id " + std::to_string(c.packet_id));
        }
        for (const service::MmtFragmentInfo& f : mi->fragments) {
            mmt_since_ = f.seq;
            const std::string key = "mmt:" + std::to_string(f.packet_id);
            if (!player_.hasTrack(key)) continue;
            const auto frag = engine_.mmtFragment(info.bsid, info.service_id, f.packet_id, f.mpu);
            if (!frag) continue;
            player_.feedFragment(key, ByteView(frag->bytes.data(), frag->bytes.size()), f.start_s, f.discontinuity);
        }
    }

    void summary() const {
        std::uint64_t frames = 0, buffers = 0;
        for (const TuneRecord& r : tunes_) {
            frames += r.video_frames;
            buffers += r.audio_buffers;
        }
        log("summary channels=" + std::to_string(model_.channels().size()) + " tunes=" + std::to_string(tunes_.size()) + " video_frames=" + std::to_string(frames) +
            " audio_buffers=" + std::to_string(buffers) + " banners=" + std::to_string(banners_) + " keys=" + std::to_string(keys_applied_));
    }

    Options opts_;
    service::Engine engine_;
    Player player_;
    UiModel model_;
    esg::Guide guide_;
    GMainLoop* loop_ = nullptr;
    double t0_ = 0.0;
    bool quitting_ = false;
    int exit_code_ = 0;
    std::optional<Channel> tuned_;
    std::vector<TuneRecord> tunes_;
    std::vector<ScriptedKey> keys_;
    std::size_t next_key_ = 0;
    std::uint64_t last_version_ = ~std::uint64_t{0}, last_esg_version_ = ~std::uint64_t{0};
    std::uint64_t media_since_ = 0, mmt_since_ = 0, mmt_generation_ = 0, media_generation_ = 0;
    double last_esg_poll_s_ = -10.0, max_segment_s_ = 0.0, eof_since_s_ = 0.0;
    std::size_t channels_logged_ = 0;
    std::set<std::string> verdicts_logged_;
    std::string status_, video_codec_, audio_codec_, video_verdict_;
    std::uint64_t banners_ = 0, keys_applied_ = 0;
    bool graph_dumped_ = false;
    std::string epg_logged_;
    mutable std::mutex ui_mu_;
    UiSnapshot ui_;
};

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    Options opts;
    std::string err;
    if (!parseArgs(argc, argv, opts, &err)) {
        std::fprintf(stderr, "atsc3stb: %s\n", err.c_str());
        usage(stderr);
        return 2;
    }
    App app(std::move(opts));
    return app.run();
}
