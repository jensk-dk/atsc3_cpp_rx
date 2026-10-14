#include "atsc3/sls/fragments.hpp"

#include <algorithm>
#include <cctype>

#include "atsc3/io/inet.hpp"
#include "xml/xml.hpp"

namespace atsc3::sls {

using namespace atsc3::xml;

namespace {
ByteView bytesOf(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
std::string lowerNoParams(std::string s) {
    const auto semi = s.find(';');
    if (semi != std::string::npos) s.erase(semi);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::optional<std::uint32_t> parseIp(const pugi::xml_node& n, const char* name) {
    const auto a = attr(n, name);
    if (!a) return std::nullopt;
    std::uint32_t ip = 0;
    if (!io::parseIpv4(*a, ip)) return std::nullopt;
    return ip;
}
}  // namespace

// ---- envelope ----
const EnvelopeItem* Envelope::find(const std::string& uri) const {
    for (const EnvelopeItem& i : items) {
        if (i.metadata_uri == uri) return &i;
    }
    return nullptr;
}

bool parseEnvelope(const std::string& text, Envelope& out, std::string* error) {
    out = Envelope{};
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    const pugi::xml_node env = root(doc, "metadataEnvelope");
    if (!env) {
        if (error) *error = "root is not metadataEnvelope";
        return false;
    }
    for (const pugi::xml_node& it : children(env, "item")) {
        EnvelopeItem i;
        i.metadata_uri = attrOr(it, "metadataURI", "");
        i.content_type = lowerNoParams(attrOr(it, "contentType", ""));
        i.version = attrUInt(it, "version");
        if (const auto v = attr(it, "validFrom")) i.valid_from_ns = parseDateTimeNs(*v);
        if (const auto v = attr(it, "validUntil")) i.valid_until_ns = parseDateTimeNs(*v);
        if (const auto v = attr(it, "availAt")) i.avail_at_ns = parseDateTimeNs(*v);
        i.next_url = attr(it, "nextURL");
        out.items.push_back(std::move(i));
    }
    out.raw_xml = serialize(env);
    return true;
}

// ---- USBD ----
bool parseUsbd(const std::string& text, Usbd& out, std::string* error) {
    out = Usbd{};
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    const pugi::xml_node bundle = root(doc, "BundleDescriptionROUTE");
    if (!bundle) {
        if (error) *error = "root is not BundleDescriptionROUTE";
        return false;
    }
    const pugi::xml_node usd = child(bundle, "UserServiceDescription");
    if (!usd) {
        if (error) *error = "no UserServiceDescription";
        return false;
    }
    out.service_id = static_cast<std::uint16_t>(attrUIntOr(usd, "serviceId", 0));
    if (attr(usd, "serviceStatus")) out.service_status = attrBool(usd, "serviceStatus", true);
    for (const pugi::xml_node& n : children(usd, "Name")) out.names.push_back(NamedText{lang(n).value_or(""), n.text().as_string()});
    for (const pugi::xml_node& n : children(usd, "ServiceLanguage")) out.service_languages.emplace_back(n.text().as_string());
    for (const pugi::xml_node& dm : children(usd, "DeliveryMethod")) {
        UsbdDeliveryMethod m;
        for (const pugi::xml_node& b : children(dm, "BroadcastAppService"))
            for (const pugi::xml_node& p : children(b, "BasePattern")) m.broadcast_base_patterns.emplace_back(p.text().as_string());
        for (const pugi::xml_node& u : children(dm, "UnicastAppService"))
            for (const pugi::xml_node& p : children(u, "BasePattern")) m.unicast_base_patterns.emplace_back(p.text().as_string());
        out.delivery_methods.push_back(std::move(m));
    }
    out.raw_xml = serialize(bundle);
    return true;
}

// ---- S-TSID ----
namespace {
SrcFlow parseSrcFlow(const pugi::xml_node& sf) {
    SrcFlow f;
    f.rt = attrBool(sf, "rt", false);
    f.min_buff_size = attrUInt(sf, "minBuffSize");
    if (const pugi::xml_node efdt = child(sf, "EFDT")) {
        route::Efdt e;
        if (route::parseEfdt(serialize(efdt), e)) f.efdt = std::move(e);
    }
    if (const pugi::xml_node ci = child(sf, "ContentInfo")) {
        if (const pugi::xml_node mi = child(ci, "MediaInfo")) {
            MediaInfo m;
            m.rep_id = attr(mi, "repId");
            m.content_type = attr(mi, "contentType");
            m.lang = lang(mi);
            m.startup = attrBool(mi, "startup", false);
            for (const pugi::xml_node& cr : children(mi, "ContentRating")) m.content_ratings.push_back(attrOr(cr, "value", ""));
            f.media_info = std::move(m);
        }
        f.has_aea_media = static_cast<bool>(child(ci, "AEAMedia"));
    }
    for (const pugi::xml_node& p : children(sf, "Payload")) {
        route::PayloadRule r;
        r.codepoint = static_cast<std::uint8_t>(attrUIntOr(p, "codePoint", 128));
        r.format_id = static_cast<route::FormatId>(attrUIntOr(p, "formatId", 1));
        r.frag = static_cast<std::uint8_t>(attrUIntOr(p, "frag", 0));
        r.order = attrBool(p, "order", true);
        r.src_fec_payload_id = static_cast<std::uint8_t>(attrUIntOr(p, "srcFecPayloadId", 0));
        r.fec_params = attr(p, "fecParams");
        f.payloads.push_back(r);
    }
    f.raw_xml = serialize(sf);
    return f;
}
RepairFlow parseRepairFlow(const pugi::xml_node& rf) {
    RepairFlow r;
    if (const pugi::xml_node fp = child(rf, "FECParameters")) {
        r.maximum_delay = attrUInt(fp, "maximumDelay");
        r.overhead = attrUInt(fp, "overhead");
        r.min_buff_size = attrUInt(fp, "minBuffSize");
        r.fec_oti = attr(fp, "fecOTI");
        for (const pugi::xml_node& po : children(fp, "ProtectedObject")) {
            ProtectedObject o;
            o.session_description = attr(po, "sessionDescription");
            o.tsi = attrUIntOr(po, "tsi", 0);
            if (const pugi::xml_node st = child(po, "SourceTOI")) {
                o.source_toi_x = attrUInt(st, "x");
                o.source_toi_y = attrUInt(st, "y");
            }
            o.fec_transport_object_size = attr(po, "fecTransportObjectSize");
            r.protected_objects.push_back(std::move(o));
        }
    }
    r.raw_xml = serialize(rf);
    return r;
}
}  // namespace

bool parseStsid(const std::string& text, Stsid& out, std::string* error) {
    out = Stsid{};
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    const pugi::xml_node st = root(doc, "S-TSID");
    if (!st) {
        if (error) *error = "root is not S-TSID";
        return false;
    }
    for (const pugi::xml_node& rsn : children(st, "RS")) {
        Rs rs;
        rs.src_ip = parseIp(rsn, "sIpAddr");
        rs.dst_ip = parseIp(rsn, "dIpAddr");
        if (const auto p = attrUInt(rsn, "dPort")) rs.dst_port = static_cast<std::uint16_t>(*p);
        for (const pugi::xml_node& lsn : children(rsn, "LS")) {
            Ls ls;
            const auto tsi = attrUInt(lsn, "tsi");
            if (!tsi) continue;
            ls.tsi = *tsi;
            ls.bw = attrUInt(lsn, "bw");
            if (const auto v = attr(lsn, "startTime")) ls.start_time_ns = parseDateTimeNs(*v);
            if (const auto v = attr(lsn, "endTime")) ls.end_time_ns = parseDateTimeNs(*v);
            if (const pugi::xml_node sf = child(lsn, "SrcFlow")) ls.src_flow = parseSrcFlow(sf);
            if (const pugi::xml_node rf = child(lsn, "RepairFlow")) ls.repair_flow = parseRepairFlow(rf);
            rs.ls.push_back(std::move(ls));
        }
        out.rs.push_back(std::move(rs));
    }
    if (out.rs.empty()) {
        if (error) *error = "S-TSID without RS";
        return false;
    }
    out.raw_xml = serialize(st);
    return true;
}

route::ChannelConfig channelConfig(const Ls& ls) {
    route::ChannelConfig c;
    c.tsi = ls.tsi;
    c.bw = ls.bw;
    if (ls.src_flow) {
        c.real_time = ls.src_flow->rt;
        c.efdt = ls.src_flow->efdt;
        c.payloads = ls.src_flow->payloads;
        if (ls.src_flow->media_info) {
            c.rep_id = ls.src_flow->media_info->rep_id;
            c.media_content_type = ls.src_flow->media_info->content_type;
            c.lang = ls.src_flow->media_info->lang;
        }
    }
    return c;
}

// ---- HELD ----
bool parseHeld(const std::string& text, Held& out, std::string* error) {
    out = Held{};
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    const pugi::xml_node held = root(doc, "HELD");
    if (!held) {
        if (error) *error = "root is not HELD";
        return false;
    }
    for (const pugi::xml_node& n : children(held, "HTMLEntryPackage")) {
        HtmlEntryPackage p;
        p.app_context_id = attrOr(n, "appContextId", "");
        p.app_id = attr(n, "appId");
        p.required_capabilities = attr(n, "requiredCapabilities");
        p.app_rendering = attr(n, "appRendering");
        p.clear_app_context_cache_date = attr(n, "clearAppContextCacheDate");
        p.bcast_entry_package_url = attr(n, "bcastEntryPackageUrl");
        p.bcast_entry_page_url = attr(n, "bcastEntryPageUrl");
        p.bband_entry_page_url = attr(n, "bbandEntryPageUrl");
        if (const auto v = attr(n, "validFrom")) p.valid_from_ns = parseDateTimeNs(*v);
        if (const auto v = attr(n, "validUntil")) p.valid_until_ns = parseDateTimeNs(*v);
        if (const auto l = attr(n, "coupledServices"))
            for (const std::uint32_t v : splitUIntList(*l)) p.coupled_services.push_back(static_cast<std::uint16_t>(v));
        if (const auto l = attr(n, "lctTSIRef")) p.lct_tsi_ref = splitUIntList(*l);
        p.raw_xml = serialize(n);
        out.packages.push_back(std::move(p));
    }
    out.raw_xml = serialize(held);
    return true;
}

}  // namespace atsc3::sls
