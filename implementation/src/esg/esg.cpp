// OMA BCAST SG fragment parsers with the A/332 extensions and the programme index (spec 5.17.5).
// Lenient by policy (spec 5.16): local-name matching, Name/Description as @text (A/332 replacement elements)
// or as element text (OMA LanguageString), NTP or Unix seconds in timestamps.
#include "atsc3/esg/esg.hpp"

#include <algorithm>
#include <cstdlib>

#include "xml/xml.hpp"

namespace atsc3::esg {

const char* name() noexcept { return "atsc3-esg"; }

namespace {

using namespace atsc3::xml;

ByteView bytesOf(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

std::string trimmed(const char* s) {
    std::string t = s ? s : "";
    const auto b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = t.find_last_not_of(" \t\r\n");
    return t.substr(b, e - b + 1);
}

LangText langText(const pugi::xml_node& n) {
    LangText t;
    t.lang = lang(n).value_or("en");
    if (const auto a = attr(n, "text")) t.text = *a;   // A/332 Table 5.2 / 5.9 replacement elements
    else t.text = trimmed(n.child_value());
    return t;
}

std::optional<std::int64_t> timeAttr(const pugi::xml_node& n, std::string_view name) {
    const auto a = attr(n, name);
    if (!a || a->empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(a->c_str(), &end, 10);
    if (end == a->c_str()) {
        if (const auto ns = parseDateTimeNs(*a)) return *ns / 1'000'000'000;
        return std::nullopt;
    }
    return service::esgTimeToUnix(v);
}

void parseBase(const pugi::xml_node& root, const std::string& xml_text, FragmentBase& b) {
    b.id = attrOr(root, "id", "");
    b.version = attrUIntOr(root, "version", 0);
    b.valid_from_unix = timeAttr(root, "validFrom");
    b.valid_to_unix = timeAttr(root, "validTo");
    b.xml = xml_text;
}

Icon parseIcon(const pugi::xml_node& n) {
    Icon i;
    i.uri = trimmed(n.child_value());
    i.mime = attrOr(n, "MIMEType", "");
    i.width = attrUInt(n, "width");
    i.height = attrUInt(n, "height");
    i.data_size = attrUInt(n, "dataSize");
    return i;
}

// ContentAdvisoryRatings / OtherRatings may sit directly under the fragment root (Content) or inside PrivateExt
// (Service, Table 5.5); scan both.
void parseRatings(const pugi::xml_node& parent, std::vector<Rating>& out) {
    for (const pugi::xml_node& r : children(parent, "ContentAdvisoryRatings")) {
        Rating rating;
        if (const pugi::xml_node reg = child(r, "RegionIdentifier")) rating.region = static_cast<std::uint8_t>(std::atoi(reg.child_value()) & 0xFF);
        if (const pugi::xml_node d = child(r, "RatingDescription")) {
            rating.description = trimmed(d.child_value());
            rating.lang = lang(d).value_or("en");
        }
        for (const pugi::xml_node& dv : children(r, "RatingDimVal")) {
            std::uint8_t dim = 0;
            if (const pugi::xml_node dn = child(dv, "RatingDimension")) dim = static_cast<std::uint8_t>(std::atoi(dn.child_value()) & 0xFF);
            rating.dimensions.emplace_back(dim, trimmed(child(dv, "RatingValueString").child_value()));
        }
        out.push_back(std::move(rating));
    }
    for (const pugi::xml_node& r : children(parent, "OtherRatings")) {
        Rating rating;
        rating.scheme = attrOr(r, "ratingScheme", "");
        if (const pugi::xml_node s = child(r, "RatingInformationString")) rating.description = trimmed(s.child_value());
        else if (const pugi::xml_node d = child(r, "RatingInformationData")) rating.description = serialize(d);
        out.push_back(std::move(rating));
    }
}

void parseLanguages(const pugi::xml_node& root, std::vector<std::string>& audio, std::vector<std::string>& text) {
    for (const pugi::xml_node& a : children(root, "AudioLanguage")) {
        const std::string v = trimmed(a.child_value());
        audio.push_back(v.empty() ? attrOr(a, "languageSDPTag", "") : v);
    }
    for (const pugi::xml_node& t : children(root, "TextLanguage")) {
        const std::string v = trimmed(t.child_value());
        text.push_back(v.empty() ? attrOr(t, "languageSDPTag", "") : v);
    }
}

bool loadRoot(const std::string& xml_text, std::string_view local, pugi::xml_document& doc, pugi::xml_node& out, std::string* error) {
    if (!load(doc, bytesOf(xml_text), error)) return false;
    out = root(doc, local);
    if (!out) {
        if (error) *error = "root is not " + std::string(local);
        return false;
    }
    return true;
}

PresentationWindow parseWindow(const pugi::xml_node& w) {
    PresentationWindow pw;
    pw.start_unix = timeAttr(w, "startTime").value_or(0);
    pw.end_unix = timeAttr(w, "endTime").value_or(0);
    pw.duration_s = attrUInt(w, "duration");
    if (pw.end_unix == 0 && pw.duration_s) pw.end_unix = pw.start_unix + static_cast<std::int64_t>(*pw.duration_s);
    pw.id = attr(w, "id");
    return pw;
}

}  // namespace

bool parseService(const std::string& xml_text, Service& out, std::string* error) {
    out = Service{};
    pugi::xml_document doc;
    pugi::xml_node root;
    if (!loadRoot(xml_text, "Service", doc, root, error)) return false;
    parseBase(root, xml_text, out);
    out.global_service_id = attr(root, "globalServiceID");
    out.weight = attrUInt(root, "weight");
    out.emergency = attrBool(root, "emergency", false);
    if (const pugi::xml_node st = child(root, "ServiceType")) out.service_type = static_cast<std::uint32_t>(std::strtoul(st.child_value(), nullptr, 10));
    for (const pugi::xml_node& n : children(root, "Name")) out.names.push_back(langText(n));
    for (const pugi::xml_node& n : children(root, "Description")) out.descriptions.push_back(langText(n));
    parseLanguages(root, out.audio_languages, out.text_languages);
    for (const pugi::xml_node& g : children(root, "Genre")) out.genres.push_back(attrOr(g, "href", trimmed(g.child_value())));
    parseRatings(root, out.ratings);
    if (const pugi::xml_node pe = child(root, "PrivateExt")) {
        parseRatings(pe, out.ratings);
        if (const pugi::xml_node ext = child(pe, "ATSC3ServiceExtension")) {
            for (const pugi::xml_node& ic : children(ext, "Icon")) out.icons.push_back(parseIcon(ic));
            if (const pugi::xml_node mj = child(ext, "MajorChannelNum")) out.major = static_cast<std::uint16_t>(std::atoi(mj.child_value()) & 0xFFFF);
            if (const pugi::xml_node mn = child(ext, "MinorChannelNum")) out.minor = static_cast<std::uint16_t>(std::atoi(mn.child_value()) & 0xFFFF);
        }
    }
    return true;
}

bool parseSchedule(const std::string& xml_text, Schedule& out, std::string* error) {
    out = Schedule{};
    pugi::xml_document doc;
    pugi::xml_node root;
    if (!loadRoot(xml_text, "Schedule", doc, root, error)) return false;
    parseBase(root, xml_text, out);
    out.service_id = attrOr(child(root, "ServiceReference"), "idRef", "");
    for (const pugi::xml_node& cr : children(root, "ContentReference")) {
        ContentRef ref;
        ref.content_id = attrOr(cr, "idRef", "");
        for (const pugi::xml_node& w : children(cr, "PresentationWindow")) ref.windows.push_back(parseWindow(w));
        out.contents.push_back(std::move(ref));
    }
    return true;
}

bool parseContent(const std::string& xml_text, Content& out, std::string* error) {
    out = Content{};
    pugi::xml_document doc;
    pugi::xml_node root;
    if (!loadRoot(xml_text, "Content", doc, root, error)) return false;
    parseBase(root, xml_text, out);
    for (const pugi::xml_node& sr : children(root, "ServiceReference")) out.service_ids.push_back(attrOr(sr, "idRef", ""));
    for (const pugi::xml_node& n : children(root, "Name")) out.names.push_back(langText(n));
    for (const pugi::xml_node& n : children(root, "Description")) out.descriptions.push_back(langText(n));
    if (const pugi::xml_node len = child(root, "Length")) out.length_s = parseDurationSeconds(trimmed(len.child_value()));
    parseLanguages(root, out.audio_languages, out.text_languages);
    for (const pugi::xml_node& g : children(root, "Genre")) out.genres.push_back(attrOr(g, "href", trimmed(g.child_value())));
    parseRatings(root, out.ratings);
    if (const pugi::xml_node pe = child(root, "PrivateExt")) {
        parseRatings(pe, out.ratings);
        for (const pugi::xml_node& ic : children(pe, "ContentIcon")) out.icons.push_back(parseIcon(ic));
        if (const pugi::xml_node comps = child(pe, "Components")) {
            static const std::pair<const char*, const char*> kinds[] = {{"VideoComponent", "video"}, {"AudioComponent", "audio"}, {"CCComponent", "cc"}, {"AppComponent", "app"}};
            for (const auto& [element, kind] : kinds) {
                for (const pugi::xml_node& c : children(comps, element)) {
                    Component comp;
                    comp.kind = kind;
                    comp.language = attrOr(c, "language", "en");
                    comp.text = trimmed(c.child_value());
                    out.components.push_back(std::move(comp));
                }
            }
        }
        for (const pugi::xml_node& cap : children(pe, "Capabilities")) out.capabilities.push_back(serialize(cap));
    }
    return true;
}

std::string pickText(const std::vector<LangText>& texts, std::string_view preferred_lang) {
    for (const LangText& t : texts) {
        if (t.lang.size() >= preferred_lang.size() && t.lang.compare(0, preferred_lang.size(), preferred_lang) == 0 && !t.text.empty()) return t.text;
    }
    for (const LangText& t : texts) {
        if (!t.text.empty()) return t.text;
    }
    return "";
}

// ---- Guide ----

void Guide::clear() {
    services_.clear();
    schedules_.clear();
    contents_.clear();
    services_sorted_.clear();
    programmes_.clear();
    stats_ = GuideStats{};
}

namespace {
template <class T> bool insertVersioned(std::map<std::string, T>& m, T&& v, const std::string& fallback_id) {
    std::string key = v.id.empty() ? fallback_id : v.id;
    auto it = m.find(key);
    if (it != m.end() && it->second.version > v.version) return false;   // older than what we have
    if (it == m.end()) m.emplace(std::move(key), std::move(v));
    else it->second = std::move(v);
    return true;
}
}  // namespace

bool Guide::add(const service::EsgFragment& f, std::string* error) {
    const std::string fallback = std::string(service::toString(f.type)) + "#" + std::to_string(f.bsid) + "/" + std::to_string(f.service_id) + "/" +
                                 std::to_string(f.toi) + "/" + std::to_string(f.transport_id);
    bool ok = false;
    switch (f.type) {
        case service::EsgFragmentType::Service: {
            Service s;
            ok = parseService(f.xml, s, error);
            if (ok) insertVersioned(services_, std::move(s), fallback);
            break;
        }
        case service::EsgFragmentType::Schedule: {
            Schedule s;
            ok = parseSchedule(f.xml, s, error);
            if (ok) insertVersioned(schedules_, std::move(s), fallback);
            break;
        }
        case service::EsgFragmentType::Content: {
            Content c;
            ok = parseContent(f.xml, c, error);
            if (ok) insertVersioned(contents_, std::move(c), fallback);
            break;
        }
        default:
            ++stats_.ignored;
            if (error) *error = "fragment type " + std::string(service::toString(f.type)) + " not used by the guide";
            return false;
    }
    if (!ok) ++stats_.failed;
    return ok;
}

void Guide::build() {
    services_sorted_.clear();
    for (const auto& [id, s] : services_) services_sorted_.push_back(&s);
    std::sort(services_sorted_.begin(), services_sorted_.end(), [](const Service* a, const Service* b) {
        const auto ka = std::make_tuple(a->major.value_or(0xFFFF), a->minor.value_or(0xFFFF), pickText(a->names), a->id);
        const auto kb = std::make_tuple(b->major.value_or(0xFFFF), b->minor.value_or(0xFFFF), pickText(b->names), b->id);
        return ka < kb;
    });
    programmes_.clear();
    stats_.unresolved_contents = 0;
    for (const auto& [id, sch] : schedules_) {
        for (const ContentRef& ref : sch.contents) {
            const auto c = contents_.find(ref.content_id);
            const Content* content = c == contents_.end() ? nullptr : &c->second;
            if (!content) ++stats_.unresolved_contents;
            for (const PresentationWindow& w : ref.windows) {
                Programme p;
                p.service_id = sch.service_id;
                p.content_id = ref.content_id;
                p.schedule_id = sch.id;
                p.start_unix = w.start_unix;
                p.end_unix = w.end_unix;
                p.content = content;
                programmes_.push_back(std::move(p));
            }
        }
    }
    std::sort(programmes_.begin(), programmes_.end(), [](const Programme& a, const Programme& b) {
        return std::tie(a.service_id, a.start_unix, a.end_unix, a.content_id) < std::tie(b.service_id, b.start_unix, b.end_unix, b.content_id);
    });
    // identical windows repeated across schedule versions / fragments collapse to one row
    programmes_.erase(std::unique(programmes_.begin(), programmes_.end(),
                                  [](const Programme& a, const Programme& b) {
                                      return a.service_id == b.service_id && a.start_unix == b.start_unix && a.end_unix == b.end_unix && a.content_id == b.content_id;
                                  }),
                      programmes_.end());
    stats_.services = services_.size();
    stats_.schedules = schedules_.size();
    stats_.contents = contents_.size();
    stats_.programmes = programmes_.size();
}

Guide Guide::fromFragments(const std::vector<service::EsgFragment>& fragments) {
    Guide g;
    for (const service::EsgFragment& f : fragments) g.add(f);
    g.build();
    return g;
}

const Service* Guide::service(const std::string& id) const {
    const auto it = services_.find(id);
    return it == services_.end() ? nullptr : &it->second;
}
const Content* Guide::content(const std::string& id) const {
    const auto it = contents_.find(id);
    return it == contents_.end() ? nullptr : &it->second;
}
const Schedule* Guide::schedule(const std::string& id) const {
    const auto it = schedules_.find(id);
    return it == schedules_.end() ? nullptr : &it->second;
}

std::vector<Programme> Guide::programmes(std::int64_t from_unix, std::int64_t to_unix, const std::string* service_id) const {
    std::vector<Programme> out;
    for (const Programme& p : programmes_) {
        if (service_id && p.service_id != *service_id) continue;
        if (p.end_unix <= from_unix || p.start_unix >= to_unix) continue;
        out.push_back(p);
    }
    return out;
}

std::optional<std::pair<std::int64_t, std::int64_t>> Guide::span() const {
    if (programmes_.empty()) return std::nullopt;
    std::int64_t lo = programmes_.front().start_unix, hi = programmes_.front().end_unix;
    for (const Programme& p : programmes_) {
        lo = std::min(lo, p.start_unix);
        hi = std::max(hi, p.end_unix);
    }
    return std::make_pair(lo, hi);
}

}  // namespace atsc3::esg
