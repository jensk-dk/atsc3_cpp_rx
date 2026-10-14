#pragma once
// ROUTE session: LCT channels and delivery object recovery (A/331 A.3.10; spec 5.8.2–5.8.5).
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/route/efdt.hpp"
#include "atsc3/route/lct.hpp"

namespace atsc3::route {

enum class Completion { TransferLength, FdtLength, CloseObject, Carousel, Timeout };
const char* toString(Completion c) noexcept;

/// Delivery mode derived from the codepoint / SrcFlow.Payload (A/331 Table A.3.2, A.3.6).
enum class FormatId : std::uint8_t { Unknown = 0, File = 1, Entity = 2, UnsignedPackage = 3, SignedPackage = 4 };

struct PayloadRule {       // SrcFlow.Payload (A/331 A.3.1: codePoint, formatId, frag, order, srcFecPayloadId)
    std::uint8_t codepoint = 128;
    FormatId format_id = FormatId::File;
    std::uint8_t frag = 0;
    bool order = true;
    std::uint8_t src_fec_payload_id = 0;
    std::optional<std::string> fec_params;
};

struct ChannelConfig {
    std::uint32_t tsi = 0;
    bool real_time = false;
    std::optional<std::uint32_t> bw;
    std::optional<Efdt> efdt;             // from the S-TSID (TOI 0 in-band EFDTs are picked up automatically)
    std::vector<PayloadRule> payloads;
    std::optional<std::string> rep_id;    // MediaInfo@repId
    std::optional<std::string> media_content_type;  // video / audio / subtitles / text
    std::optional<std::string> lang;
};

struct DeliveredObject {
    std::uint32_t tsi = 0, toi = 0;
    std::uint8_t codepoint = 0;
    FormatId format = FormatId::Unknown;
    Completion completion = Completion::TransferLength;
    std::vector<std::uint8_t> data;        // object bytes after content decoding (gzip inflated)
    std::uint64_t transfer_length = 0;     // bytes as transported
    bool content_decoded = false;
    // metadata from EFDT / entity headers
    std::optional<std::string> content_location, content_type, content_encoding, content_md5;
    std::optional<std::uint64_t> content_length;
    std::vector<std::string> app_context_id_list;
    bool from_template = false;
    std::optional<std::string> rep_id;     // channel's MediaInfo
    std::optional<std::string> media_content_type;
    std::optional<std::uint64_t> presentation_time_ntp;
    std::int64_t first_ns = 0, last_ns = 0;
    std::uint32_t packets = 0;
    bool close_object_seen = false;
};

struct ObjectError {
    std::uint32_t tsi = 0, toi = 0;
    std::string reason;
    std::uint64_t received = 0;
    std::optional<std::uint64_t> expected;
};

struct SessionCallbacks {
    std::function<void(std::shared_ptr<const DeliveredObject>)> onObject;
    std::function<void(std::uint32_t tsi, const Efdt&)> onEfdt;       // in-band EFDT (TOI 0) received/updated
    std::function<void(const ObjectError&)> onObjectError;
    /// Progressive delivery of contiguous prefixes (MDE mode) for real-time channels when enabled.
    std::function<void(std::uint32_t tsi, std::uint32_t toi, ByteView fragment, std::uint64_t offset)> onFragment;
};

struct SessionConfig {
    std::size_t max_object_bytes = 64u << 20;
    std::int64_t object_timeout_ns = 10'000'000'000;       // real-time channels
    std::int64_t nrt_object_timeout_ns = 60'000'000'000;   // non-real-time channels
    bool deliver_undescribed = true;                       // objects on TSIs with no ChannelConfig
    bool fragments = false;                                // enable onFragment
};

struct SessionStats {
    std::uint64_t packets = 0, dataless = 0, bad_lct = 0, bytes = 0, objects_complete = 0, objects_timeout = 0;
    std::uint64_t objects_dropped = 0, duplicates = 0, efdt_updates = 0, out_of_bounds = 0, decode_failed = 0;
};

class Session {
public:
    Session(SessionCallbacks cb, SessionConfig cfg = {});
    void setChannel(const ChannelConfig& cfg);
    void removeChannel(std::uint32_t tsi);
    std::optional<ChannelConfig> channel(std::uint32_t tsi) const;
    /// Feed a UDP datagram of this ROUTE session (any TSI).
    void onDatagram(const ip::UdpDatagram& d);
    /// Times out stale objects (also run on every datagram).
    void expire(std::int64_t now_ns);
    /// Current in-band or configured EFDT for a channel.
    const Efdt* efdt(std::uint32_t tsi) const;
    const SessionStats& stats() const noexcept { return stats_; }
    std::size_t openObjects() const noexcept { return objects_.size(); }
    /// An object still being received: bytes held so far (merged ranges) against the expected length when known
    /// (spec 5.18.4: the UI shows the progress of a slow package carousel).
    struct PendingObject {
        std::uint32_t tsi = 0, toi = 0;
        std::uint64_t received = 0;
        std::optional<std::uint64_t> expected;
        std::int64_t first_ns = 0, last_ns = 0;
    };
    std::vector<PendingObject> pending() const;
    void reset();

private:
    struct Partial {
        std::vector<std::uint8_t> data;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        std::optional<std::uint64_t> expected;
        std::uint64_t highest = 0;
        std::uint8_t codepoint = 0;
        bool close_seen = false;
        std::int64_t first_ns = 0, last_ns = 0;
        std::uint32_t packets = 0;
        std::optional<std::uint64_t> presentation_time_ntp;
        bool fragment_mode = false;
        std::uint64_t delivered_prefix = 0;
    };
    struct Channel {
        ChannelConfig cfg;
        std::optional<Efdt> inband_efdt;
        bool configured = false;
    };
    using Key = std::uint64_t;
    static Key key(std::uint32_t tsi, std::uint32_t toi) { return (static_cast<Key>(tsi) << 32) | toi; }

    void complete(Key k, Partial& p, std::uint32_t tsi, std::uint32_t toi, Completion how);
    void fail(Key k, const Partial& p, std::uint32_t tsi, std::uint32_t toi, const std::string& reason);
    bool contiguous(const Partial& p, std::uint64_t upto) const;
    FormatId formatFor(std::uint32_t tsi, std::uint8_t codepoint) const;
    void applyMetadata(DeliveredObject& obj, const Channel* ch);
    void handleInbandEfdt(std::uint32_t tsi, const std::vector<std::uint8_t>& data);

    SessionCallbacks cb_;
    SessionConfig cfg_;
    SessionStats stats_;
    std::map<std::uint32_t, Channel> channels_;
    std::map<Key, Partial> objects_;
};

}  // namespace atsc3::route
