#include "player.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/video/navigation.h>
#include <gst/video/video-info.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace atsc3::stb {

namespace {
constexpr std::int64_t kNs = 1'000'000'000;
constexpr std::int64_t kBehindMarginNs = 200'000'000;       // data mapped closer than this to the clock: re-anchor
constexpr std::int64_t kFarAheadNs = 30 * kNs;              // data mapped further ahead than this + lead: restart
constexpr guint64 kAppSrcMaxBytes = guint64{64} << 20;

struct KeyEvent {
    Player::KeyFn fn;
    std::string key;
};
gboolean deliverKey(gpointer user) {
    auto* ev = static_cast<KeyEvent*>(user);
    if (ev->fn) ev->fn(ev->key);
    delete ev;
    return G_SOURCE_REMOVE;
}
}  // namespace

struct Player::Track {
    Player* owner = nullptr;
    std::string key, kind, codecs;
    std::uint32_t track_id = 0, timescale = 1;
    GstElement* bin = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* head = nullptr;        // first converter: decodebin's pad links here
    GstPad* ghost = nullptr;
    GstPad* mixer_pad = nullptr;
    bool discontinuity = true;
    std::uint64_t fragments = 0, samples = 0, bytes = 0;
    std::atomic<std::uint64_t> decoded{0};
};

// ---- caps ---------------------------------------------------------------------------------------------------------

GstCaps* capsFor(const media::DecoderConfig& dc) {
    const std::string& e = dc.original_entry;
    const media::TrackInfo& t = dc.track;
    GstCaps* caps = nullptr;
    if (e == "hev1" || e == "hvc1" || e == "dvhe" || e == "dvh1") {
        caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, (e == "hev1" || e == "dvhe") ? "hev1" : "hvc1", "alignment", G_TYPE_STRING, "au", NULL);
    } else if (e == "avc1" || e == "avc3") {
        caps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "avc", "alignment", G_TYPE_STRING, "au", NULL);
    } else if (e == "av01") {
        caps = gst_caps_new_simple("video/x-av1", "stream-format", G_TYPE_STRING, "obu-stream", "alignment", G_TYPE_STRING, "tu", NULL);
    } else if (e == "vp09") {
        caps = gst_caps_new_empty_simple("video/x-vp9");
    } else if (e == "mp4a") {
        caps = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw", NULL);
    } else if (e == "ec-3") {
        caps = gst_caps_new_empty_simple("audio/x-eac3");
    } else if (e == "ac-3") {
        caps = gst_caps_new_empty_simple("audio/x-ac3");
    } else if (e == "ac-4") {
        caps = gst_caps_new_empty_simple("audio/x-ac4");   // no GStreamer decoder exists; named so the verdict can say so
    } else if (e == "Opus") {
        caps = gst_caps_new_empty_simple("audio/x-opus");
    } else {
        return nullptr;
    }
    if (t.width && t.height && *t.width && *t.height) gst_caps_set_simple(caps, "width", G_TYPE_INT, static_cast<int>(*t.width), "height", G_TYPE_INT, static_cast<int>(*t.height), NULL);
    if (t.sample_rate && *t.sample_rate) gst_caps_set_simple(caps, "rate", G_TYPE_INT, static_cast<int>(*t.sample_rate), NULL);
    if (t.channels && *t.channels) gst_caps_set_simple(caps, "channels", G_TYPE_INT, static_cast<int>(*t.channels), NULL);
    if (!dc.extradata.empty() && (dc.extradata_box == "hvcC" || dc.extradata_box == "avcC" || dc.extradata_box == "esds" || dc.extradata_box == "av1C")) {
        GstBuffer* cd = gst_buffer_new_memdup(dc.extradata.data(), dc.extradata.size());
        gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, cd, NULL);
        gst_buffer_unref(cd);
    }
    return caps;
}

