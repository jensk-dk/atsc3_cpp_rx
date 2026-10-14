#pragma once
// Service engine (spec 5.11): runs the whole receive chain on a tuner (service::ITuner, coupled at the layer the
// tuner delivers: BBP, ALP or IP), keeps the
// service table from every SLT, acquires the SLS of every ROUTE and MMTP service so that components
// (tracks, codecs) are known, lets services be selected (media/NRT sessions opened), and exposes
// thread-safe snapshots for the web server and other front ends. All callbacks run on the tuner thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atsc3/alp/decap.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/link/signaling.hpp"
#include "atsc3/lls/processor.hpp"
#include "atsc3/mmtp/session.hpp"
#include "atsc3/phy/bbp.hpp"
#include "atsc3/service/esg.hpp"
#include "atsc3/service/tuner.hpp"
#include "atsc3/sls/route_receiver.hpp"

namespace atsc3::service {

struct ComponentInfo {
    std::string kind;            // video, audio, subtitles, text, application, nrt, data, unknown
    std::string transport;       // "ROUTE" or "MMTP"
    std::optional<std::uint32_t> tsi;
    std::optional<std::uint16_t> packet_id;
    std::string id;              // Representation@id or MMT asset id
    std::string codecs, mime_type, lang, name;
    std::vector<std::string> roles;
    std::optional<std::uint64_t> bandwidth;
    std::optional<std::uint32_t> width, height, timescale, audio_sampling_rate;
    std::optional<std::string> frame_rate;
    bool content_protected = false;
    bool default_asset = false;
    bool real_time = false;
    std::string session;         // "dst:port"
    std::uint64_t objects = 0, bytes = 0, packets = 0, errors = 0;
    std::optional<std::uint32_t> last_toi, last_mpu_sequence;
    std::optional<std::string> last_name;
};

struct SlsFragmentInfo {
    std::string kind, content_type, location;
    std::optional<std::uint32_t> version;
    std::size_t bytes = 0;
};

/// A broadcaster application signalled for a service by a HELD (ROUTE SLS fragment or MMT `mmt_atsc3_message`
/// 0x0003), one per HTMLEntryPackage, with the state of its Application Context Cache (spec 5.18.4, A/344 6.2).
struct AppInfo {
    std::string app_context_id, app_id;
    std::optional<std::string> bcast_entry_package_url, bcast_entry_page_url, bband_entry_page_url, required_capabilities;
    bool app_rendering = false;
    std::vector<std::uint16_t> coupled_services;
    std::vector<std::uint32_t> lct_tsi_ref;
    std::optional<std::int64_t> valid_from_ns, valid_until_ns;
    bool ready = false;                 // the broadcast entry page is in the cache (its package arrived complete)
    std::size_t files = 0, bytes = 0;   // cached files tagged with this appContextId (or untagged)
    // the entry package on the carousel: its channel, declared size (EFDT Content-Length) and the bytes received so
    // far of the copy being assembled (a slow package shows progress instead of silence)
    std::optional<std::uint32_t> package_tsi;
    std::optional<std::uint64_t> package_bytes, package_received;
};

struct ServiceInfo {
    std::uint8_t lls_group = 0;
    std::uint16_t bsid = 0, service_id = 0;
    std::optional<std::string> global_service_id;
    std::optional<std::uint16_t> major, minor;
    std::string short_name, usbd_name;
    std::uint8_t category = 0;
    std::string category_name;
    std::string protocol;        // ROUTE, MMTP, broadband, none
    bool protected_flag = false, hidden = false, hide_in_guide = false, broadband_required = false, essential = false;
    std::optional<std::string> drm_system_id;
    std::string sls_address;
    bool sls_received = false;
    std::uint64_t sls_updates = 0;
    std::uint32_t sls_toi = 0;
    bool selected = false;
    std::vector<std::string> languages;
    std::vector<ComponentInfo> components;
    std::vector<SlsFragmentInfo> sls_fragments;
    std::string mmt_package_id;
    std::vector<AppInfo> apps;    // HELD HTMLEntryPackages (empty without a HELD)
    std::vector<std::string> warnings;
    std::uint64_t objects = 0, object_errors = 0, bytes = 0, mmtp_packets = 0;
    // MPD summary
    std::string mpd_profiles, mpd_type;
    std::optional<double> mpd_min_buffer_s, mpd_max_segment_s, mpd_time_shift_s;
    std::optional<std::int64_t> mpd_publish_ns, mpd_availability_start_ns;
};

struct ServiceDetail {
    ServiceInfo info;
    std::vector<std::pair<std::string, std::string>> fragments;   // kind -> text (SLS parts, MMT USBD/HELD)
    std::vector<std::string> recent_objects;                      // "tsi/toi name bytes"
};

/// Media segment store of a selected ROUTE service (spec 5.17.3): init segments pinned, media segments in a
/// bounded ring keyed by Content-Location, classified against the MPD (Representation, $Number$/$Time$ key).
struct MediaSegmentInfo {
    std::uint64_t seq = 0;             // arrival order within the service (monotonic, for ?since= polling)
    std::string name, rep_id;
    bool is_init = false;
    std::uint64_t key = 0;             // $Number$ or $Time$
    std::size_t bytes = 0;
    std::uint32_t tsi = 0, toi = 0;
    std::int64_t received_ns = 0;
    std::string content_type;          // video/mp4, audio/mp4, application/mp4
    std::optional<std::uint64_t> tfdt;           // media segments: baseMediaDecodeTime of the first traf (track timescale)
    std::optional<std::uint32_t> track_timescale; // init segments: mdhd timescale of the media track
};
struct MediaRepresentationInfo {
    std::string id, kind, codecs, mime_type, lang;
    std::optional<std::uint64_t> bandwidth;
    std::optional<std::uint32_t> width, height, timescale;
    std::optional<std::uint64_t> segment_duration;   // SegmentTemplate@duration (in timescale units)
    std::uint64_t start_number = 1;    // SegmentTemplate@startNumber as signalled
    /// $Number$ calibration (spec 5.17.3): the signalled numbering is off by `number_shift` segments against the media
    /// timeline (tfdt) of the newest cached segment, so the origin MPD and the service clock use
    /// `effective_start_number` = start_number - number_shift. 0 / start_number when the numbering is consistent or
    /// nothing is cached yet.
    std::int64_t number_shift = 0;
    std::uint64_t effective_start_number = 1;
    std::string addressing;            // "number", "time" or "none"
    std::string init_name;             // resolved initialization template
    std::string media_template;
    bool content_protected = false;
    bool init = false;                 // init segment cached
    std::size_t cached_segments = 0;
    std::optional<std::uint64_t> first_key, last_key;
};
struct MediaIndex {
    std::uint16_t bsid = 0, service_id = 0;
    bool selected = false;
    std::uint64_t next_seq = 0;
    std::vector<MediaRepresentationInfo> representations;
    std::vector<MediaSegmentInfo> segments;   // seq > since, arrival order
    std::size_t cached_bytes = 0, cache_limit = 0;
    std::uint64_t evicted = 0;
    std::optional<double> max_segment_duration_s, time_shift_buffer_depth_s;
    std::optional<std::int64_t> availability_start_ns;
    double period_start_s = 0.0;
    std::string mpd_type;
    /// Wall-clock instant (Unix ms) at which the newest cached segment becomes available per the MPD, i.e. the
    /// "now" a DASH player must assume so that its live edge lies inside the cache (spec 5.17.3). nullopt
    /// without segments, an availabilityStartTime or $Number$/$Time$ addressing.
    std::optional<std::int64_t> live_edge_unix_ms;
};

/// MMT media store of a selected MMTP service (spec 5.17.4, HTTP variant): per packet_id the latest MSE init segment
/// and the reconstructed movie fragments, in a bounded ring shared by all components of the service.
struct MmtComponentInfo {
    std::uint16_t packet_id = 0;
    std::string asset_id, asset_type;           // MP table (asset_type is not trusted for codecs)
    bool default_asset = false;
    std::string kind, sample_entry, codecs, mime, lang;   // from the init segment's media track
    std::uint32_t timescale = 0;
    std::optional<std::uint16_t> width, height, channels;
    std::optional<std::uint32_t> sample_rate;
    std::optional<std::uint64_t> init_seq;
    std::size_t init_bytes = 0;
    std::optional<double> timestamp_offset_s;   // constant SourceBuffer.timestampOffset for this component
    std::string timeline_mode;                  // "keep", "rewrite", "undecided"
    std::size_t fragments = 0;
    std::optional<std::uint32_t> last_mpu;
    std::optional<double> last_end_s;           // end of the newest fragment on the service timeline
    std::uint64_t damaged = 0, dropped = 0, mpu_gaps = 0;
};
struct MmtFragmentInfo {
    std::uint64_t seq = 0;
    std::uint16_t packet_id = 0;
    std::uint32_t mpu = 0;
    std::size_t bytes = 0;
    double start_s = 0.0, duration_s = 0.0;     // on the service timeline (timestamp offset applied)
    bool damaged = false, discontinuity = false, tfdt_rewritten = false;
    std::optional<std::int64_t> presentation_unix_ms;
};
struct MmtIndex {
    std::uint16_t bsid = 0, service_id = 0;
    bool selected = false;
    std::uint64_t generation = 0;   // changes when the origin's timeline restarted (source reopen, loop wrap, encoder restart)
    std::uint64_t next_seq = 0;
    std::optional<std::uint64_t> anchor_ntp;
    std::optional<std::int64_t> anchor_unix_ms;
    std::vector<MmtComponentInfo> components;
    std::vector<MmtFragmentInfo> fragments;     // seq > since, arrival order
    std::optional<double> live_edge_s;
    std::size_t cached_bytes = 0, cache_limit = 0;
    std::uint64_t evicted = 0;
};

/// Application Context Cache of a selected service (spec 5.18.4): NRT files and the unpacked parts of
/// `multipart/related` packages, keyed by Content-Location, served by the web server under /apps/{bsid}/{sid}/.
struct AppFileInfo {
    std::string path, content_type;     // path = Content-Location (package parts: the part's, relative to the cache root)
    std::size_t bytes = 0;
    std::optional<std::uint32_t> version;   // metadataEnvelope item version of a package part
    std::string package;                // Content-Location of the package the part came from (empty for plain files)
    std::vector<std::string> app_context_ids;
    std::uint32_t tsi = 0, toi = 0;
    std::int64_t received_ns = 0;
};
struct AppFile {
    std::string content_type;
    std::shared_ptr<const route::DeliveredObject> owner;   // keeps the bytes alive
    std::size_t offset = 0, size = 0;
    const std::uint8_t* data() const { return owner ? owner->data.data() + offset : nullptr; }
};
struct AppIndex {
    std::uint16_t bsid = 0, service_id = 0;
    bool selected = false;
    std::vector<AppInfo> apps;
    std::vector<AppFileInfo> files;
    std::size_t cached_bytes = 0, cache_limit = 0;
    std::uint64_t objects = 0, packages = 0, rejected = 0;   // NRT objects seen, packages unpacked, objects refused (cache full / bad package)
};

struct LlsTableInfo {
    std::uint8_t group = 0, table_id = 0, version = 0;
    std::string name, source_ip, root_namespace, xml;
    std::size_t bytes = 0;
    std::int64_t t_ns = 0;
};

struct LinkInfo {
    std::vector<std::pair<std::uint8_t, link::Lmt>> lmts;   // carrier PLP -> LMT
    std::vector<link::PlpMap::Entry> plp_map;
    std::optional<io::L1Info> l1;
    std::vector<ip::UnroutedFlow> unrouted;
};

/// ESG delivery state (spec 5.11 item 4): the fragments split out of every SGDU, the SGDDs and the NRT files
/// (icons) of the ESG services. Fragments are kept per (type, id) at their latest version.
struct EsgDescriptorInfo {
    Sgdd sgdd;
    std::uint16_t bsid = 0, service_id = 0;
    std::uint32_t tsi = 0, toi = 0;
    std::string location;
    std::int64_t received_ns = 0;
};
struct EsgFileInfo {
    std::string name, content_type;
    std::size_t bytes = 0;
    std::uint16_t bsid = 0, service_id = 0;
    std::uint32_t tsi = 0, toi = 0;
    std::int64_t received_ns = 0;
};
struct EsgFile {
    std::string content_type;
    std::vector<std::uint8_t> bytes;
};
struct EsgStats {
    std::uint64_t objects = 0, sgdd = 0, sgdu = 0, files = 0, ignored = 0, parse_errors = 0;
    std::uint64_t fragments_seen = 0, fragments_new = 0, fragments_updated = 0, fragments_duplicate = 0;
    std::size_t file_bytes = 0;
};
struct EsgSnapshot {
    std::uint64_t version = 0;          // Status::esg_version at snapshot time
    EsgStats stats;
    std::vector<EsgFragment> fragments;
    std::vector<EsgDescriptorInfo> descriptors;
    std::vector<EsgFileInfo> files;
};

struct Status {
    bool open = false;
    std::string tuner;                        // the tuner implementation ("pcap", "sony")
    std::string source, error;                // what the tuner is tuned to (capture path, frequency) / why open failed
    TunerOutput output = TunerOutput::Ip;     // the layer the tuner is coupled at
    std::string detection;                    // the tuner's FormatDetected message
    bool paused = false, finished = false;
    std::int64_t position_ns = 0, duration_ns = 0;   // replay position/length from the tuner (0 for RF)
    SignalStats signal;
    std::int64_t opened_at_unix_ms = 0;
    std::vector<std::uint8_t> lls_groups;
    std::vector<std::uint16_t> bsids;
    std::size_t services = 0, selected = 0;
    std::uint64_t misrouted_units = 0;   // units the tuner delivered for a layer the chain was not built for
    phy::DeframerStats deframer;
    alp::Stats decap;
    ip::DemuxStats demux;
    lls::ProcessorStats lls;
    link::LinkSignaling::Stats link;
    std::uint64_t version = 0;   // increments on every state change (service list, SLS, selection)
    std::uint64_t esg_version = 0;   // increments when an ESG fragment, descriptor or file is new or changed
    std::uint64_t media_generation = 1;   // increments on open and on every loop wrap (media timelines start over)
    std::int64_t capture_unix_ns = 0;   // capture timestamp of the last delivered unit (0 when unknown)
};

struct OpenOptions {
    std::optional<TunerOutput> output;   // coupling layer; default: the highest layer the tuner offers (Ip, then Alp, then Bbp)
    bool auto_open_esg = true;      // open the NRT sessions of ESG services (serviceCategory 4) without selecting them
    std::size_t media_cache_bytes = 96u << 20;   // per selected service; init segments do not count
    std::size_t app_cache_bytes = 64u << 20;     // Application Context Cache per selected service (spec 5.18.4)
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Takes the tuner, couples it to the chain at one of the outputs it offers and starts delivery. Returns false (and
    /// sets the status error) when the tuner is not ready, offers no usable output or cannot start. The engine keeps the
    /// tuner until close(); tuner() hands it back for implementation-specific controls (replay pacing, position, …).
    bool open(std::shared_ptr<ITuner> tuner, const OpenOptions& opts, std::string* error = nullptr);
    void close();
    /// Forwards to the tuner; false when it cannot pause (RF tuners).
    bool pause(bool paused);
    std::shared_ptr<ITuner> tuner() const;

    bool select(std::uint16_t bsid, std::uint16_t service_id, bool selected);
    std::set<std::pair<std::uint16_t, std::uint16_t>> selection() const;

    Status status() const;
    std::vector<ServiceInfo> services() const;
    std::optional<ServiceDetail> service(std::uint16_t bsid, std::uint16_t service_id) const;
    std::vector<LlsTableInfo> llsTables() const;
    LinkInfo link() const;
    EsgSnapshot esg() const;
    std::uint64_t esgVersion() const;
    /// An NRT file (icon) of an ESG service by Content-Location.
    std::optional<EsgFile> esgFile(const std::string& name) const;

    /// Segment store of a (selected) ROUTE service; segments with seq > `since`. nullopt for unknown services.
    std::optional<MediaIndex> media(std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since = 0) const;
    /// A cached init or media segment by Content-Location (shared, no copy).
    std::shared_ptr<const route::DeliveredObject> mediaObject(std::uint16_t bsid, std::uint16_t service_id, const std::string& name) const;
    /// The on-air MPD rewritten for the local origin (sls::makeOriginMpd); nullopt without an MPD.
    std::optional<std::string> originMpd(std::uint16_t bsid, std::uint16_t service_id, const std::string& base_url, const std::string& time_url,
                                         const std::string& location) const;
    /// A static MPD over the cached segments (sls::makeStaticMpd), for offline players.
    std::optional<std::string> staticMpd(std::uint16_t bsid, std::uint16_t service_id) const;

    /// MMT media store of an MMTP service (fragments with seq > since). nullopt for unknown or non-MMTP services.
    std::optional<MmtIndex> mmtMedia(std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since = 0) const;
    std::shared_ptr<const mmtp::MpuInitSegment> mmtInit(std::uint16_t bsid, std::uint16_t service_id, std::uint16_t packet_id) const;
    std::shared_ptr<const mmtp::MpuFragment> mmtFragment(std::uint16_t bsid, std::uint16_t service_id, std::uint16_t packet_id, std::uint32_t mpu) const;
    /// Application Context Cache of a service (spec 5.18.4): the HELD apps with their readiness and the cached files.
    /// nullopt for unknown services.
    std::optional<AppIndex> apps(std::uint16_t bsid, std::uint16_t service_id) const;
    /// A cached application file by path (Content-Location); nullopt when absent.
    std::optional<AppFile> appFile(std::uint16_t bsid, std::uint16_t service_id, const std::string& path) const;

    /// Returns when the status version changes or the timeout passes (long-poll support).
    std::uint64_t waitVersion(std::uint64_t since, std::chrono::milliseconds timeout) const;

private:
    struct Chain;   // the per-capture receive chain (tuner, deframer, decap, demux, LLS, services)
    struct Service;
    class Sink;
    struct EsgStore;
    void onSlt(const lls::TableInfo& info, const lls::Slt& slt);
    void onEsgObject(Service& s, const route::DeliveredObject& o);
    void onMediaObject(Service& s, std::shared_ptr<const route::DeliveredObject> o);
    void onRouteObject(Service& s, std::shared_ptr<const route::DeliveredObject> o);
    void onAppObject(Service& s, std::shared_ptr<const route::DeliveredObject> o);
    void openMmtRouteComponent(Service& s);
    std::vector<AppInfo> buildApps(const Service& s) const;
    MediaIndex buildMediaIndex(const Service& s, std::uint16_t bsid, std::uint16_t service_id, std::uint64_t since) const;
    void onMmtInit(Service& s, std::shared_ptr<const mmtp::MpuInitSegment> i);
    void onMmtFragment(Service& s, std::shared_ptr<const mmtp::MpuFragment> f);
    void onMmtTable(Service& s, const mmtp::MpTable& t);
    void onWrap();
    void onEndOfStream();
    const Service* findService(std::uint16_t bsid, std::uint16_t service_id) const;
    bool sessionsWanted(const Service& s) const;
    void bump();
    ServiceInfo buildInfo(const Service& s) const;
    static std::string categoryName(std::uint8_t c);

    /// API calls announce themselves while they wait for mu_. The delivery thread takes and releases mu_ once per
    /// packet; in non-real-time replay it would otherwise re-take it before a waiting caller runs and starve every
    /// API call (a VLC demux fetching one segment per second). It pauses briefly while someone is waiting.
    struct ApiLock {
        explicit ApiLock(const Engine& e) : eng(e) {
            eng.api_waiters_.fetch_add(1, std::memory_order_relaxed);
            lock = std::unique_lock<std::mutex>(eng.mu_);
            eng.api_waiters_.fetch_sub(1, std::memory_order_relaxed);
        }
        const Engine& eng;
        std::unique_lock<std::mutex> lock;
    };
    void yieldToApi() const;

    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    mutable std::atomic<unsigned> api_waiters_{0};
    std::unique_ptr<Chain> chain_;
    std::set<std::pair<std::uint16_t, std::uint16_t>> selection_;
    Status status_;
};

}  // namespace atsc3::service
