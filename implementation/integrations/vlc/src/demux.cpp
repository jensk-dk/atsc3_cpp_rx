#include "demux.hpp"

#include "atsc3/tuners/pcap/virtual_tuner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <vlc_block.h>
#include <vlc_es_out.h>
#include <vlc_meta.h>
#include <vlc_variables.h>

#include "alerts.hpp"
#include "atsc3/esg/esg.hpp"
#include "atsc3/media/isobmff.hpp"
#include "epg.hpp"
#include "es_mapper.hpp"

namespace atsc3::vlcplugin {

namespace {
constexpr std::int64_t kNs = 1'000'000'000;
// Lead of the data over the clock. VLC's decoders refuse timestamps further ahead of the last PCR than the pts delay
// plus 3 s (input/clock.c: "Timestamp conversion failed"), so the lead stays within [1.5, 2.5] s and the pts delay
// reported to VLC is at least 1.5 s (spec 5.19.3's recommended network caching).
constexpr double kLiveDelayMinS = 1.5, kLiveDelayMaxS = 2.5;
constexpr std::int64_t kPtsDelayMinMs = 1500;
std::int64_t monoNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string channelOf(const service::ServiceInfo& s) {
    if (s.major && s.minor) return std::to_string(*s.major) + "." + std::to_string(*s.minor);
    return "service " + std::to_string(s.service_id);
}
std::string nameOf(const service::ServiceInfo& s) {
    if (!s.short_name.empty()) return s.short_name;
    if (!s.usbd_name.empty()) return s.usbd_name;
    return "service " + std::to_string(s.service_id);
}
}  // namespace

Demux::Demux(demux_t* d, Mrl mrl, Options opts) : d_(d), mrl_(std::move(mrl)), opts_(std::move(opts)) {}

Demux::~Demux() {
    dropTracks();
    if (alert_es_) es_out_Del(d_->out, alert_es_);
    engine_.close();
}

bool Demux::open(std::string* error) {
    if (mrl_.kind != Mrl::Kind::Capture) {
        if (error) *error = mrl_.kind == Mrl::Kind::Udp ? "live UDP sources are not implemented yet (spec 5.19.1)" : mrl_.kind == Mrl::Kind::Tuner ? "RF tuner sources are not implemented yet (spec 5.19.1)" : mrl_.error;
        return false;
    }
    pcap::VirtualTunerConfig cfg;
    cfg.path = mrl_.target;
    cfg.pacing = opts_.realtime ? pcap::Pacing::RealTime : pcap::Pacing::NonRealTime;
    cfg.speed = opts_.speed;
    cfg.loop = opts_.loop;
    cfg.sidecar = opts_.sidecar;
    cfg.start_offset_ns = static_cast<std::int64_t>(opts_.start_s * 1e9);
    service::OpenOptions o;
    o.auto_open_esg = opts_.esg;
    if (!engine_.open(std::make_shared<pcap::VirtualTuner>(cfg), o, error)) return false;
    live_delay_s_ = opts_.live_delay_ms > 0 ? opts_.live_delay_ms / 1000.0 : kLiveDelayMinS;
    msg_Info(d_, "atsc3: opened %s (%s, %s%s)", mrl_.target.c_str(), opts_.realtime ? "real-time" : "non-real-time", opts_.loop ? "loop" : "once",
             opts_.service.empty() ? "" : (", service " + opts_.service).c_str());
    return true;
}

// ---- programs -------------------------------------------------------------------------------------------------

int Demux::groupOf(const service::ServiceInfo& s) const {
    return multi_bsid_ ? (static_cast<int>(s.bsid) << 16) | s.service_id : s.service_id;
}

bool Demux::programHasAv(const service::ServiceInfo& s) const {
    if (s.protocol != "ROUTE" && s.protocol != "MMTP") return false;
    if (s.category == 1 || s.category == 2) return true;
    for (const service::ComponentInfo& c : s.components)
        if (c.kind == "video" || c.kind == "audio") return true;
    return false;
}

void Demux::announcePrograms(const std::vector<service::ServiceInfo>& services) {
    std::set<std::uint16_t> bsids;
    for (const auto& s : services) bsids.insert(s.bsid);
    const bool multi = bsids.size() > 1;
    if (multi != multi_bsid_) {   // the numbering scheme changes: forget the old programs, the loop below re-announces
        for (const int g : announced_) es_out_Control(d_->out, ES_OUT_DEL_GROUP, g);
        announced_.clear();
        group_to_service_.clear();
        multi_bsid_ = multi;
        if (selected_) {
            dropTracks();   // re-added under the new group by the next feed
            for (const auto& s : services)
                if (Key{s.bsid, s.service_id} == *selected_) selected_group_ = groupOf(s);
            es_out_Control(d_->out, ES_OUT_SET_GROUP, selected_group_);
        }
    }
    int added = 0;
    for (const service::ServiceInfo& s : services) {
        const bool av = programHasAv(s);
        if (!av && !opts_.show_hidden) continue;
        const int g = groupOf(s);
        group_to_service_[g] = Key{s.bsid, s.service_id};
        if (announced_.count(g)) continue;
        vlc_meta_t* meta = vlc_meta_New();
        if (meta) {
            std::string title = channelOf(s) + " " + nameOf(s);
            if (s.protected_flag) title += " (DRM)";
            std::string desc = s.protocol + ", " + s.category_name;
            if (s.hidden) desc += ", hidden";
            vlc_meta_Set(meta, vlc_meta_Title, title.c_str());
            vlc_meta_Set(meta, vlc_meta_Description, desc.c_str());
            if (s.global_service_id) vlc_meta_Set(meta, vlc_meta_URL, s.global_service_id->c_str());
            es_out_Control(d_->out, ES_OUT_SET_GROUP_META, g, meta);
            vlc_meta_Delete(meta);
        }
        announced_.insert(g);
        ++added;
        msg_Dbg(d_, "atsc3: program %d: %s %s (%s, bsid %u, service %u)", g, channelOf(s).c_str(), nameOf(s).c_str(), s.protocol.c_str(), s.bsid, s.service_id);
    }
    if (added) msg_Info(d_, "atsc3: programs: %zu", announced_.size());
}

std::optional<Demux::Key> Demux::chooseInitial(const std::vector<service::ServiceInfo>& services) const {
    const auto bsidOk = [&](const service::ServiceInfo& s) { return !opts_.bsid || *opts_.bsid == s.bsid; };
    if (!opts_.service.empty()) {
        if (const auto ch = parseChannel(opts_.service)) {
            for (const auto& s : services)
                if (bsidOk(s) && s.major == ch->first && s.minor == ch->second) return Key{s.bsid, s.service_id};
            return std::nullopt;
        }
        const unsigned long id = std::strtoul(opts_.service.c_str(), nullptr, 10);
        for (const auto& s : services)
            if (bsidOk(s) && s.service_id == id) return Key{s.bsid, s.service_id};
        return std::nullopt;
    }
    if (opts_.program > 0) {
        const auto it = group_to_service_.find(opts_.program);
        if (it != group_to_service_.end()) return it->second;
        return std::nullopt;
    }
    for (const auto& s : services)
        if (bsidOk(s) && programHasAv(s) && !s.hidden) return Key{s.bsid, s.service_id};
    return std::nullopt;
}

void Demux::selectService(const Key& key, const service::ServiceInfo* info) {
    if (selected_ && *selected_ == key) return;
    deselect();
    selected_ = key;
    selected_group_ = info ? groupOf(*info) : -1;
    selected_protocol_ = info ? info->protocol : "";
    media_since_ = mmt_since_ = 0;
    anchored_ = false;
    last_pcr_ = VLC_TICK_INVALID;
    engine_.select(key.first, key.second, true);
    if (selected_group_ >= 0) es_out_Control(d_->out, ES_OUT_SET_GROUP, selected_group_);   // VLC's program follows ours
    es_out_Control(d_->out, ES_OUT_RESET_PCR);
    msg_Info(d_, "atsc3: selected %s %s (program %d)", info ? channelOf(*info).c_str() : "?", info ? nameOf(*info).c_str() : "?", selected_group_);
}

void Demux::deselect() {
    if (!selected_) return;
    engine_.select(selected_->first, selected_->second, false);
    dropTracks();
    selected_.reset();
    selected_group_ = -1;
}

void Demux::dropTracks() {
    for (auto& [k, t] : tracks_)
        if (t.es) es_out_Del(d_->out, t.es);
    tracks_.clear();
}

// ---- timeline ----------------------------------------------------------------------------------------------------

vlc_tick_t Demux::tick(double media_s) {
    return VLC_TICK_0 + static_cast<vlc_tick_t>((live_delay_s_ + media_s - anchor_media_s_) * 1e6);
}

void Demux::resyncTimeline(double media_s, const char* why) {
    const service::Status st = engine_.status();
    anchor_media_s_ = media_s;
    anchor_capture_ns_ = st.capture_unix_ns;
    anchored_ = true;
    last_pcr_ = VLC_TICK_INVALID;
    es_out_Control(d_->out, ES_OUT_RESET_PCR);
    for (auto& [k, t] : tracks_) t.discontinuity = true;
    msg_Dbg(d_, "atsc3: timeline anchored at media %.3f s (%s), live delay %.1f s", media_s, why, live_delay_s_);
    sendPcr();
}

void Demux::sendPcr() {
    if (!anchored_) return;
    vlc_tick_t pcr;
    if (opts_.realtime) {   // the stack clock: capture time since the anchor (the tuner paces it in real time)
        const service::Status st = engine_.status();
        if (st.capture_unix_ns <= 0) return;
        pcr = VLC_TICK_0 + (st.capture_unix_ns - anchor_capture_ns_) / 1000;
    } else {   // data-driven: the slowest track's position less the live delay, so every track stays ahead of the clock
        std::optional<double> slowest;
        for (const auto& [k, t] : tracks_)
            if (t.last_end_s && (!slowest || *t.last_end_s < *slowest)) slowest = *t.last_end_s;
        if (!slowest) return;
        pcr = tick(*slowest) - static_cast<vlc_tick_t>(live_delay_s_ * 1e6);
        if (pcr < VLC_TICK_0) pcr = VLC_TICK_0;
    }
    if (last_pcr_ != VLC_TICK_INVALID && pcr <= last_pcr_) return;
    es_out_SetPCR(d_->out, pcr);
    last_pcr_ = pcr;
}

// ---- tracks and samples --------------------------------------------------------------------------------------------

Demux::Track* Demux::addTrack(const std::string& key, ByteView init, const std::string& lang, const std::string& desc, const std::string& kind_hint) {
    std::vector<media::DecoderConfig> cfgs;
    std::string err;
    if (!media::decoderConfigs(init, cfgs, &err) || cfgs.empty()) {
        msg_Warn(d_, "atsc3: %s: init segment unusable (%s)", key.c_str(), err.c_str());
        return nullptr;
    }
    const media::DecoderConfig& dc = cfgs.front();
    es_format_t fmt;
    EsFormatHints hints;
    hints.group = selected_group_;
    hints.es_id = next_es_id_++;
    hints.language = lang;
    hints.description = desc;
    if (!formatFromConfig(dc, hints, fmt)) {
        msg_Warn(d_, "atsc3: %s: track of kind %s is not an elementary stream", key.c_str(), dc.track.kind().c_str());
        return nullptr;
    }
    Track t;
    t.key = key;
    t.kind = dc.track.kind().empty() ? kind_hint : dc.track.kind();
    t.codecs = dc.track.codecs;
    t.track_id = dc.track.track_id;
    t.timescale = dc.track.timescale ? dc.track.timescale : 1;
    t.es = es_out_Add(d_->out, &fmt);
    es_format_Clean(&fmt);
    if (!t.es) {
        msg_Warn(d_, "atsc3: %s: es_out_Add failed", key.c_str());
        return nullptr;
    }
    msg_Info(d_, "atsc3: es added %s %s (%s, track %u, timescale %u%s%s)", t.kind.c_str(), t.codecs.empty() ? dc.original_entry.c_str() : t.codecs.c_str(), key.c_str(),
             t.track_id, t.timescale, dc.track.encrypted ? ", encrypted" : "", dc.extradata.empty() ? "" : (", extradata " + dc.extradata_box).c_str());
    auto [it, inserted] = tracks_.insert_or_assign(key, std::move(t));
    (void)inserted;
    return &it->second;
}

void Demux::feedFragment(Track& t, ByteView fragment, std::optional<double> first_dts_s, bool discontinuity) {
    media::FragmentSamples fs;
    std::string err;
    if (!media::fragmentSamples(fragment, t.track_id, fs, &err)) {
        msg_Warn(d_, "atsc3: %s: fragment skipped (%s)", t.key.c_str(), err.c_str());
        return;
    }
    if (fs.samples.empty()) return;
    const double ts = static_cast<double>(t.timescale);
    // media time of the first sample on the shared timeline: the fragment's own tfdt (ROUTE), or the service-timeline
    // start the engine computed (MMT: timestamp offset applied)
    const double base_s = first_dts_s ? *first_dts_s : static_cast<double>(fs.samples.front().dts) / ts;
    const std::uint64_t dts0 = fs.samples.front().dts;
    if (!anchored_) resyncTimeline(base_s, "first fragment");
    const vlc_tick_t first_tick = tick(base_s);
    if (opts_.realtime && last_pcr_ != VLC_TICK_INVALID) {
        if (first_tick < last_pcr_ + VLC_TICK_FROM_MS(200)) resyncTimeline(base_s, "data behind the clock");
        else if (first_tick > last_pcr_ + VLC_TICK_FROM_MS(30000) + static_cast<vlc_tick_t>(live_delay_s_ * 1e6)) resyncTimeline(base_s, "data far ahead of the clock");
    }
    if (discontinuity) t.discontinuity = true;
    const std::int64_t send_t0 = monoNs();
    for (const media::SampleRef& s : fs.samples) {
        block_t* b = block_Alloc(s.size);
        if (!b) break;
        std::memcpy(b->p_buffer, fragment.data() + s.offset, s.size);
        const double dts_s = base_s + static_cast<double>(s.dts - dts0) / ts;
        b->i_dts = tick(dts_s);
        b->i_pts = t.kind == "video" ? tick(dts_s + static_cast<double>(s.composition_offset) / ts) : b->i_dts;
        b->i_length = static_cast<vlc_tick_t>(static_cast<double>(s.duration) / ts * 1e6);
        if (t.kind == "video" && s.sync) b->i_flags |= BLOCK_FLAG_TYPE_I;
        if (t.discontinuity) {
            b->i_flags |= BLOCK_FLAG_DISCONTINUITY;
            t.discontinuity = false;
        }
        es_out_Send(d_->out, t.es, b);
        ++t.samples;
        t.bytes += s.size;
    }
    ++t.fragments;
    t.last_end_s = base_s + static_cast<double>(fs.duration) / ts;
    sendPcr();
    const std::int64_t send_ms = (monoNs() - send_t0) / 1'000'000;
    if (opts_.verbose_stats && send_ms > 100) msg_Dbg(d_, "atsc3: es_out_Send held the input thread for %lld ms (%s)", static_cast<long long>(send_ms), t.key.c_str());
    if (opts_.verbose_stats) {
        msg_Dbg(d_, "atsc3: fed %s %s: %zu samples, media %.3f-%.3f s, tick %lld, pcr %lld, lead %.2f s", t.kind.c_str(), t.key.c_str(), fs.samples.size(), base_s,
                *t.last_end_s, static_cast<long long>(first_tick), static_cast<long long>(last_pcr_),
                last_pcr_ == VLC_TICK_INVALID ? 0.0 : static_cast<double>(first_tick - last_pcr_) / 1e6);
    }
    if (fs.truncated) msg_Warn(d_, "atsc3: %s: %zu samples beyond the fragment data", t.key.c_str(), fs.truncated);
}

void Demux::feedRoute(const service::ServiceInfo& info) {
    const auto idx = engine_.media(info.bsid, info.service_id, media_since_);
    if (!idx || !idx->selected) return;
    if (idx->max_segment_duration_s && *idx->max_segment_duration_s > max_segment_s_) {
        max_segment_s_ = *idx->max_segment_duration_s;
        if (opts_.live_delay_ms <= 0) live_delay_s_ = std::min(kLiveDelayMaxS, std::max(kLiveDelayMinS, 2.0 * max_segment_s_));
    }
    // non-real-time: hold the tuner while VLC is behind, so the engine's bounded cache keeps what we have not fed
    if (!opts_.realtime) {
        const std::uint64_t backlog = idx->next_seq > media_since_ ? idx->next_seq - media_since_ : 0;
        if (backlog > 6 && !pressure_paused_) {
            pressure_paused_ = true;
            engine_.pause(true);
            if (opts_.verbose_stats) msg_Dbg(d_, "atsc3: tuner held (%llu segments pending)", static_cast<unsigned long long>(backlog));
        } else if (backlog < 2 && pressure_paused_) {
            pressure_paused_ = false;
            engine_.pause(user_paused_);
            if (opts_.verbose_stats) msg_Dbg(d_, "atsc3: tuner released");
        }
    }
    for (const service::MediaRepresentationInfo& r : idx->representations) {
        if (!r.init || tracks_.count(r.id)) continue;
        if (r.kind != "video" && r.kind != "audio" && r.kind != "subtitles") continue;
        const auto obj = engine_.mediaObject(info.bsid, info.service_id, r.init_name);
        if (!obj) continue;
        std::string desc = r.id;
        for (const service::ComponentInfo& c : info.components)
            if (c.id == r.id && !c.roles.empty()) desc += " " + c.roles.front();
        addTrack(r.id, ByteView(obj->data.data(), obj->data.size()), r.lang, desc, r.kind);
    }
    for (const service::MediaSegmentInfo& seg : idx->segments) {
        media_since_ = seg.seq;
        if (seg.is_init) continue;
        const auto it = tracks_.find(seg.rep_id);
        if (it == tracks_.end()) continue;   // init not seen yet: the next poll adds the track and feeds from there
        const auto obj = engine_.mediaObject(info.bsid, info.service_id, seg.name);
        if (!obj) continue;
        feedFragment(it->second, ByteView(obj->data.data(), obj->data.size()), std::nullopt, false);
    }
}

void Demux::feedMmt(const service::ServiceInfo& info) {
    const auto mi = engine_.mmtMedia(info.bsid, info.service_id, mmt_since_);
    if (!mi || !mi->selected) return;
    if (mi->generation != mmt_generation_) {
        if (mmt_generation_ != 0) msg_Dbg(d_, "atsc3: MMT timeline restarted (generation %llu)", static_cast<unsigned long long>(mi->generation));
        mmt_generation_ = mi->generation;
        anchored_ = false;
        mmt_since_ = 0;
        return;
    }
    for (const service::MmtComponentInfo& c : mi->components) {
        const std::string key = "mmt:" + std::to_string(c.packet_id);
        if (!c.init_seq || tracks_.count(key)) continue;
        if (c.kind != "video" && c.kind != "audio" && c.kind != "subtitles") continue;
        const auto init = engine_.mmtInit(info.bsid, info.service_id, c.packet_id);
        if (!init) continue;
        Track* t = addTrack(key, ByteView(init->bytes.data(), init->bytes.size()), c.lang, "pid " + std::to_string(c.packet_id), c.kind);
        if (t) t->timestamp_offset_s = c.timestamp_offset_s.value_or(0.0);
    }
    for (const service::MmtFragmentInfo& f : mi->fragments) {
        mmt_since_ = f.seq;
        const auto it = tracks_.find("mmt:" + std::to_string(f.packet_id));
        if (it == tracks_.end()) continue;
        const auto frag = engine_.mmtFragment(info.bsid, info.service_id, f.packet_id, f.mpu);
        if (!frag) continue;
        feedFragment(it->second, ByteView(frag->bytes.data(), frag->bytes.size()), f.start_s, f.discontinuity);
    }
}

// ---- EPG, alerts, stats ---------------------------------------------------------------------------------------------

void Demux::pollEpg(const std::vector<service::ServiceInfo>& services, bool force) {
    if (!opts_.esg) return;
    const service::Status st = engine_.status();
    if (!force && st.esg_version == last_esg_version_) return;
    const std::int64_t now = monoNs();
    if (!force && now - last_epg_poll_ns_ < 3 * kNs) return;
    last_epg_poll_ns_ = now;
    last_esg_version_ = st.esg_version;
    const service::EsgSnapshot snap = engine_.esg();
    if (snap.fragments.empty()) return;
    const esg::Guide guide = esg::Guide::fromFragments(snap.fragments);
    std::vector<EpgTarget> targets;
    for (const service::ServiceInfo& s : services) {
        if (!announced_.count(groupOf(s))) continue;
        targets.push_back(EpgTarget{groupOf(s), s.global_service_id.value_or(""), s.major, s.minor});
    }
    int events = 0;
    const int programs = pushEpg(d_->out, guide, targets, st.capture_unix_ns > 0 ? st.capture_unix_ns / kNs : 0, &events);
    msg_Dbg(d_, "atsc3: epg: %zu programmes in the guide, %d events pushed to %d programs", guide.programmes().size(), events, programs);
}

void Demux::pollAlerts() {
    if (opts_.alerts == "off") return;
    const std::int64_t now = monoNs();
    if (now - last_alert_poll_ns_ < kNs) return;
    last_alert_poll_ns_ = now;
    for (const AlertLine& a : newAlerts(engine_.llsTables(), alerts_seen_)) {
        msg_Warn(d_, "atsc3: alert %s (%s%s%s): %s", a.id.c_str(), a.type.c_str(), a.priority ? ", priority " : "", a.priority ? std::to_string(*a.priority).c_str() : "",
                 a.text.c_str());
        if (opts_.alerts != "osd" || !anchored_ || selected_group_ < 0) continue;
        if (!alert_es_) {
            es_format_t fmt;
            es_format_Init(&fmt, SPU_ES, VLC_CODEC_TEXT);
            fmt.i_group = selected_group_;
            fmt.i_id = next_es_id_++;
            fmt.psz_description = strdup("ATSC alerts");
            alert_es_ = es_out_Add(d_->out, &fmt);
            es_format_Clean(&fmt);
        }
        if (!alert_es_) continue;
        const std::string text = "ALERT: " + a.text;
        block_t* b = block_Alloc(text.size() + 1);
        if (!b) continue;
        std::memcpy(b->p_buffer, text.c_str(), text.size() + 1);
        b->i_pts = b->i_dts = last_pcr_ != VLC_TICK_INVALID ? last_pcr_ + static_cast<vlc_tick_t>(live_delay_s_ * 1e6) : VLC_TICK_0;
        b->i_length = VLC_TICK_FROM_MS(15000);
        es_out_Send(d_->out, alert_es_, b);
    }
}

void Demux::logStats(const service::ServiceInfo& info) {
    const std::int64_t now = monoNs();
    if (now - last_stats_ns_ < 5 * kNs) return;
    last_stats_ns_ = now;
    std::string tracks;
    for (const auto& [k, t] : tracks_) {
        if (!tracks.empty()) tracks += ", ";
        tracks += t.kind + " " + std::to_string(t.fragments) + " frag/" + std::to_string(t.samples) + " samples";
    }
    const service::Status st = engine_.status();
    const double pos_s = static_cast<double>(st.position_ns) / 1e9;
    if (info.protocol == "MMTP") {
        msg_Dbg(d_, "atsc3: stats %s: %llu MMTP packets, %llu objects, %llu errors; %s; capture position %.1f s%s", nameOf(info).c_str(),
                static_cast<unsigned long long>(info.mmtp_packets), static_cast<unsigned long long>(info.objects), static_cast<unsigned long long>(info.object_errors),
                tracks.c_str(), pos_s, pressure_paused_ ? ", tuner held" : "");
    } else {
        msg_Dbg(d_, "atsc3: stats %s: %llu objects (%llu B), %llu object errors; %s; capture position %.1f s%s", nameOf(info).c_str(),
                static_cast<unsigned long long>(info.objects), static_cast<unsigned long long>(info.bytes), static_cast<unsigned long long>(info.object_errors), tracks.c_str(),
                pos_s, pressure_paused_ ? ", tuner held" : "");
    }
}

// ---- pf_demux / pf_control ----------------------------------------------------------------------------------------

int Demux::demux() {
    const std::int64_t call_t0 = monoNs();
    const service::Status st = engine_.status();
    const std::int64_t status_ms = (monoNs() - call_t0) / 1'000'000;
    if (!st.open) {
        msg_Err(d_, "atsc3: error: source closed (%s)", st.error.c_str());
        return VLC_DEMUXER_EGENERIC;
    }
    std::vector<service::ServiceInfo> services;
    const bool version_moved = st.version != last_version_;
    if (version_moved) {
        last_version_ = st.version;
        services = engine_.services();
        announcePrograms(services);
        if (!selected_) {
            if (const auto key = chooseInitial(services)) {
                const service::ServiceInfo* info = nullptr;
                for (const auto& s : services)
                    if (Key{s.bsid, s.service_id} == *key) info = &s;
                selectService(*key, info);
            }
        }
        pollEpg(services, false);
    }
    if (st.media_generation != media_generation_) {
        if (media_generation_ != 0 && anchored_) msg_Dbg(d_, "atsc3: media timelines restarted (loop wrap or reopen)");
        media_generation_ = st.media_generation;
        anchored_ = false;
    }
    std::size_t fed_before = 0;
    for (const auto& [k, t] : tracks_) fed_before += t.fragments;
    if (selected_) {
        const auto info = engine_.service(selected_->first, selected_->second);
        if (info) {
            if (info->info.protocol == "MMTP") feedMmt(info->info);
            else feedRoute(info->info);
            logStats(info->info);
        }
    }
    sendPcr();
    pollAlerts();
    std::size_t fed_after = 0;
    for (const auto& [k, t] : tracks_) fed_after += t.fragments;
    if (st.finished && !opts_.loop) {
        const std::int64_t now = monoNs();
        if (eof_since_ns_ == 0) eof_since_ns_ = now;
        if (fed_after == fed_before && now - eof_since_ns_ > 2 * kNs) {
            msg_Info(d_, "atsc3: end of capture");
            return VLC_DEMUXER_EOF;
        }
    }
    const std::int64_t call_ms = (monoNs() - call_t0) / 1'000'000;
    if (opts_.verbose_stats && call_ms > 200)
        msg_Dbg(d_, "atsc3: demux call took %lld ms (status() %lld ms, %zu fragments fed)", static_cast<long long>(call_ms), static_cast<long long>(status_ms), fed_after - fed_before);
    if (fed_after == fed_before) {
        ++idle_polls_;
        std::this_thread::sleep_for(std::chrono::milliseconds(st.paused ? 50 : 10));
    }
    return VLC_DEMUXER_SUCCESS;
}

int Demux::control(int query, va_list args) {
    switch (query) {
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_RATE:
        case DEMUX_CAN_RECORD:
            *va_arg(args, bool*) = false;
            return VLC_SUCCESS;
        case DEMUX_CAN_CONTROL_PACE:   // non-real-time: VLC consumes as fast as the stack delivers (dumps)
            *va_arg(args, bool*) = !opts_.realtime;
            return VLC_SUCCESS;
        case DEMUX_CAN_PAUSE:
            *va_arg(args, bool*) = true;
            return VLC_SUCCESS;
        case DEMUX_SET_PAUSE_STATE: {
            const bool paused = va_arg(args, int) != 0;
            user_paused_ = paused;
            engine_.pause(user_paused_ || pressure_paused_);
            if (!paused) {   // the capture clock stood still: VLC's clock jumps, re-anchor on the next data
                anchored_ = false;
            }
            msg_Dbg(d_, "atsc3: %s", paused ? "paused" : "resumed");
            return VLC_SUCCESS;
        }
        case DEMUX_GET_PTS_DELAY:
            *va_arg(args, vlc_tick_t*) = VLC_TICK_FROM_MS(std::max<std::int64_t>(var_InheritInteger(d_, "network-caching"), kPtsDelayMinMs));
            return VLC_SUCCESS;
        case DEMUX_GET_TIME: {
            const service::Status st = engine_.status();
            *va_arg(args, int64_t*) = st.position_ns / 1000;
            return VLC_SUCCESS;
        }
        case DEMUX_GET_LENGTH: {
            const service::Status st = engine_.status();
            if (st.duration_ns <= 0) return VLC_EGENERIC;
            *va_arg(args, int64_t*) = st.duration_ns / 1000;
            return VLC_SUCCESS;
        }
        case DEMUX_GET_POSITION: {
            const service::Status st = engine_.status();
            if (st.duration_ns <= 0) return VLC_EGENERIC;
            *va_arg(args, double*) = static_cast<double>(st.position_ns) / static_cast<double>(st.duration_ns);
            return VLC_SUCCESS;
        }
        case DEMUX_SET_GROUP: {
            const int group = va_arg(args, int);
            (void)va_arg(args, const vlc_list_t*);
            if (group < 0) return VLC_SUCCESS;   // "all" / none: keep the current program
            const auto it = group_to_service_.find(group);
            if (it == group_to_service_.end()) return VLC_EGENERIC;
            const service::ServiceInfo* info = nullptr;
            const auto services = engine_.services();
            for (const auto& s : services)
                if (Key{s.bsid, s.service_id} == it->second) info = &s;
            selectService(it->second, info);
            return VLC_SUCCESS;
        }
        case DEMUX_GET_META: {
            vlc_meta_t* meta = va_arg(args, vlc_meta_t*);
            const std::string name = mrl_.target.substr(mrl_.target.find_last_of('/') + 1);
            vlc_meta_Set(meta, vlc_meta_Title, name.c_str());
            vlc_meta_Set(meta, vlc_meta_Description, "ATSC 3.0 capture through the atsc3 receiver stack");
            return VLC_SUCCESS;
        }
        default:
            return VLC_EGENERIC;
    }
}

}  // namespace atsc3::vlcplugin