bool canDecode(GstCaps* caps, bool allow_hardware) {
    if (!caps) return false;
    GList* decoders = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    GList* matching = gst_element_factory_list_filter(decoders, caps, GST_PAD_SINK, FALSE);
    bool ok = false;
    for (GList* l = matching; l; l = l->next) {
        auto* f = GST_ELEMENT_FACTORY(l->data);
        if (allow_hardware || !gst_element_factory_list_is_type(f, GST_ELEMENT_FACTORY_TYPE_HARDWARE)) {
            ok = true;
            break;
        }
    }
    gst_plugin_feature_list_free(matching);
    gst_plugin_feature_list_free(decoders);
    return ok;
}

// ---- construction -------------------------------------------------------------------------------------------------

Player::Player(PlayerConfig cfg) : cfg_(std::move(cfg)), live_delay_s_(cfg_.live_delay_s) {}

Player::~Player() { stop(); }

void Player::setDraw(DrawFn fn) {
    std::lock_guard<std::mutex> lock(draw_mu_);
    draw_ = std::move(fn);
}
void Player::setKey(KeyFn fn) { key_ = std::move(fn); }
void Player::setMessage(MessageFn fn) { message_ = std::move(fn); }

void Player::message(const std::string& text, bool fatal) {
    if (message_) message_(text, fatal);
}

GstElement* Player::make(const char* factory, const char* name, std::string* error) {
    GstElement* e = gst_element_factory_make(factory, name);
    if (!e && error) *error = std::string("GStreamer element \"") + factory + "\" is not available (install the plugin that provides it)";
    return e;
}

bool Player::buildPipeline(std::string* error) {
    pipeline_ = gst_pipeline_new("atsc3stb");
    GstCaps* frame = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, cfg_.width, "height", G_TYPE_INT, cfg_.height, "framerate", GST_TYPE_FRACTION, 60, 1, NULL);
    // video: black background → compositor → convert → cairo overlay → convert → sink
    GstElement* bg = make("videotestsrc", "background", error);
    GstElement* bgcaps = make("capsfilter", "background-caps", error);
    compositor_ = make("compositor", "mix", error);
    GstElement* outcaps = make("capsfilter", "frame-caps", error);
    GstElement* vconv1 = make("videoconvert", "vconv-pre", error);
    overlay_ = make("cairooverlay", "ui", error);
    GstElement* vconv2 = make("videoconvert", "vconv-post", error);
    GstElement* vsink = make(cfg_.video_sink.c_str(), "video-sink", error);
    // audio: silence → audiomixer → convert → resample → sink
    GstElement* asrc = make("audiotestsrc", "silence", error);
    audiomixer_ = make("audiomixer", "amix", error);
    GstElement* aconv = make("audioconvert", "aconv", error);
    GstElement* ares = make("audioresample", "ares", error);
    GstElement* asink = make(cfg_.audio_sink.c_str(), "audio-sink", error);
    if (!bg || !bgcaps || !compositor_ || !outcaps || !vconv1 || !overlay_ || !vconv2 || !vsink || !asrc || !audiomixer_ || !aconv || !ares || !asink) {
        gst_caps_unref(frame);
        return false;
    }
    gst_util_set_object_arg(G_OBJECT(bg), "pattern", "black");
    g_object_set(bg, "is-live", TRUE, NULL);
    g_object_set(bgcaps, "caps", frame, NULL);
    g_object_set(outcaps, "caps", frame, NULL);
    gst_caps_unref(frame);
    gst_util_set_object_arg(G_OBJECT(compositor_), "background", "black");
    gst_util_set_object_arg(G_OBJECT(asrc), "wave", "silence");
    g_object_set(asrc, "is-live", TRUE, NULL);
    if (cfg_.audio_sink == "fakesink") g_object_set(asink, "sync", TRUE, NULL);
    if (cfg_.video_sink == "fakesink") g_object_set(vsink, "sync", TRUE, NULL);

    gst_bin_add_many(GST_BIN(pipeline_), bg, bgcaps, compositor_, outcaps, vconv1, overlay_, vconv2, vsink, asrc, audiomixer_, aconv, ares, asink, NULL);
    if (!gst_element_link(bg, bgcaps) || !gst_element_link_many(compositor_, outcaps, vconv1, overlay_, vconv2, vsink, NULL) ||
        !gst_element_link_many(audiomixer_, aconv, ares, asink, NULL)) {
        if (error) *error = "the persistent GStreamer pipeline could not be linked";
        return false;
    }
    {
        GstPad* p = gst_element_request_pad_simple(compositor_, "sink_%u");
        GstPad* s = gst_element_get_static_pad(bgcaps, "src");
        g_object_set(p, "zorder", static_cast<guint>(0), NULL);
        const bool ok = gst_pad_link(s, p) == GST_PAD_LINK_OK;
        gst_object_unref(s);
        gst_object_unref(p);
        if (!ok) {
            if (error) *error = "the background could not be linked to the compositor";
            return false;
        }
    }
    {
        GstPad* p = gst_element_request_pad_simple(audiomixer_, "sink_%u");
        GstPad* s = gst_element_get_static_pad(asrc, "src");
        const bool ok = gst_pad_link(s, p) == GST_PAD_LINK_OK;
        gst_object_unref(s);
        gst_object_unref(p);
        if (!ok) {
            if (error) *error = "the silence source could not be linked to the audio mixer";
            return false;
        }
    }
    g_signal_connect(overlay_, "draw", G_CALLBACK(&Player::onDraw), this);
    g_signal_connect(overlay_, "caps-changed", G_CALLBACK(&Player::onCapsChanged), this);
    {
        GstPad* src = gst_element_get_static_pad(overlay_, "src");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, &Player::onUpstreamEvent, this, nullptr);
        gst_object_unref(src);
    }
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    bus_watch_ = gst_bus_add_watch(bus, &Player::onBus, this);
    gst_object_unref(bus);
    return true;
}

