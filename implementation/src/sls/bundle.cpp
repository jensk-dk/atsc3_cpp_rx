#include "atsc3/sls/bundle.hpp"

#include <algorithm>
#include <cctype>

#include "atsc3/route/multipart.hpp"
#include "atsc3/util/gzip.hpp"
#include "xml/xml.hpp"

namespace atsc3::sls {

SlsToi decodeSlsToi(std::uint32_t toi) noexcept {
    SlsToi t;
    t.gzip = (toi >> 31) & 1;
    t.r = static_cast<std::uint8_t>((toi >> 24) & 0x7F);
    t.dwd = (toi >> 23) & 1;
    t.held = (toi >> 22) & 1;
    t.apd = (toi >> 19) & 1;
    t.mpd = (toi >> 18) & 1;
    t.stsid = (toi >> 17) & 1;
    t.usbd = (toi >> 16) & 1;
    t.version = static_cast<std::uint16_t>(toi & 0xFFFF);
    return t;
}

const char* toString(FragmentKind k) noexcept {
    switch (k) {
        case FragmentKind::Envelope: return "envelope";
        case FragmentKind::Usbd: return "usbd";
        case FragmentKind::Stsid: return "stsid";
        case FragmentKind::Mpd: return "mpd";
        case FragmentKind::Held: return "held";
        case FragmentKind::Dwd: return "dwd";
        case FragmentKind::Apd: return "apd";
        case FragmentKind::Rsat: return "rsat";
        default: return "unknown";
    }
}

const SlsFragment* SlsBundle::fragment(FragmentKind k) const {
    for (const SlsFragment& f : fragments) {
        if (f.kind == k) return &f;
    }
    return nullptr;
}

namespace {
FragmentKind kindFromType(const std::string& ct) {
    if (ct == "application/mbms-envelope+xml") return FragmentKind::Envelope;
    if (ct == "application/route-usd+xml" || ct == "application/mbms-user-service-description+xml") return FragmentKind::Usbd;
    if (ct == "application/route-s-tsid+xml" || ct == "application/s-tsid+xml") return FragmentKind::Stsid;
    if (ct == "application/dash+xml") return FragmentKind::Mpd;
    if (ct == "application/atsc-held+xml") return FragmentKind::Held;
    if (ct == "application/atsc-dwd+xml") return FragmentKind::Dwd;
    if (ct == "application/mbms-associated-procedure-description+xml") return FragmentKind::Apd;
    if (ct == "application/atsc-rsat+xml") return FragmentKind::Rsat;
    return FragmentKind::Unknown;
}
FragmentKind kindFromRoot(const std::string& text) {
    pugi::xml_document doc;
    if (!xml::load(doc, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()))) return FragmentKind::Unknown;
    const std::string_view name = xml::localName(doc.document_element().name());
    if (name == "metadataEnvelope") return FragmentKind::Envelope;
    if (name == "BundleDescriptionROUTE") return FragmentKind::Usbd;
    if (name == "S-TSID") return FragmentKind::Stsid;
    if (name == "MPD") return FragmentKind::Mpd;
    if (name == "HELD") return FragmentKind::Held;
    if (name == "DWD") return FragmentKind::Dwd;
    if (name == "AssociatedProcedureDescription") return FragmentKind::Apd;
    if (name == "RSAT") return FragmentKind::Rsat;
    return FragmentKind::Unknown;
}
}  // namespace

bool parseSlsBundle(ByteView object, std::uint32_t toi, SlsBundle& out, std::string* error) {
    out = SlsBundle{};
    out.toi = toi;
    out.toi_bits = decodeSlsToi(toi);
    std::vector<std::uint8_t> inflated;
    if (util::isGzip(object)) {
        auto r = util::gunzip(object, 64u << 20);
        if (!r) {
            if (error) *error = "gzip inflate failed";
            return false;
        }
        inflated = std::move(*r);
        object = ByteView(inflated.data(), inflated.size());
    }
    route::Multipart mp;
    if (!route::parseMultipart(object, mp, error)) return false;
    if (mp.is_signed || (mp.parts.size() == 2 && mp.parts[1].headers.contentType() == "application/pkcs7-signature")) {
        out.is_signed = true;
        if (mp.parts.size() >= 2) out.signature.assign(mp.parts[1].body.begin(), mp.parts[1].body.end());
        route::Multipart inner;
        if (!route::parseMultipart(mp.parts[0].body, inner, error)) return false;
        mp = inner;
    }
    for (const route::MimePart& part : mp.parts) {
        SlsFragment f;
        f.content_type = part.headers.contentType();
        f.location = part.headers.contentLocation().value_or("");
        f.text.assign(part.body.begin(), part.body.end());
        if (part.headers.contentEncoding() && *part.headers.contentEncoding() == "gzip") {
            if (auto r = util::gunzip(part.body, 64u << 20)) f.text.assign(r->begin(), r->end());
        }
        f.kind = kindFromType(f.content_type);
        if (f.kind == FragmentKind::Unknown) f.kind = kindFromRoot(f.text);
        out.fragments.push_back(std::move(f));
    }
    // Envelope first (if present) to attach versions and spot missing items.
    for (SlsFragment& f : out.fragments) {
        if (f.kind != FragmentKind::Envelope) continue;
        Envelope env;
        std::string err;
        if (parseEnvelope(f.text, env, &err)) {
            for (const EnvelopeItem& item : env.items) {
                bool found = false;
                for (SlsFragment& g : out.fragments) {
                    if (g.kind == FragmentKind::Envelope) continue;
                    if ((!g.location.empty() && g.location == item.metadata_uri) ||
                        (g.location.empty() && g.kind == kindFromType(item.content_type))) {
                        g.version = item.version;
                        if (g.location.empty()) g.location = item.metadata_uri;
                        found = true;
                        break;
                    }
                }
                if (!found) out.warnings.push_back("envelope item without part: " + item.metadata_uri);
            }
            out.envelope = std::move(env);
        } else {
            out.warnings.push_back("envelope: " + err);
        }
        break;
    }
    for (const SlsFragment& f : out.fragments) {
        std::string err;
        switch (f.kind) {
            case FragmentKind::Usbd: {
                Usbd u;
                if (parseUsbd(f.text, u, &err)) out.usbd = std::move(u);
                else out.warnings.push_back("usbd: " + err);
                break;
            }
            case FragmentKind::Stsid: {
                Stsid s;
                if (parseStsid(f.text, s, &err)) out.stsid = std::move(s);
                else out.warnings.push_back("stsid: " + err);
                break;
            }
            case FragmentKind::Mpd: {
                Mpd m;
                if (parseMpd(f.text, m, &err)) out.mpd = std::move(m);
                else out.warnings.push_back("mpd: " + err);
                break;
            }
            case FragmentKind::Held: {
                Held h;
                if (parseHeld(f.text, h, &err)) out.held = std::move(h);
                else out.warnings.push_back("held: " + err);
                break;
            }
            default:
                break;
        }
    }
    if (!out.usbd && !out.stsid && !out.mpd) {
        if (error) *error = "bundle has no USBD, S-TSID or MPD";
        return false;
    }
    return true;
}

}  // namespace atsc3::sls
