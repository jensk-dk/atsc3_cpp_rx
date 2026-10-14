#pragma once
// LLS tables (A/331 §6, schemas in specs/schemas/lls; spec 5.7).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atsc3::lls {

enum TableId : std::uint8_t {
    kSlt = 0x01,
    kRrt = 0x02,
    kSystemTime = 0x03,
    kAeat = 0x04,
    kOmn = 0x05,
    kCdt = 0x06,
    kDrct = 0x07,
    kSignedMultiTable = 0xFE,
    kUserDefined = 0xFF
};
const char* tableName(std::uint8_t id) noexcept;

struct TableHeader {
    std::uint8_t table_id = 0;
    std::uint8_t group_id = 0;
    std::uint8_t group_count_minus1 = 0;
    std::uint8_t version = 0;
};

/// Where a table came from.
struct TableInfo {
    TableHeader header;
    std::uint32_t source_ip = 0;
    std::uint8_t plp = 0xFF;
    std::int64_t t_ns = 0;
    bool in_signed_multi_table = false;
    std::uint8_t multi_table_group = 0;     // the SignedMultiTable's own group id
    std::string root_namespace;             // as declared on the XML root
    std::string raw_xml;                    // decompressed table text (XML tables)
};

// ---- SLT (SLT-1.0-20211209.xsd) ----
struct InetUrl {
    std::string url;
    std::uint8_t url_type = 0;  // 1 SLS signaling server, 2 ESG, 3 service usage, 4 DRM license
};
struct SimulcastTsid {
    std::uint16_t tsid = 0;
    std::optional<std::uint16_t> major, minor;
};
struct BroadcastSvcSignaling {
    std::uint8_t sls_protocol = 0;           // 1 ROUTE, 2 MMTP
    std::uint8_t sls_major_version = 1;
    std::uint8_t sls_minor_version = 0;
    std::uint32_t sls_destination_ip = 0;    // host order
    std::uint16_t sls_destination_port = 0;
    std::uint32_t sls_source_ip = 0;         // 0 = absent
};
struct OtherBsid {
    std::vector<std::uint16_t> bsids;
    std::uint8_t type = 0;  // 1 duplicate, 2 portion, 3 ...
};
struct Directional {
    std::uint32_t heading = 0;
    double strength = 0.0;
    std::optional<std::int64_t> haat;
};
struct OtherRf {
    std::uint16_t other_bsid_rf = 0, other_bsid = 0;
    double lat = 0.0, lon = 0.0;
    std::int64_t elev = 0, erp = 0;
    std::vector<Directional> directional;
};
struct SltService {
    std::uint16_t service_id = 0;
    std::optional<std::string> global_service_id;
    std::uint8_t slt_svc_seq_num = 0;
    bool protected_flag = false;
    std::optional<std::uint16_t> major_channel_no, minor_channel_no;
    std::uint8_t service_category = 0;  // 1 A/V, 2 audio, 3 app, 4 ESG, 5 EA, 6 DRM data
    std::optional<std::string> short_service_name;
    bool hidden = false, hide_in_guide = false, broadband_access_required = false;
    std::optional<bool> essential;
    std::optional<std::string> drm_system_id;
    enum class Configuration { Broadcast, Broadband } configuration = Configuration::Broadcast;
    std::optional<SimulcastTsid> simulcast;
    std::optional<std::string> svc_capabilities;
    std::optional<BroadcastSvcSignaling> bss;
    std::vector<InetUrl> svc_inet_url;
    std::vector<OtherBsid> other_bsid;
    std::vector<OtherRf> other_rf;
    std::string raw_xml;
};
struct Slt {
    std::vector<std::uint16_t> bsid;
    std::optional<std::string> slt_capabilities;
    std::vector<InetUrl> slt_inet_url;
    std::vector<SltService> services;
};

// ---- SystemTime (SYSTIME-1.0-20211208.xsd) ----
struct SystemTime {
    std::int16_t current_utc_offset = 0;   // TAI - UTC seconds
    std::uint16_t ptp_prepend = 0;
    bool leap59 = false, leap61 = false;
    std::optional<double> utc_local_offset_s;   // xs:duration
    bool ds_status = false;
    std::optional<std::uint8_t> ds_day_of_month, ds_hour;
};

// ---- AEAT (AEAT-1.0-20210322.xsd) ----
struct LangText {
    std::string lang;
    std::string text;
};
struct AeaLocation {
    std::string type;  // FIPS, SGC, polygon, circle
    std::string value;
};
struct AeaLiveMedia {
    std::vector<std::uint16_t> bsid;
    std::uint16_t service_id = 0;
    std::vector<LangText> service_name;
};
struct AeaMedia {
    std::optional<std::string> lang, media_desc, media_type, alternate_url, content_type, media_assoc;
    std::string url;
    std::optional<std::uint64_t> content_length;
};
struct Aea {
    std::string aea_id, issuer, audience, aea_type;  // audience: public/restricted/private; type: alert/update/cancel
    std::vector<std::string> sub_audience;
    std::optional<std::string> ref_aea_id, category;
    std::optional<std::uint8_t> priority;
    bool wakeup = false;
    std::optional<std::int64_t> effective_ns, expires_ns;
    std::optional<std::string> event_code, event_code_type;
    std::vector<LangText> event_desc, aea_text;
    std::vector<AeaLocation> locations;
    std::optional<AeaLiveMedia> live_media;
    std::vector<AeaMedia> media;
    std::string raw_xml;
};
struct Aeat {
    std::optional<std::string> aea_table_id;
    std::vector<Aea> aeas;
};

// ---- RRT (RRT-1.0-20210128.xsd) ----
struct RatingValue {
    std::uint8_t rating_value = 0;
    std::vector<LangText> abbrev, text;
};
struct RatingDimension {
    std::uint8_t index = 0, levels = 0;
    bool graduated = false;
    std::vector<LangText> title;
    std::vector<RatingValue> ratings;
};
struct RatingRegionTable {
    std::uint8_t region_identifier = 0;
    std::vector<LangText> region_id_text;
    std::vector<RatingDimension> dimensions;
};
struct Rrt {
    std::vector<RatingRegionTable> tables;
};

// ---- OnScreenMessageNotification (ONSCREEN-1.0-20210401.xsd) ----
struct KeepScreenClear {
    std::vector<std::uint16_t> bsid;
    std::optional<std::uint16_t> service_id;
    std::uint16_t service_id_range = 0;
    double notification_duration_s = 60.0;
    bool ksc_flag = true;
    std::uint8_t version = 0;
};
struct Omn {
    std::vector<KeepScreenClear> entries;
};

// ---- SignedMultiTable (A/331 Table 6.16) ----
struct SignedMultiTableEntry {
    std::uint8_t table_id = 0;
    std::uint8_t version = 0;
    std::uint16_t length = 0;
};
struct SignedMultiTable {
    std::vector<SignedMultiTableEntry> entries;
    std::uint16_t signature_length = 0;
    std::vector<std::uint8_t> signature;  // CMS SignedData (A/360)
};

/// Parsers (XML tables take the decompressed text). They never throw; false = not parseable.
bool parseSlt(const std::string& xml, Slt& out, std::string* error = nullptr);
bool parseSystemTime(const std::string& xml, SystemTime& out, std::string* error = nullptr);
bool parseAeat(const std::string& xml, Aeat& out, std::string* error = nullptr);
bool parseRrt(const std::string& xml, Rrt& out, std::string* error = nullptr);
bool parseOmn(const std::string& xml, Omn& out, std::string* error = nullptr);

}  // namespace atsc3::lls