bool Player::start(std::string* error) {
    if (pipeline_) return true;
    if (!buildPipeline(error)) {
        stop();
        return false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = "the GStreamer pipeline refused to start (sinks \"" + cfg_.video_sink + "\" / \"" + cfg_.audio_sink + "\")";
        stop();
        return false;
    }
    // live sources: the change completes with NO_PREROLL; wait so that the base time is set before the first tick()
    const GstStateChangeReturn r = gst_element_get_state(pipeline_, nullptr, nullptr, 5 * GST_SECOND);
    if (r == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = "the GStreamer pipeline failed while starting (see the bus error)";
        stop();
        return false;
    }
    return true;
}

void Player::stop() {
    if (!pipeline_) return;
    dropTracks();
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_watch_) {
        g_source_remove(bus_watch_);
        bus_watch_ = 0;
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    compositor_ = audiomixer_ = overlay_ = nullptr;
}

// ---- tracks -------------------------------------------------------------------------------------------------------

void Player::retune() {
    dropTracks();
    anchored_ = false;
    rebuild_ = false;
}

bool Player::hasTrack(const std::string& key) const { return tracks_.count(key) != 0; }

bool Player::takeRebuild() {
    const bool r = rebuild_;
    rebuild_ = false;
    return r;
}

void Player::dropTracks() {
    for (auto& [key, t] : tracks_) {
        gst_element_set_locked_state(t->bin, TRUE);   // the pipeline's state continuation must not restart it
        gst_element_set_state(t->bin, GST_STATE_NULL);
        if (t->ghost && t->mixer_pad) gst_pad_unlink(t->ghost, t->mixer_pad);
        if (t->mixer_pad) {
            GstElement* mixer = t->kind == "video" ? compositor_ : audiomixer_;
            gst_element_release_request_pad(mixer, t->mixer_pad);
            gst_object_unref(t->mixer_pad);
            t->mixer_pad = nullptr;
        }
        gst_bin_remove(GST_BIN(pipeline_), t->bin);   // drops the bin's reference
        t->bin = nullptr;
    }
    tracks_.clear();
    video_frames_ = 0;
    audio_buffers_ = 0;
}

