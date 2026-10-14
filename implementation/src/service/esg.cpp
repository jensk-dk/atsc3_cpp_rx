// SGDU splitter and SGDD parser (spec 5.11 item 4; OMA BCAST SG 1.0.1 §5.4.1.3/§5.4.1.5 with the A/332 §5.4
// constraints). Layout cross-checked on the Sinclair/Digicap SGDUs of the BWI capture and on the UniSoft SGDD.
#include "atsc3/service/esg.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "xml/xml.hpp"

namespace atsc3::service {

std::string_view toString(EsgFragmentType t) noexcept {
    switch (t) {
        case EsgFragmentType::Service: return "Service";
        case EsgFragmentType::Content: return "Content";
        case EsgFragmentType::Schedule: return "Schedule";
        case EsgFragmentType::Access: return "Access";
        case EsgFragmentType::SessionDescription: return "SessionDescription";
        case EsgFragmentType::PurchaseItem: return "PurchaseItem";
        case EsgFragmentType::PurchaseData: return "PurchaseData";
        case EsgFragmentType::PurchaseChannel: return "PurchaseChannel";
        case EsgFragmentType::PreviewData: return "PreviewData";
        case EsgFragmentType::InteractivityData: return "InteractivityData";
        default: return "Unknown";
    }
}

EsgFragmentType esgFragmentTypeFromRoot(std::string_view root) noexcept {
    if (root == "Service") return EsgFragmentType::Service;
    if (root == "Content") return EsgFragmentType::Content;
    if (root == "Schedule") return EsgFragmentType::Schedule;
    if (root == "Access") return EsgFragmentType::Access;
    if (root == "SessionDescription") return EsgFragmentType::SessionDescription;
    if (root == "PurchaseItem") return EsgFragmentType::PurchaseItem;
    if (root == "PurchaseData") return EsgFragmentType::PurchaseData;
    if (root == "PurchaseChannel") return EsgFragmentType::PurchaseChannel;
    if (root == "PreviewData") return EsgFragmentType::PreviewData;
    if (root == "InteractivityData") return EsgFragmentType::InteractivityData;
    return EsgFragmentType::Unknown;
}

std::int64_t esgTimeToUnix(std::uint64_t value) noexcept {
    if (value < kNtpUnixEpoch) return static_cast<std::int64_t>(value);   // already Unix seconds (observed)
    return static_cast<std::int64_t>(value - kNtpUnixEpoch);
}

namespace {

std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

EsgFragmentType typeFromCode(std::uint8_t code) noexcept {
    return code <= 10 ? static_cast<EsgFragmentType>(code) : EsgFragmentType::Unknown;
}

std::optional<std::int64_t> esgTimeAttr(const pugi::xml_node& n, std::string_view name) {
    const auto a = xml::attr(n, name);
    if (!a || a->empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(a->c_str(), &end, 10);
    if (end == a->c_str()) {
        // xs:dateTime is not what OMA specifies here, but be lenient
        if (const auto ns = xml::parseDateTimeNs(*a)) return *ns / 1'000'000'000;
        return std::nullopt;
    }
    return esgTimeToUnix(v);
}

ByteView bytesOf(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

}  // namespace

bool parseSgdu(ByteView data, Sgdu& out, std::string* error) {
    out = Sgdu{};
    constexpr std::size_t kPreamble = 9;   // extension_offset(4) reserved(1) n_o_service_guide_fragments(4)
    if (data.size() < kPreamble) {
        if (error) *error = "SGDU shorter than its unit header";
        return false;
    }
    out.extension_offset = be32(data.data());
    out.fragment_count = be32(data.data() + 5);
    const std::size_t header = kPreamble + static_cast<std::size_t>(out.fragment_count) * 12u;
    if (out.fragment_count > 1'000'000u || header > data.size()) {
        if (error) *error = "SGDU header claims " + std::to_string(out.fragment_count) + " fragments in " + std::to_string(data.size()) + " bytes";
        return false;
    }
    if (out.extension_offset != 0) out.warnings.push_back("extension_offset " + std::to_string(out.extension_offset) + " (extensions ignored)");

    const std::size_t payload_len = data.size() - header;
    // The extension block, when present, follows the fragments: it bounds the last slot.
    std::size_t payload_end = payload_len;
    if (out.extension_offset != 0 && out.extension_offset <= payload_len) payload_end = out.extension_offset;

    std::vector<std::uint32_t> offsets;
    offsets.reserve(out.fragment_count);
    out.fragments.reserve(out.fragment_count);
    for (std::uint32_t i = 0; i < out.fragment_count; ++i) {
        const std::uint8_t* e = data.data() + kPreamble + static_cast<std::size_t>(i) * 12u;
        SgduFragment f;
        f.transport_id = be32(e);
        f.version = be32(e + 4);
        f.offset = be32(e + 8);
        offsets.push_back(f.offset);
        out.fragments.push_back(std::move(f));
    }
    std::sort(offsets.begin(), offsets.end());
    for (SgduFragment& f : out.fragments) {
        if (f.offset >= payload_end) {
            ++out.skipped;
            if (out.warnings.size() < 8) out.warnings.push_back("fragment " + std::to_string(f.transport_id) + " offset beyond the payload");
            continue;
        }
        const auto next = std::upper_bound(offsets.begin(), offsets.end(), f.offset);
        const std::size_t end = next == offsets.end() ? payload_end : std::min<std::size_t>(*next, payload_end);
        const std::uint8_t* slot = data.data() + header + f.offset;
        f.size = end - f.offset;
        if (f.size < 1) {
            ++out.skipped;
            continue;
        }
        f.encoding = slot[0];
        if (f.encoding != 0) {   // SDP / USBD / ADP encodings are not allowed by A/332: keep the entry, drop the data
            if (out.warnings.size() < 8) out.warnings.push_back("fragment " + std::to_string(f.transport_id) + " uses fragmentEncoding " + std::to_string(f.encoding));
            continue;
        }
        if (f.size < 2) {
            ++out.skipped;
            continue;
        }
        f.type = typeFromCode(slot[1]);
        const char* text = reinterpret_cast<const char*>(slot + 2);
        std::size_t len = f.size - 2;
        while (len > 0 && (text[len - 1] == '\0' || std::isspace(static_cast<unsigned char>(text[len - 1])))) --len;   // slot padding
        f.xml.assign(text, len);
    }
    return true;
}

bool parseSgdd(ByteView xml_bytes, Sgdd& out, std::string* error) {
    out = Sgdd{};
    pugi::xml_document doc;
    if (!xml::load(doc, xml_bytes, error)) return false;
    const pugi::xml_node root = xml::root(doc, "ServiceGuideDeliveryDescriptor");
    if (!root) {
        if (error) *error = "root is not ServiceGuideDeliveryDescriptor";
        return false;
    }
    out.id = xml::attrOr(root, "id", "");
    out.version = xml::attrUInt(root, "version");
    for (const pugi::xml_node& de : xml::children(root, "DescriptorEntry")) {
        SgddEntry e;
        if (const pugi::xml_node gc = xml::child(de, "GroupingCriteria")) {
            if (const pugi::xml_node tg = xml::child(gc, "TimeGroupingCriteria")) {
                e.start_unix = esgTimeAttr(tg, "startTime");
                e.end_unix = esgTimeAttr(tg, "endTime");
            }
        }
        if (const pugi::xml_node tr = xml::child(de, "Transport")) {
            e.ip = xml::attr(tr, "ipAddress");
            e.src_ip = xml::attr(tr, "srcIpAddress");
            if (const auto p = xml::attrUInt(tr, "port")) e.port = static_cast<std::uint16_t>(*p & 0xFFFFu);
            e.tsi = xml::attrUInt(tr, "transmissionSessionID");
        }
        for (const pugi::xml_node& un : xml::children(de, "ServiceGuideDeliveryUnit")) {
            SgddUnit u;
            u.toi = xml::attrUInt(un, "transportObjectID");
            u.content_location = xml::attrOr(un, "contentLocation", "");
            for (const pugi::xml_node& fr : xml::children(un, "Fragment")) {
                SgddFragmentRef r;
                r.transport_id = xml::attrUIntOr(fr, "transportID", 0);
                r.version = xml::attrUIntOr(fr, "version", 0);
                r.encoding = static_cast<std::uint8_t>(xml::attrUIntOr(fr, "fragmentEncoding", 0) & 0xFFu);
                r.type = typeFromCode(static_cast<std::uint8_t>(xml::attrUIntOr(fr, "fragmentType", 0) & 0xFFu));
                r.id = xml::attrOr(fr, "id", "");
                r.valid_from_unix = esgTimeAttr(fr, "validFrom");
                r.valid_to_unix = esgTimeAttr(fr, "validTo");
                u.fragments.push_back(std::move(r));
            }
            e.units.push_back(std::move(u));
        }
        out.entries.push_back(std::move(e));
    }
    return true;
}

bool parseSgdd(const std::string& xml_text, Sgdd& out, std::string* error) { return parseSgdd(bytesOf(xml_text), out, error); }

bool parseEsgFragmentHeader(const std::string& xml_text, EsgFragment& out, std::string* error) {
    pugi::xml_document doc;
    if (!xml::load(doc, bytesOf(xml_text), error)) return false;
    const pugi::xml_node root = doc.document_element();
    if (!root) {
        if (error) *error = "no root element";
        return false;
    }
    out.type = esgFragmentTypeFromRoot(xml::localName(root.name()));
    out.id = xml::attrOr(root, "id", "");
    if (const auto v = xml::attrUInt(root, "version")) out.version = *v;
    out.valid_from_unix = esgTimeAttr(root, "validFrom");
    out.valid_to_unix = esgTimeAttr(root, "validTo");
    return true;
}

}  // namespace atsc3::service
