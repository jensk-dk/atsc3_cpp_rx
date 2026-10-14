#pragma once
// ROUTE SLS fragments (A/331 §7.1, specs/schemas/sls/route, specs/schemas/sls/application; spec 5.9.2):
// metadataEnvelope, USBD (BundleDescriptionROUTE), S-TSID, HELD. The MPD lives in mpd.hpp.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/route/efdt.hpp"
#include "atsc3/route/session.hpp"

namespace atsc3::sls {

struct NamedText {
    std::string lang;
    std::string text;
};

// ---- metadataEnvelope (3GPP TS 26.346 + ATSC extensions) ----
struct EnvelopeItem {
    std::string metadata_uri;
    std::string content_type;   // lower-case, no parameters
    std::optional<std::uint32_t> version;
    std::optional<std::int64_t> valid_from_ns, valid_until_ns, avail_at_ns;
    std::optional<std::string> next_url;
};
struct Envelope {
    std::vector<EnvelopeItem> items;
    std::string raw_xml;
    const EnvelopeItem* find(const std::string& metadata_uri) const;
};
bool parseEnvelope(const std::string& xml, Envelope& out, std::string* error = nullptr);

// ---- USBD (ROUTEUSD-1.0-20170920.xsd) ----
struct UsbdDeliveryMethod {
    std::vector<std::string> broadcast_base_patterns;
    std::vector<std::string> unicast_base_patterns;
};
struct Usbd {
    std::uint16_t service_id = 0;
    std::optional<bool> service_status;
    std::vector<NamedText> names;
    std::vector<std::string> service_languages;
    std::vector<UsbdDeliveryMethod> delivery_methods;
    std::string raw_xml;
};
bool parseUsbd(const std::string& xml, Usbd& out, std::string* error = nullptr);

// ---- S-TSID (S-TSID-1.0-20220708.xsd) ----
struct MediaInfo {
    std::optional<std::string> rep_id;
    std::optional<std::string> content_type;  // video, audio, subtitles (and non-schema "text" on air)
    std::optional<std::string> lang;
    bool startup = false;
    std::vector<std::string> content_ratings;
};
struct SrcFlow {
    bool rt = false;
    std::optional<std::uint32_t> min_buff_size;
    std::optional<route::Efdt> efdt;
    std::optional<MediaInfo> media_info;
    bool has_aea_media = false;
    std::vector<route::PayloadRule> payloads;
    std::string raw_xml;
};
struct ProtectedObject {
    std::optional<std::string> session_description;
    std::uint32_t tsi = 0;
    std::optional<std::uint32_t> source_toi_x, source_toi_y;
    std::optional<std::string> fec_transport_object_size;
};
struct RepairFlow {
    std::optional<std::uint32_t> maximum_delay, overhead, min_buff_size;
    std::optional<std::string> fec_oti;
    std::vector<ProtectedObject> protected_objects;
    std::string raw_xml;
};
struct Ls {
    std::uint32_t tsi = 0;
    std::optional<std::uint32_t> bw;
    std::optional<std::int64_t> start_time_ns, end_time_ns;
    std::optional<SrcFlow> src_flow;
    std::optional<RepairFlow> repair_flow;
};
struct Rs {
    std::optional<std::uint32_t> src_ip, dst_ip;   // host order; absent = the SLS session's
    std::optional<std::uint16_t> dst_port;
    std::vector<Ls> ls;
};
struct Stsid {
    std::vector<Rs> rs;
    std::string raw_xml;
};
bool parseStsid(const std::string& xml, Stsid& out, std::string* error = nullptr);
/// ChannelConfig for a ROUTE session from an LS element.
route::ChannelConfig channelConfig(const Ls& ls);

// ---- HELD (HELD-1.0) ----
struct HtmlEntryPackage {
    std::string app_context_id;
    std::optional<std::string> app_id;
    std::optional<std::string> required_capabilities, app_rendering, clear_app_context_cache_date;
    std::optional<std::string> bcast_entry_package_url, bcast_entry_page_url, bband_entry_page_url;
    std::optional<std::int64_t> valid_from_ns, valid_until_ns;
    std::vector<std::uint16_t> coupled_services;
    std::vector<std::uint32_t> lct_tsi_ref;
    std::string raw_xml;
};
struct Held {
    std::vector<HtmlEntryPackage> packages;
    std::string raw_xml;
};
bool parseHeld(const std::string& xml, Held& out, std::string* error = nullptr);

}  // namespace atsc3::sls