TrackVerdict Player::addTrack(const std::string& key, ByteView init) {
    TrackVerdict v;
    std::vector<media::DecoderConfig> cfgs;
    std::string err;
    if (!media::decoderConfigs(init, cfgs, &err) || cfgs.empty()) {
        v.reason = "init segment unusable (" + err + ")";
        return v;
    }
    const media::DecoderConfig& dc = cfgs.front();
    v.kind = dc.track.kind();
    v.codecs = dc.track.codecs.empty() ? dc.original_entry : dc.track.codecs;
    if (v.kind != "video" && v.kind != "audio") {
        v.reason = v.kind == "subtitles" ? "subtitles are not rendered" : "not an audio or video track";
        return v;
    }
    if (dc.track.encrypted) {
        v.reason = "encrypted (no decryption)";
        return v;
    }
    GstCaps* caps = capsFor(dc);
    if (!caps) {
        v.reason = "sample entry " + dc.original_entry + " has no GStreamer caps mapping";
        return v;
    }
    gchar* cs = gst_caps_to_string(caps);
    v.caps = cs ? cs : "";
    g_free(cs);
    if (cfg_.verbose) message("track " + key + ": " + v.caps.substr(0, 160) + (v.caps.size() > 160 ? "..." : "") + ", extradata " + std::to_string(dc.extradata.size()) + " bytes (" + dc.extradata_box + ")", false);
    if (!canDecode(caps, cfg_.hw_decode)) {
        gst_caps_unref(caps);
        v.reason = "no GStreamer decoder for " + v.caps;
        return v;
    }
    if (!pipeline_) {
        gst_caps_unref(caps);
        v.reason = "player not started";
        return v;
    }
    const bool video = v.kind == "video";
    auto t = std::make_unique<Track>();
    t->owner = this;
    t->key = key;
    t->kind = v.kind;
    t->codecs = v.codecs;
    t->track_id = dc.track.track_id;
    t->timescale = dc.track.timescale ? dc.track.timescale : 1;
    t->bin = gst_bin_new(("track-" + key).c_str());
    t->appsrc = gst_element_factory_make("appsrc", nullptr);
    GstElement* dec = gst_element_factory_make("decodebin", nullptr);
    t->head = gst_element_factory_make(video ? "videoconvert" : "audioconvert", nullptr);
    GstElement* tail = gst_element_factory_make(video ? "videoscale" : "audioresample", nullptr);
    if (!t->appsrc || !dec || !t->head || !tail) {
        gst_caps_unref(caps);
        v.reason = "GStreamer elements appsrc/decodebin/convert are not available";
        return v;
    }
    g_object_set(t->appsrc, "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE, "do-timestamp", FALSE, "block", FALSE, "max-bytes", kAppSrcMaxBytes,
                 "min-latency", static_cast<gint64>(0), "max-latency", static_cast<gint64>(2 * GST_SECOND), NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(t->bin), t->appsrc, dec, t->head, tail, NULL);
    gst_element_link(t->appsrc, dec);
    gst_element_link(t->head, tail);
    g_signal_connect(dec, "pad-added", G_CALLBACK(&Player::onPadAdded), t.get());
    g_signal_connect(dec, "autoplug-select", G_CALLBACK(&Player::onAutoplugSelect), t.get());
    {
        GstPad* src = gst_element_get_static_pad(tail, "src");
        t->ghost = gst_ghost_pad_new("src", src);
        gst_object_unref(src);
        gst_pad_set_active(t->ghost, TRUE);
        gst_element_add_pad(t->bin, t->ghost);
    }
    gst_bin_add(GST_BIN(pipeline_), t->bin);
    GstElement* mixer = video ? compositor_ : audiomixer_;
    t->mixer_pad = gst_element_request_pad_simple(mixer, "sink_%u");
    if (video) {
        g_object_set(t->mixer_pad, "zorder", static_cast<guint>(1), "xpos", 0, "ypos", 0, "width", cfg_.width, "height", cfg_.height, NULL);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(t->mixer_pad), "sizing-policy")) gst_util_set_object_arg(G_OBJECT(t->mixer_pad), "sizing-policy", "keep-aspect-ratio");
    }
    gst_pad_add_probe(t->mixer_pad, GST_PAD_PROBE_TYPE_BUFFER, &Player::onMixerBuffer, t.get(), nullptr);
    if (gst_pad_link(t->ghost, t->mixer_pad) != GST_PAD_LINK_OK) {
        gst_element_release_request_pad(mixer, t->mixer_pad);
        gst_object_unref(t->mixer_pad);
        gst_bin_remove(GST_BIN(pipeline_), t->bin);
        v.reason = "the track could not be linked to the " + std::string(video ? "compositor" : "audio mixer");
        return v;
    }
    // Explicitly PLAYING, not sync_state_with_parent: while another branch's decodebin is still completing its
    // asynchronous state change the pipeline reports PAUSED, and a branch synced to that never streams.
    gst_element_set_state(t->bin, GST_STATE_PLAYING);
    tracks_[key] = std::move(t);
    v.added = true;
    return v;
}

