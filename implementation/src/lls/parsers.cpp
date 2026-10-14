// Typed LLS table parsers (specs/schemas/lls/*.xsd). Namespace-tolerant, defaults applied, unknown content ignored.
#include "atsc3/io/inet.hpp"
#include "atsc3/lls/tables.hpp"
#include "xml/xml.hpp"

namespace atsc3::lls {

using namespace atsc3::xml;

namespace {
bool loadRoot(const std::string& text, const char* root_name, pugi::xml_document& doc, pugi::xml_node& root_node,
              std::string* error) {
    if (!load(doc, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error)) return false;
    root_node = root(doc, root_name);
    if (!root_node) {
        if (error) *error = std::string("root element is not ") + root_name + " (got " + doc.document_element().name() + ")";
        return false;
    }
    return true;
}
std::vector<std::uint16_t> u16List(const std::optional<std::string>& s) {
    std::vector<std::uint16_t> v;
    if (!s) return v;
    for (std::uint32_t x : splitUIntList(*s)) v.push_back(static_cast<std::uint16_t>(x));
    return v;
}
std::vector<std::uint16_t> u16ListText(const pugi::xml_node& n) {
    std::vector<std::uint16_t> v;
    for (std::uint32_t x : splitUIntList(n.text().as_string())) v.push_back(static_cast<std::uint16_t>(x));
    return v;
}
std::vector<LangText> langTexts(const pugi::xml_node& parent, const char* name) {
    std::vector<LangText> v;
    for (const pugi::xml_node& n : children(parent, name)) {
        LangText lt;
        lt.lang = lang(n).value_or("");
        lt.text = n.text().as_string();
        v.push_back(std::move(lt));
    }
    return v;
}
std::optional<std::uint16_t> u16(const pugi::xml_node& n, const char* name) {
    const auto v = attrUInt(n, name);
    if (!v) return std::nullopt;
    return static_cast<std::uint16_t>(*v);
}
}  // namespace

bool parseSlt(const std::string& text, Slt& out, std::string* error) {
    out = Slt{};
    pugi::xml_document doc;
    pugi::xml_node r;
    if (!loadRoot(text, "SLT", doc, r, error)) return false;
    out.bsid = u16List(attr(r, "bsid"));
    if (const pugi::xml_node c = child(r, "SLTCapabilities")) out.slt_capabilities = c.text().as_string();
    for (const pugi::xml_node& u : children(r, "SLTInetUrl")) {
        out.slt_inet_url.push_back(InetUrl{u.text().as_string(), static_cast<std::uint8_t>(attrUIntOr(u, "urlType", 0))});
    }
    for (const pugi::xml_node& s : children(r, "Service")) {
        SltService svc;
        const auto id = attrUInt(s, "serviceId");
        if (!id) continue;  // serviceId is required
        svc.service_id = static_cast<std::uint16_t>(*id);
        svc.global_service_id = attr(s, "globalServiceID");
        svc.slt_svc_seq_num = static_cast<std::uint8_t>(attrUIntOr(s, "sltSvcSeqNum", 0));
        svc.protected_flag = attrBool(s, "protected", false);
        svc.major_channel_no = u16(s, "majorChannelNo");
        svc.minor_channel_no = u16(s, "minorChannelNo");
        svc.service_category = static_cast<std::uint8_t>(attrUIntOr(s, "serviceCategory", 0));
        svc.short_service_name = attr(s, "shortServiceName");
        svc.hidden = attrBool(s, "hidden", false);
        svc.hide_in_guide = attrBool(s, "hideInGuide", false);
        svc.broadband_access_required = attrBool(s, "broadbandAccessRequired", false);
        if (attr(s, "essential")) svc.essential = attrBool(s, "essential", false);
        svc.drm_system_id = attr(s, "drmSystemID");
        if (attrOr(s, "configuration", "Broadcast") == "Broadband") svc.configuration = SltService::Configuration::Broadband;
        if (const pugi::xml_node sim = child(s, "SimulcastTSID")) {
            SimulcastTsid st;
            st.tsid = static_cast<std::uint16_t>(sim.text().as_uint());
            st.major = u16(sim, "simulcastMajorChannelNo");
            st.minor = u16(sim, "simulcastMinorChannelNo");
            svc.simulcast = st;
        }
        if (const pugi::xml_node cap = child(s, "SvcCapabilities")) svc.svc_capabilities = cap.text().as_string();
        if (const pugi::xml_node b = child(s, "BroadcastSvcSignaling")) {
            BroadcastSvcSignaling bss;
            bss.sls_protocol = static_cast<std::uint8_t>(attrUIntOr(b, "slsProtocol", 0));
            bss.sls_major_version = static_cast<std::uint8_t>(attrUIntOr(b, "slsMajorProtocolVersion", 1));
            bss.sls_minor_version = static_cast<std::uint8_t>(attrUIntOr(b, "slsMinorProtocolVersion", 0));
            std::uint32_t ip = 0;
            if (const auto a = attr(b, "slsDestinationIpAddress"); a && io::parseIpv4(*a, ip)) bss.sls_destination_ip = ip;
            bss.sls_destination_port = static_cast<std::uint16_t>(attrUIntOr(b, "slsDestinationUdpPort", 0));
            if (const auto a = attr(b, "slsSourceIpAddress"); a && io::parseIpv4(*a, ip)) bss.sls_source_ip = ip;
            svc.bss = bss;
        }
        for (const pugi::xml_node& u : children(s, "SvcInetUrl")) {
            svc.svc_inet_url.push_back(InetUrl{u.text().as_string(), static_cast<std::uint8_t>(attrUIntOr(u, "urlType", 0))});
        }
        for (const pugi::xml_node& o : children(s, "OtherBsid")) {
            svc.other_bsid.push_back(OtherBsid{u16ListText(o), static_cast<std::uint8_t>(attrUIntOr(o, "type", 0))});
        }
        for (const pugi::xml_node& o : children(s, "OtherRf")) {
            OtherRf rf;
            rf.other_bsid_rf = static_cast<std::uint16_t>(attrUIntOr(o, "otherBsidRf", 0));
            rf.other_bsid = static_cast<std::uint16_t>(attrUIntOr(o, "otherBsid", 0));
            rf.lat = attrDouble(o, "lat").value_or(0.0);
            rf.lon = attrDouble(o, "long").value_or(0.0);
            rf.elev = attrInt(o, "elev").value_or(0);
            rf.erp = attrInt(o, "erp").value_or(0);
            for (const pugi::xml_node& d : children(o, "Directional")) {
                Directional dir;
                dir.heading = attrUIntOr(d, "heading", 0);
                dir.strength = attrDouble(d, "strength").value_or(0.0);
                dir.haat = attrInt(d, "haat");
                rf.directional.push_back(dir);
            }
            svc.other_rf.push_back(std::move(rf));
        }
        svc.raw_xml = serialize(s);
        out.services.push_back(std::move(svc));
    }
    return true;
}

bool parseSystemTime(const std::string& text, SystemTime& out, std::string* error) {
    out = SystemTime{};
    pugi::xml_document doc;
    pugi::xml_node r;
    if (!loadRoot(text, "SystemTime", doc, r, error)) return false;
    out.current_utc_offset = static_cast<std::int16_t>(attrInt(r, "currentUtcOffset").value_or(0));
    out.ptp_prepend = static_cast<std::uint16_t>(attrUIntOr(r, "ptpPrepend", 0));
    out.leap59 = attrBool(r, "leap59", false);
    out.leap61 = attrBool(r, "leap61", false);
    if (const auto d = attr(r, "utcLocalOffset")) out.utc_local_offset_s = parseDurationSeconds(*d);
    out.ds_status = attrBool(r, "dsStatus", false);
    if (const auto v = attrUInt(r, "dsDayOfMonth")) out.ds_day_of_month = static_cast<std::uint8_t>(*v);
    if (const auto v = attrUInt(r, "dsHour")) out.ds_hour = static_cast<std::uint8_t>(*v);
    return true;
}

bool parseAeat(const std::string& text, Aeat& out, std::string* error) {
    out = Aeat{};
    pugi::xml_document doc;
    pugi::xml_node r;
    if (!loadRoot(text, "AEAT", doc, r, error)) return false;
    out.aea_table_id = attr(r, "aeaTableId");
    for (const pugi::xml_node& a : children(r, "AEA")) {
        Aea aea;
        aea.aea_id = attrOr(a, "aeaId", "");
        aea.issuer = attrOr(a, "issuer", "");
        aea.audience = attrOr(a, "audience", "");
        aea.aea_type = attrOr(a, "aeaType", "");
        if (const auto s = attr(a, "subAudience")) aea.sub_audience = splitList(*s);
        aea.ref_aea_id = attr(a, "refAEAId");
        aea.category = attr(a, "category");
        if (const auto p = attrUInt(a, "priority")) aea.priority = static_cast<std::uint8_t>(*p);
        aea.wakeup = attrBool(a, "wakeup", false);
        if (const pugi::xml_node h = child(a, "Header")) {
            if (const auto e = attr(h, "effective")) aea.effective_ns = parseDateTimeNs(*e);
            if (const auto e = attr(h, "expires")) aea.expires_ns = parseDateTimeNs(*e);
            if (const pugi::xml_node ec = child(h, "EventCode")) {
                aea.event_code = ec.text().as_string();
                aea.event_code_type = attr(ec, "type");
            }
            aea.event_desc = langTexts(h, "EventDesc");
            for (const pugi::xml_node& l : children(h, "Location")) {
                aea.locations.push_back(AeaLocation{attrOr(l, "type", ""), l.text().as_string()});
            }
        }
        aea.aea_text = langTexts(a, "AEAText");
        if (const pugi::xml_node lm = child(a, "LiveMedia")) {
            AeaLiveMedia m;
            m.bsid = u16List(attr(lm, "bsid"));
            m.service_id = static_cast<std::uint16_t>(attrUIntOr(lm, "serviceId", 0));
            m.service_name = langTexts(lm, "ServiceName");
            aea.live_media = std::move(m);
        }
        for (const pugi::xml_node& m : children(a, "Media")) {
            AeaMedia media;
            media.lang = lang(m);
            media.media_desc = attr(m, "mediaDesc");
            media.media_type = attr(m, "mediaType");
            media.url = attrOr(m, "url", "");
            media.alternate_url = attr(m, "alternateUrl");
            media.content_type = attr(m, "contentType");
            if (const auto l = attrInt(m, "contentLength"); l && *l >= 0) media.content_length = static_cast<std::uint64_t>(*l);
            media.media_assoc = attr(m, "mediaAssoc");
            aea.media.push_back(std::move(media));
        }
        aea.raw_xml = serialize(a);
        out.aeas.push_back(std::move(aea));
    }
    return true;
}

bool parseRrt(const std::string& text, Rrt& out, std::string* error) {
    out = Rrt{};
    pugi::xml_document doc;
    pugi::xml_node r;
    if (!loadRoot(text, "RatingRegionTables", doc, r, error)) return false;
    for (const pugi::xml_node& t : children(r, "RatingRegionTable")) {
        RatingRegionTable rrt;
        rrt.region_identifier = static_cast<std::uint8_t>(attrUIntOr(t, "regionIdentifier", 0));
        rrt.region_id_text = langTexts(t, "RegionIdText");
        for (const pugi::xml_node& d : children(t, "Dimension")) {
            RatingDimension dim;
            dim.index = static_cast<std::uint8_t>(attrUIntOr(d, "dimensionIndex", 0));
            dim.levels = static_cast<std::uint8_t>(attrUIntOr(d, "dimensionLevels", 0));
            dim.graduated = attrBool(d, "dimensionGraduated", false);
            dim.title = langTexts(d, "DimensionTitle");
            for (const pugi::xml_node& v : children(d, "Rating")) {
                RatingValue rv;
                rv.rating_value = static_cast<std::uint8_t>(attrUIntOr(v, "ratingValue", 0));
                rv.abbrev = langTexts(v, "RatingValueAbbrev");
                rv.text = langTexts(v, "RatingValueString");
                dim.ratings.push_back(std::move(rv));
            }
            rrt.dimensions.push_back(std::move(dim));
        }
        out.tables.push_back(std::move(rrt));
    }
    return true;
}

bool parseOmn(const std::string& text, Omn& out, std::string* error) {
    out = Omn{};
    pugi::xml_document doc;
    if (!load(doc, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error)) return false;
    pugi::xml_node r = root(doc, "OnScreenMessageNotification");
    if (!r) r = root(doc, "OnscreenMessageNotification");  // A/331 text spelling
    if (!r) {
        if (error) *error = "root element is not OnScreenMessageNotification";
        return false;
    }
    for (const pugi::xml_node& k : children(r, "KeepScreenClear")) {
        KeepScreenClear ksc;
        ksc.bsid = u16List(attr(k, "bsid"));
        ksc.service_id = u16(k, "serviceId");
        ksc.service_id_range = static_cast<std::uint16_t>(attrUIntOr(k, "serviceIdRange", 0));
        if (const auto d = attr(k, "notificationDuration")) ksc.notification_duration_s = parseDurationSeconds(*d).value_or(60.0);
        ksc.ksc_flag = attrBool(k, "kscFlag", true);
        ksc.version = static_cast<std::uint8_t>(attrUIntOr(k, "version", 0));
        out.entries.push_back(std::move(ksc));
    }
    return true;
}

}  // namespace atsc3::lls
