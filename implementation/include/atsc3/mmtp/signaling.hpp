#pragma once
// MMT signaling messages (ISO/IEC 23008-1 §10, A/331 §7.2; spec 5.10.4): PA, MP table (complete and
// subset), mmt_atsc3_message, MMT USBD. Session-level reassembly lives in session.hpp.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::mmtp {

struct AssetLocation {
    std::uint8_t type = 0;      // 0 same flow, 1 IPv4 flow, 2 IPv6, 3+ other
    std::uint16_t packet_id = 0;
    std::uint32_t src_ip = 0, dst_ip = 0;  // type 1
    std::uint16_t dst_port = 0;
};
struct MpuTimestamp {
    std::uint32_t mpu_sequence_number = 0;
    std::uint64_t ntp = 0;
};
struct Asset {
    std::uint8_t identifier_type = 0;
    std::uint32_t asset_id_scheme = 0;
    std::vector<std::uint8_t> asset_id;
    std::string assetIdText() const;      // printable id or hex
    std::string asset_type;               // 4CC, e.g. "hev1", "ac-4", "stpp"
    bool default_asset = false;
    std::optional<std::uint8_t> clock_relation_id;
    std::optional<std::uint32_t> timescale;
    std::vector<AssetLocation> locations;
    std::vector<MpuTimestamp> mpu_timestamps;  // MPU_timestamp_descriptor (tag 0x0001)
    std::vector<std::uint16_t> descriptor_tags;
    std::optional<std::uint16_t> packetId() const;  // first type-0 location
};
struct MpTable {
    std::uint8_t table_id = 0;      // 0x11 / 0x20 complete, 0x12..0x1F subset
    std::uint8_t version = 0;
    std::uint8_t mode = 0;
    std::string package_id;
    std::vector<Asset> assets;
    bool complete() const noexcept { return table_id == 0x11 || table_id == 0x20; }
};

/// A/331 Table 7.9.
struct Atsc3Message {
    std::uint16_t service_id = 0;
    std::uint16_t content_type = 0;   // 0x0001 USBD, 0x0002 MPD, 0x0003 HELD, 0x0005 VSPD, 0x0008 CAD, 0x0009 ASPD, ...
    std::uint8_t content_version = 0;
    std::uint8_t compression = 1;     // 1 none, 2 gzip
    std::string uri;
    std::vector<std::uint8_t> content;  // inflated when gzip
    bool inflated = false;
};
const char* atsc3ContentTypeName(std::uint16_t content_type) noexcept;

struct Message {
    std::uint16_t message_id = 0;
    std::uint8_t version = 0;
    ByteView body;  // after message_id/version/length
    std::optional<MpTable> mp_table;        // 0x0011..0x0020
    std::optional<Atsc3Message> atsc3;      // 0x8100
};

/// Parses one signaling message (message_id onwards). Unknown ids yield Message with raw body only.
bool parseMessage(ByteView msg, Message& out, std::string* error = nullptr);
bool parseMpTable(ByteView table, MpTable& out, std::string* error = nullptr);

// ---- MMT USBD (MMTUSD-1.0: BundleDescriptionMMT) ----
struct MmtComponentInfo {
    std::uint8_t component_type = 0;  // 0 audio, 1 video, 2 closed caption, 3 application
    std::optional<std::uint8_t> component_role;
    bool protected_flag = false;
    std::string component_id;        // = asset id
    std::optional<std::string> component_name;
};
/// USBD ROUTEComponent: a ROUTE session carrying locally-cached content (NRT files, app packages) of an MMT service;
/// its SLS bundle holds the S-TSID named by sTSIDUri.
struct MmtRouteComponent {
    std::optional<std::string> stsid_uri, apd_uri;
    std::uint32_t dst_ip = 0;   // host order
    std::uint16_t dst_port = 0;
    std::uint32_t src_ip = 0;   // 0 = any
};
struct MmtUsbd {
    std::uint16_t service_id = 0;
    std::optional<bool> service_status;
    std::vector<std::pair<std::string, std::string>> names;  // lang, text
    std::vector<std::string> service_languages;
    std::optional<std::string> mmt_package_id;
    std::vector<MmtComponentInfo> components;
    std::optional<MmtRouteComponent> route_component;
    std::string raw_xml;
};
bool parseMmtUsbd(const std::string& xml, MmtUsbd& out, std::string* error = nullptr);

}  // namespace atsc3::mmtp