void Player::onPadAdded(GstElement* /*decodebin*/, GstPad* pad, gpointer user) {
    auto* t = static_cast<Track*>(user);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    bool matches = false;
    if (caps && gst_caps_get_size(caps) > 0) {
        const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
        matches = name && ((t->kind == "video" && g_str_has_prefix(name, "video/x-raw")) || (t->kind == "audio" && g_str_has_prefix(name, "audio/x-raw")));
    }
    if (t->owner->cfg_.verbose) {
        gchar* cs = caps ? gst_caps_to_string(caps) : nullptr;
        t->owner->message("track " + t->key + ": decodebin exposed " + (cs ? std::string(cs).substr(0, 120) : std::string("?")), false);
        g_free(cs);
    }
    if (caps) gst_caps_unref(caps);
    if (!matches) return;
    GstPad* sink = gst_element_get_static_pad(t->head, "sink");
    if (!gst_pad_is_linked(sink)) {
        if (gst_pad_link(pad, sink) != GST_PAD_LINK_OK) t->owner->message("track " + t->key + ": decoded pad could not be linked", false);
    }
    gst_object_unref(sink);
}

// decodebin's GstAutoplugSelectResult: 0 = try the factory, 2 = skip it. Hardware decoders (VA-API, V4L2) are
// skipped unless asked for: their output path is not reliable on every desktop (a VA-API download gave garbage here).
int Player::onAutoplugSelect(GstElement* /*decodebin*/, GstPad* /*pad*/, GstCaps* /*caps*/, GstElementFactory* factory, gpointer user) {
    auto* t = static_cast<Track*>(user);
    const bool hardware = gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_HARDWARE);
    if (hardware && !t->owner->cfg_.hw_decode) return 2;
    if (t->owner->cfg_.verbose && gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_DECODER))
        t->owner->message("track " + t->key + ": decoder " + GST_OBJECT_NAME(factory) + (hardware ? " (hardware)" : ""), false);
    return 0;
}

GstPadProbeReturn Player::onMixerBuffer(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer user) {
    auto* t = static_cast<Track*>(user);
    t->decoded.fetch_add(1, std::memory_order_relaxed);
    if (t->kind == "video") t->owner->video_frames_.fetch_add(1, std::memory_order_relaxed);
    else t->owner->audio_buffers_.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

// ---- timeline and feeding ------------------------------------------------------------------------------------------

double Player::runningTime() const {
    if (!pipeline_) return 0.0;
    GstClock* clock = gst_element_get_clock(pipeline_);
    if (!clock) return 0.0;
    const GstClockTime now = gst_clock_get_time(clock);
    gst_object_unref(clock);
    const GstClockTime base = gst_element_get_base_time(pipeline_);
    if (base == GST_CLOCK_TIME_NONE || now == GST_CLOCK_TIME_NONE || now < base) return 0.0;
    return static_cast<double>(now - base) / 1e9;
}

void Player::dumpGraph(const char* name) const {
    if (pipeline_) GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline_), GST_DEBUG_GRAPH_SHOW_ALL, name);
}

