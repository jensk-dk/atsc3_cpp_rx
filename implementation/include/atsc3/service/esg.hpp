#pragma once
// ESG delivery containers (spec 5.11 item 4, A/332 §5.4-5.5): the OMA BCAST Service Guide Delivery Unit
// (SGDU, `application/vnd.oma.bcast.sgdu`) splitter and the Service Guide Delivery Descriptor (SGDD,
// `application/vnd.oma.bcast.sgdd+xml`) parser. The service manager uses them to turn the NRT objects of an
// ESG service (serviceCategory 4) into versioned `EsgFragment`s; the OMA data model (Service / Schedule /
// Content fragments, A/332 extensions, schedule index) is the core `esg` layer (`atsc3/esg/esg.hpp`).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::service {

/// OMA BCAST SG 1.0.1 §5.4.1.3 fragmentType; A/332 restricts SGDUs to 1..3 (0 and >3 are ignored).
enum class EsgFragmentType : std::uint8_t {
    Unknown = 0,
    Service = 1,
    Content = 2,
    Schedule = 3,
    Access = 4,
    SessionDescription = 5,
    PurchaseItem = 6,
    PurchaseData = 7,
    PurchaseChannel = 8,
    PreviewData = 9,
    InteractivityData = 10,
};
std::string_view toString(EsgFragmentType t) noexcept;
EsgFragmentType esgFragmentTypeFromRoot(std::string_view root_local_name) noexcept;

/// OMA timestamps are NTP seconds (since 1900); one encoder (UniSoft, 2019) sends Unix seconds instead.
/// Values below the NTP value of 1970-01-01 are therefore taken as Unix seconds already.
std::int64_t esgTimeToUnix(std::uint64_t value) noexcept;
constexpr std::uint64_t kNtpUnixEpoch = 2208988800ull;

// ---- SGDU (binary container) ----

struct SgduFragment {
    std::uint32_t transport_id = 0;    // fragmentTransportID
    std::uint32_t version = 0;         // fragmentVersion
    std::uint8_t encoding = 0;         // fragmentEncoding: 0 = XML (the only one A/332 allows)
    EsgFragmentType type = EsgFragmentType::Unknown;   // fragmentType, when encoding == 0
    std::uint32_t offset = 0;          // from the unit header entry
    std::size_t size = 0;              // bytes of the payload slot (header bytes included)
    std::string xml;                   // the XMLFragment (empty when not XML)
};

struct Sgdu {
    std::uint32_t extension_offset = 0;
    std::uint32_t fragment_count = 0;  // n_o_service_guide_fragments as signaled
    std::vector<SgduFragment> fragments;
    std::uint32_t skipped = 0;         // entries with an offset outside the payload
    std::vector<std::string> warnings;
};

/// Splits an SGDU. Layout verified on air (Sinclair/Digicap 2023): big-endian unit_header =
/// extension_offset(32) reserved(8) n_o_service_guide_fragments(32) then n × {fragmentTransportID(32),
/// fragmentVersion(32), offset(32)}; unit_payload slots start right after the header, each slot =
/// fragmentEncoding(8) [fragmentType(8) XMLFragment]. Slot sizes come from the next higher offset (or the
/// extension / end of data). Returns false only when the header itself cannot be read.
bool parseSgdu(ByteView data, Sgdu& out, std::string* error = nullptr);

// ---- SGDD (XML descriptor) ----

struct SgddFragmentRef {
    std::uint32_t transport_id = 0;
    std::uint32_t version = 0;
    std::uint8_t encoding = 0;
    EsgFragmentType type = EsgFragmentType::Unknown;
    std::string id;
    std::optional<std::int64_t> valid_from_unix, valid_to_unix;
};

struct SgddUnit {
    std::optional<std::uint32_t> toi;        // transportObjectID
    std::string content_location;
    std::vector<SgddFragmentRef> fragments;
};

struct SgddEntry {
    std::optional<std::int64_t> start_unix, end_unix;   // TimeGroupingCriteria
    std::optional<std::string> ip, src_ip;              // Transport
    std::optional<std::uint16_t> port;
    std::optional<std::uint32_t> tsi;                    // transmissionSessionID
    std::vector<SgddUnit> units;
};

struct Sgdd {
    std::string id;
    std::optional<std::uint32_t> version;
    std::vector<SgddEntry> entries;
};

bool parseSgdd(ByteView xml, Sgdd& out, std::string* error = nullptr);
bool parseSgdd(const std::string& xml, Sgdd& out, std::string* error = nullptr);

// ---- what the service manager delivers ----

struct EsgFragment {
    EsgFragmentType type = EsgFragmentType::Unknown;
    std::string id;                    // root @id
    std::uint32_t version = 0;         // root @version (falls back to the SGDU fragmentVersion)
    std::optional<std::int64_t> valid_from_unix, valid_to_unix;   // root validFrom / validTo
    std::string xml;
    std::uint16_t bsid = 0, service_id = 0;   // the ESG service that carried it
    std::uint32_t toi = 0, transport_id = 0;  // SGDU object and fragmentTransportID
    std::int64_t received_ns = 0;             // capture time of the object
};

/// Reads type (from the root element name), id, version and validity from a fragment's XML. False when the
/// XML does not parse or has no root element; `out.xml` is left untouched.
bool parseEsgFragmentHeader(const std::string& xml, EsgFragment& out, std::string* error = nullptr);

}  // namespace atsc3::service
