#pragma once
// The atsc3:// access-demux (spec 5.19.2, 5.19.3): owns a service::Engine on the pcap virtual tuner, lists the
// services as VLC programs, feeds the selected service's components as elementary streams through the in-plugin
// CMAF/MPU path, keeps one timeline for all of them, pushes the ESG as EPG and logs alerts.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <vlc_common.h>
#include <vlc_demux.h>

// The 3.0 line gained these aliases after 3.0.16 (vlc_common.h / vlc_mtime.h in 3.0.24); an older 3.0.x SDK such as
// the distribution's libvlccore-dev 3.0.16 has only mtime_t, VLC_TS_* and CLOCK_FREQ. The plugin ABI is the same.
#ifndef VLC_TICK_INVALID
typedef mtime_t vlc_tick_t;
#define VLC_TICK_INVALID VLC_TS_INVALID
#define VLC_TICK_0 VLC_TS_0
#define VLC_TICK_FROM_MS(ms) (CLOCK_FREQ * (ms) / 1000)
#endif

#include "atsc3/service/engine.hpp"
#include "mrl.hpp"

namespace atsc3::vlcplugin {

class Demux {
public:
    Demux(demux_t* d, Mrl mrl, Options opts);
    ~Demux();
    Demux(const Demux&) = delete;
    Demux& operator=(const Demux&) = delete;

    /// Opens the source; false with `error` when the capture cannot be opened.
    bool open(std::string* error);
    /// pf_demux: VLC_DEMUXER_SUCCESS / VLC_DEMUXER_EOF / VLC_DEMUXER_EGENERIC.
    int demux();
    /// pf_control.
    int control(int query, va_list args);

private:
    struct Track {
        es_out_id_t* es = nullptr;
        std::string key;                 // ROUTE: Representation id; MMT: "mmt:<packet_id>"
        std::string kind;                // video | audio | subtitles
        std::string codecs;
        std::uint32_t track_id = 0;
        std::uint32_t timescale = 1;
        double timestamp_offset_s = 0.0; // MMT: SourceBuffer-style offset onto the service timeline
        bool discontinuity = true;       // the next block carries BLOCK_FLAG_DISCONTINUITY
        std::uint64_t samples = 0, bytes = 0, fragments = 0;
        std::optional<double> last_end_s;
    };

    using Key = std::pair<std::uint16_t, std::uint16_t>;   // (bsid, serviceId)

    int groupOf(const service::ServiceInfo& s) const;
    void announcePrograms(const std::vector<service::ServiceInfo>& services);
    std::optional<Key> chooseInitial(const std::vector<service::ServiceInfo>& services) const;
    void selectService(const Key& key, const service::ServiceInfo* info);
    void deselect();
    void dropTracks();
    void feedRoute(const service::ServiceInfo& info);
    void feedMmt(const service::ServiceInfo& info);
    Track* addTrack(const std::string& key, ByteView init, const std::string& lang, const std::string& desc, const std::string& kind_hint);
    void feedFragment(Track& t, ByteView fragment, std::optional<double> first_dts_s, bool discontinuity);
    vlc_tick_t tick(double media_s);
    void resyncTimeline(double media_s, const char* why);
    void sendPcr();
    void pollEpg(const std::vector<service::ServiceInfo>& services, bool force);
    void pollAlerts();
    void logStats(const service::ServiceInfo& info);
    bool programHasAv(const service::ServiceInfo& s) const;

    demux_t* d_;
    Mrl mrl_;
    Options opts_;
    service::Engine engine_;
    bool multi_bsid_ = false;
    std::set<int> announced_;
    std::map<int, Key> group_to_service_;
    std::optional<Key> selected_;
    int selected_group_ = -1;
    std::string selected_protocol_;
    std::map<std::string, Track> tracks_;
    int next_es_id_ = 1;
    std::uint64_t media_since_ = 0, mmt_since_ = 0, mmt_generation_ = 0, media_generation_ = 0;
    std::uint64_t last_version_ = ~std::uint64_t{0}, last_esg_version_ = ~std::uint64_t{0};
    // timeline (5.19.2 "Presentation time / clock")
    bool anchored_ = false;
    double anchor_media_s_ = 0.0;
    std::int64_t anchor_capture_ns_ = 0;
    vlc_tick_t last_pcr_ = VLC_TICK_INVALID;
    double live_delay_s_ = 1.5;
    double max_segment_s_ = 0.0;
    // pacing / control
    bool user_paused_ = false, pressure_paused_ = false;
    std::int64_t last_stats_ns_ = 0, last_alert_poll_ns_ = 0, last_epg_poll_ns_ = 0, eof_since_ns_ = 0;
    std::set<std::string> alerts_seen_;
    es_out_id_t* alert_es_ = nullptr;
    std::uint64_t idle_polls_ = 0;
};

}  // namespace atsc3::vlcplugin

/// VLC's per-instance private data.
struct demux_sys_t {
    atsc3::vlcplugin::Demux* impl = nullptr;
};