std::int64_t Player::tick(double media_s) const {
    const double ns = static_cast<double>(anchor_running_ns_) + (live_delay_s_ + media_s - anchor_media_s_) * 1e9;
    return ns < 0.0 ? 0 : static_cast<std::int64_t>(ns);
}

void Player::anchor(double media_s, const char* why) {
    anchor_media_s_ = media_s;
    anchor_running_ns_ = static_cast<std::int64_t>(runningTime() * 1e9);
    anchored_ = true;
    for (auto& [k, t] : tracks_) t->discontinuity = true;
    if (cfg_.verbose) message("timeline anchored at media " + std::to_string(media_s) + " s (" + why + "), live delay " + std::to_string(live_delay_s_) + " s", false);
}

void Player::setLiveDelay(double seconds) { live_delay_s_ = seconds; }

void Player::feedFragment(const std::string& key, ByteView fragment, std::optional<double> first_dts_s, bool discontinuity) {
    const auto it = tracks_.find(key);
    if (it == tracks_.end()) return;
    Track& t = *it->second;
    media::FragmentSamples fs;
    std::string err;
    if (!media::fragmentSamples(fragment, t.track_id, fs, &err)) {
        message("track " + key + ": fragment skipped (" + err + ")", false);
        return;
    }
    if (fs.samples.empty()) return;
    const double ts = static_cast<double>(t.timescale);
    const double base_s = first_dts_s ? *first_dts_s : static_cast<double>(fs.samples.front().dts) / ts;
    const std::uint64_t dts0 = fs.samples.front().dts;
    const std::int64_t now_ns = static_cast<std::int64_t>(runningTime() * 1e9);
    if (!anchored_) anchor(base_s, "first fragment");
    const std::int64_t first = tick(base_s);
    if (first < now_ns + kBehindMarginNs) {
        anchor(base_s, "data behind the clock");   // timestamps jump forward: the branches survive
    } else if (first > now_ns + kFarAheadNs + static_cast<std::int64_t>(live_delay_s_ * 1e9)) {
        // timestamps would go backwards for the other tracks: restart the branches on a fresh anchor
        message("data far ahead of the clock (" + std::to_string((first - now_ns) / kNs) + " s): restarting the decoders", false);
        dropTracks();
        anchored_ = false;
        rebuild_ = true;
        return;
    }
    if (discontinuity) t.discontinuity = true;
    const bool video = t.kind == "video";
    for (const media::SampleRef& s : fs.samples) {
        if (s.offset + s.size > fragment.size()) break;
        GstBuffer* b = gst_buffer_new_allocate(nullptr, s.size, nullptr);
        if (!b) break;
        gst_buffer_fill(b, 0, fragment.data() + s.offset, s.size);
        const double dts_s = base_s + static_cast<double>(s.dts - dts0) / ts;
        const std::int64_t dts = tick(dts_s);
        const std::int64_t pts = video ? tick(dts_s + static_cast<double>(s.composition_offset) / ts) : dts;
        GST_BUFFER_DTS(b) = static_cast<GstClockTime>(dts);
        GST_BUFFER_PTS(b) = static_cast<GstClockTime>(std::max(pts, dts));
        GST_BUFFER_DURATION(b) = static_cast<GstClockTime>(static_cast<double>(s.duration) / ts * 1e9);
        if (video && !s.sync) GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
        if (t.discontinuity) {
            GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DISCONT);
            t.discontinuity = false;
        }
        const GstFlowReturn r = gst_app_src_push_buffer(GST_APP_SRC(t.appsrc), b);   // takes the buffer
        if (r != GST_FLOW_OK) {
            message("track " + key + ": push refused (" + gst_flow_get_name(r) + ")", false);
            break;
        }
        ++t.samples;
        t.bytes += s.size;
    }
    ++t.fragments;
    if (cfg_.verbose && t.fragments <= 3) {
        std::size_t sync = 0;
        for (const media::SampleRef& s : fs.samples) sync += s.sync ? 1 : 0;
        message("track " + key + ": fragment " + std::to_string(t.fragments) + ": " + std::to_string(fs.samples.size()) + " samples, " + std::to_string(sync) +
                    " sync, first dts " + std::to_string(base_s) + " s, composition offset " + std::to_string(fs.samples.front().composition_offset) + ", tick " +
                    std::to_string(static_cast<double>(first) / 1e9) + " s, running " + std::to_string(static_cast<double>(now_ns) / 1e9) + " s",
                false);
    }
    if (fs.truncated) message("track " + key + ": " + std::to_string(fs.truncated) + " samples beyond the fragment data", false);
}

