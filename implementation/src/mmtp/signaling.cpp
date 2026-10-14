#include "atsc3/mmtp/signaling.hpp"

#include <cctype>
#include <cstdio>

#include "atsc3/io/inet.hpp"
#include "atsc3/util/gzip.hpp"
#include "xml/xml.hpp"

namespace atsc3::mmtp {

namespace {
struct Reader {
    ByteView d;
    std::size_t off = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (off + n > d.size()) ok = false;
        return ok;
    }
    std::uint8_t u8() { return need(1) ? d[off++] : 0; }
    std::uint16_t u16() {
        if (!need(2)) return 0;
        const std::uint16_t v = io::be16(d.data() + off);
        off += 2;
        return v;
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        const std::uint32_t v = io::be32(d.data() + off);
        off += 4;
        return v;
    }
    std::uint64_t u64() {
        const std::uint64_t hi = u32();
        return (hi << 32) | u32();
    }
    ByteView bytes(std::size_t n) {
        if (!need(n)) return ByteView();
        ByteView v = d.subview(off, n);
        off += n;
        return v;
    }
};

bool printable4cc(ByteView v) {
    if (v.size() != 4) return false;
    for (const std::uint8_t c : v)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

bool parseAssets(Reader& r, MpTable& t) {
    const std::uint8_t n = r.u8();
    for (std::uint8_t i = 0; i < n && r.ok; ++i) {
        Asset a;
        a.identifier_type = r.u8();
        if (a.identifier_type != 0) return false;  // only asset_id form is defined
        a.asset_id_scheme = r.u32();
        const std::uint32_t len = r.u32();
        if (len > 1024) return false;
        ByteView id = r.bytes(len);
        a.asset_id.assign(id.begin(), id.end());
        ByteView type = r.bytes(4);
        if (!r.ok || !printable4cc(type)) return false;
        a.asset_type.assign(type.begin(), type.end());
        const std::uint8_t flags = r.u8();
        a.default_asset = (flags & 0x80) != 0;
        if (flags & 0x01) {
            a.clock_relation_id = r.u8();
            const std::uint8_t tf = r.u8();
            if (tf & 0x01) a.timescale = r.u32();
        }
        const std::uint8_t nloc = r.u8();
        for (std::uint8_t j = 0; j < nloc && r.ok; ++j) {
            AssetLocation loc;
            loc.type = r.u8();
            switch (loc.type) {
                case 0: loc.packet_id = r.u16(); break;
                case 1:
                    loc.src_ip = r.u32();
                    loc.dst_ip = r.u32();
                    loc.dst_port = r.u16();
                    loc.packet_id = r.u16();
                    break;
                case 2:
                    r.bytes(16);
                    r.bytes(16);
                    loc.dst_port = r.u16();
                    loc.packet_id = r.u16();
                    break;
                default:
                    return false;  // URL forms not expected in broadcast
            }
            a.locations.push_back(loc);
        }
        const std::uint16_t dlen = r.u16();
        Reader dr{r.bytes(dlen), 0, true};
        while (dr.ok && dr.off + 3 <= dr.d.size()) {
            const std::uint16_t tag = dr.u16();
            const std::uint8_t len8 = dr.u8();
            Reader body{dr.bytes(len8), 0, true};
            a.descriptor_tags.push_back(tag);
            if (tag == 0x0001) {
                while (body.off + 12 <= body.d.size()) {
                    MpuTimestamp ts;
                    ts.mpu_sequence_number = body.u32();
                    ts.ntp = body.u64();
                    a.mpu_timestamps.push_back(ts);
                }
            }
        }
        if (!r.ok) return false;
        t.assets.push_back(std::move(a));
    }
    return r.ok;
}
}  // namespace

std::string Asset::assetIdText() const {
    bool printable = !asset_id.empty();
    for (const std::uint8_t c : asset_id)
        if (c < 0x20 || c > 0x7E) printable = false;
    if (printable) return std::string(asset_id.begin(), asset_id.end());
    std::string hex;
    char buf[4];
    for (const std::uint8_t c : asset_id) {
        std::snprintf(buf, sizeof buf, "%02x", c);
        hex += buf;
    }
    return hex;
}

std::optional<std::uint16_t> Asset::packetId() const {
    for (const AssetLocation& l : locations)
        if (l.type == 0 || l.type == 1 || l.type == 2) return l.packet_id;
    return std::nullopt;
}

const char* atsc3ContentTypeName(std::uint16_t ct) noexcept {
    switch (ct) {
        case 0x0001: return "USBD";
        case 0x0002: return "MPD";
        case 0x0003: return "HELD";
        case 0x0004: return "AEI";
        case 0x0005: return "VideoStreamProperties";
        case 0x0006: return "Staggercast";
        case 0x0007: return "InbandEventDescriptor";
        case 0x0008: return "CaptionAsset";
        case 0x0009: return "AudioStreamProperties";
        case 0x000A: return "DWD";
        case 0x000B: return "RSAT";
        case 0x000C: return "SecurityProperties";
        default: return "unknown";
    }
}

bool parseMpTable(ByteView table, MpTable& out, std::string* error) {
    out = MpTable{};
    Reader r{table, 0, true};
    out.table_id = r.u8();
    out.version = r.u8();
    const std::uint16_t len = r.u16();
    (void)len;
    out.mode = static_cast<std::uint8_t>(r.u8() & 0x03);
    const std::size_t assets_off = r.off;
    // Complete tables (0x11, 0x20) carry MMT_package_id and MP_table_descriptors, subset tables do not.
    // The other layout is tried as a fallback for encoders that deviate.
    auto with_package = [&](MpTable& t) {
        Reader r2{table, assets_off, true};
        const std::uint8_t pil = r2.u8();
        ByteView pid = r2.bytes(pil);
        t.package_id.assign(pid.begin(), pid.end());
        const std::uint16_t dl = r2.u16();
        r2.bytes(dl);
        return r2.ok && parseAssets(r2, t);
    };
    auto without_package = [&](MpTable& t) {
        Reader r3{table, assets_off, true};
        t.package_id.clear();
        return parseAssets(r3, t);
    };
    MpTable first = out, second = out;
    const bool complete = out.table_id == 0x11 || out.table_id == 0x20;
    const bool ok1 = complete ? with_package(first) : without_package(first);
    if (ok1 && !first.assets.empty()) {
        out = std::move(first);
        return true;
    }
    const bool ok2 = complete ? without_package(second) : with_package(second);
    if (ok2 && !second.assets.empty()) {
        out = std::move(second);
        return true;
    }
    if (ok1) {  // a table without assets (valid but empty)
        out = std::move(first);
        return true;
    }
    if (error) *error = "MP table asset loop does not parse";
    return false;
}

bool parseMessage(ByteView msg, Message& out, std::string* error) {
    out = Message{};
    Reader r{msg, 0, true};
    out.message_id = r.u16();
    out.version = r.u8();
    if (!r.ok) {
        if (error) *error = "message too short";
        return false;
    }
    if (out.message_id == 0x8100) {
        const std::uint32_t len = r.u32();
        out.body = r.bytes(std::min<std::size_t>(len, msg.size() - r.off));
        Reader b{out.body, 0, true};
        Atsc3Message a;
        a.service_id = b.u16();
        a.content_type = b.u16();
        a.content_version = b.u8();
        a.compression = b.u8();
        const std::uint8_t ul = b.u8();
        ByteView uri = b.bytes(ul);
        a.uri.assign(uri.begin(), uri.end());
        const std::uint32_t cl = b.u32();
        ByteView content = b.bytes(std::min<std::size_t>(cl, b.d.size() - b.off));
        if (!b.ok) {
            if (error) *error = "mmt_atsc3_message too short";
            return false;
        }
        if (a.compression == 2 || util::isGzip(content)) {
            if (auto inflated = util::gunzip(content, 16u << 20)) {
                a.content = std::move(*inflated);
                a.inflated = true;
            } else {
                a.content.assign(content.begin(), content.end());
            }
        } else {
            a.content.assign(content.begin(), content.end());
        }
        out.atsc3 = std::move(a);
        return true;
    }
    const std::uint16_t len = r.u16();
    out.body = r.bytes(std::min<std::size_t>(len, msg.size() - r.off));
    if (out.message_id >= 0x0011 && out.message_id <= 0x0020) {
        MpTable t;
        if (parseMpTable(out.body, t, error)) out.mp_table = std::move(t);
        else return false;
    }
    return true;
}

bool parseMmtUsbd(const std::string& text, MmtUsbd& out, std::string* error) {
    using namespace atsc3::xml;
    out = MmtUsbd{};
    pugi::xml_document doc;
    if (!load(doc, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error)) return false;
    const pugi::xml_node bundle = root(doc, "BundleDescriptionMMT");
    if (!bundle) {
        if (error) *error = "root is not BundleDescriptionMMT";
        return false;
    }
    const pugi::xml_node usd = child(bundle, "UserServiceDescription");
    if (!usd) {
        if (error) *error = "no UserServiceDescription";
        return false;
    }
    out.service_id = static_cast<std::uint16_t>(attrUIntOr(usd, "serviceId", 0));
    if (attr(usd, "serviceStatus")) out.service_status = attrBool(usd, "serviceStatus", true);
    for (const pugi::xml_node& n : children(usd, "Name")) out.names.emplace_back(lang(n).value_or(""), n.text().as_string());
    for (const pugi::xml_node& n : children(usd, "ServiceLanguage")) out.service_languages.emplace_back(n.text().as_string());
    if (const pugi::xml_node mc = child(usd, "MPUComponent")) out.mmt_package_id = attr(mc, "mmtPackageId");
    if (const pugi::xml_node rc = child(usd, "ROUTEComponent")) {
        MmtRouteComponent r;
        r.stsid_uri = attr(rc, "sTSIDUri");
        r.apd_uri = attr(rc, "apdUri");
        if (const auto ip = attr(rc, "sTSIDDestinationIpAddress")) io::parseIpv4(*ip, r.dst_ip);
        r.dst_port = static_cast<std::uint16_t>(attrUIntOr(rc, "sTSIDDestinationUdpPort", 0));
        if (const auto ip = attr(rc, "sTSIDSourceIpAddress")) io::parseIpv4(*ip, r.src_ip);
        if (r.dst_ip && r.dst_port) out.route_component = r;
    }
    for (const pugi::xml_node& c : children(usd, "ComponentInfo")) {
        MmtComponentInfo ci;
        ci.component_type = static_cast<std::uint8_t>(attrUIntOr(c, "componentType", 0));
        if (const auto v = attrUInt(c, "componentRole")) ci.component_role = static_cast<std::uint8_t>(*v);
        ci.protected_flag = attrBool(c, "componentProtectedFlag", false);
        ci.component_id = attrOr(c, "componentId", "");
        ci.component_name = attr(c, "componentName");
        out.components.push_back(std::move(ci));
    }
    out.raw_xml = serialize(bundle);
    return true;
}

}  // namespace atsc3::mmtp