std::vector<TrackStats> Player::stats() const {
    std::vector<TrackStats> out;
    for (const auto& [k, t] : tracks_) {
        TrackStats s;
        s.key = t->key;
        s.kind = t->kind;
        s.codecs = t->codecs;
        s.fragments = t->fragments;
        s.samples = t->samples;
        s.bytes = t->bytes;
        s.decoded = t->decoded.load();
        out.push_back(std::move(s));
    }
    return out;
}

// ---- overlay, keys, bus ---------------------------------------------------------------------------------------------

void Player::onDraw(GstElement* /*overlay*/, cairo_t* cr, guint64 /*timestamp*/, guint64 /*duration*/, gpointer user) {
    auto* p = static_cast<Player*>(user);
    std::lock_guard<std::mutex> lock(p->draw_mu_);
    if (p->draw_ && p->frame_w_ > 0 && p->frame_h_ > 0) p->draw_(cr, p->frame_w_, p->frame_h_);
}

void Player::onCapsChanged(GstElement* /*overlay*/, GstCaps* caps, gpointer user) {
    auto* p = static_cast<Player*>(user);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) return;
    std::lock_guard<std::mutex> lock(p->draw_mu_);
    p->frame_w_ = GST_VIDEO_INFO_WIDTH(&info);
    p->frame_h_ = GST_VIDEO_INFO_HEIGHT(&info);
}

GstPadProbeReturn Player::onUpstreamEvent(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user) {
    auto* p = static_cast<Player*>(user);
    GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (ev && GST_EVENT_TYPE(ev) == GST_EVENT_NAVIGATION && gst_navigation_event_get_type(ev) == GST_NAVIGATION_EVENT_KEY_PRESS) {
        const gchar* key = nullptr;
        if (gst_navigation_event_parse_key_event(ev, &key) && key && p->key_) g_idle_add(deliverKey, new KeyEvent{p->key_, key});
    }
    return GST_PAD_PROBE_OK;
}

gboolean Player::onBus(GstBus* /*bus*/, GstMessage* msg, gpointer user) {
    auto* p = static_cast<Player*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* e = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::string text = std::string(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg))) + ": " + (e ? e->message : "error");
            if (dbg && p->cfg_.verbose) text += std::string(" [") + dbg + "]";
            if (e) g_error_free(e);
            g_free(dbg);
            p->message(text, true);
            break;
        }
        case GST_MESSAGE_WARNING: {
            if (!p->cfg_.verbose) break;
            GError* e = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_warning(msg, &e, &dbg);
            p->message(std::string(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg))) + ": " + (e ? e->message : "warning"), false);
            if (e) g_error_free(e);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            p->message("end of stream", true);
            break;
        default:
            break;
    }
    return TRUE;
}

}  // namespace atsc3::stb
