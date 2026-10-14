#include "xml/xml.hpp"

#include <cstdlib>
#include <ctime>
#include <sstream>

namespace atsc3::xml {

bool load(pugi::xml_document& doc, ByteView bytes, std::string* error) {
    std::size_t off = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) off = 3;
    while (off < bytes.size() && (bytes[off] == ' ' || bytes[off] == '\n' || bytes[off] == '\r' || bytes[off] == '\t')) ++off;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.data() + off, bytes.size() - off, pugi::parse_default, pugi::encoding_utf8);
    if (!r) {
        if (error) *error = r.description();
        return false;
    }
    return true;
}

std::string_view localName(const char* q) noexcept {
    std::string_view s(q ? q : "");
    const auto pos = s.find(':');
    return pos == std::string_view::npos ? s : s.substr(pos + 1);
}

bool nameIs(const pugi::xml_node& n, std::string_view local) noexcept { return localName(n.name()) == local; }

pugi::xml_node child(const pugi::xml_node& n, std::string_view local) {
    for (pugi::xml_node c : n.children()) {
        if (c.type() == pugi::node_element && nameIs(c, local)) return c;
    }
    return {};
}

std::vector<pugi::xml_node> children(const pugi::xml_node& n, std::string_view local) {
    std::vector<pugi::xml_node> v;
    for (pugi::xml_node c : n.children()) {
        if (c.type() == pugi::node_element && nameIs(c, local)) v.push_back(c);
    }
    return v;
}

pugi::xml_node root(const pugi::xml_document& doc, std::string_view local) {
    const pugi::xml_node r = doc.document_element();
    return nameIs(r, local) ? r : pugi::xml_node();
}

std::optional<std::string> attr(const pugi::xml_node& n, std::string_view local) {
    for (pugi::xml_attribute a : n.attributes()) {
        const std::string_view name(a.name());
        if (name == local) return std::string(a.value());
    }
    for (pugi::xml_attribute a : n.attributes()) {
        if (localName(a.name()) == local && std::string_view(a.name()).rfind("xmlns", 0) != 0) return std::string(a.value());
    }
    return std::nullopt;
}

std::string attrOr(const pugi::xml_node& n, std::string_view local, const std::string& def) {
    const auto v = attr(n, local);
    return v ? *v : def;
}

bool attrBool(const pugi::xml_node& n, std::string_view local, bool def) {
    const auto v = attr(n, local);
    if (!v) return def;
    return *v == "true" || *v == "1";
}

std::optional<std::uint32_t> attrUInt(const pugi::xml_node& n, std::string_view local) {
    const auto v = attr(n, local);
    if (!v || v->empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long long x = std::strtoull(v->c_str(), &end, 10);
    if (end == v->c_str() || x > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<std::uint32_t>(x);
}

std::uint32_t attrUIntOr(const pugi::xml_node& n, std::string_view local, std::uint32_t def) {
    const auto v = attrUInt(n, local);
    return v ? *v : def;
}

std::optional<std::int64_t> attrInt(const pugi::xml_node& n, std::string_view local) {
    const auto v = attr(n, local);
    if (!v || v->empty()) return std::nullopt;
    char* end = nullptr;
    const long long x = std::strtoll(v->c_str(), &end, 10);
    if (end == v->c_str()) return std::nullopt;
    return static_cast<std::int64_t>(x);
}

std::optional<double> attrDouble(const pugi::xml_node& n, std::string_view local) {
    const auto v = attr(n, local);
    if (!v || v->empty()) return std::nullopt;
    char* end = nullptr;
    const double x = std::strtod(v->c_str(), &end);
    if (end == v->c_str()) return std::nullopt;
    return x;
}

std::optional<std::string> lang(const pugi::xml_node& n) {
    if (const auto v = attr(n, "lang")) return v;
    return std::nullopt;
}

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
        const std::size_t start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
        if (i > start) out.emplace_back(text.substr(start, i - start));
    }
    return out;
}

std::vector<std::uint32_t> splitUIntList(std::string_view text) {
    std::vector<std::uint32_t> out;
    for (const std::string& s : splitList(text)) {
        char* end = nullptr;
        const unsigned long long x = std::strtoull(s.c_str(), &end, 10);
        if (end != s.c_str() && x <= 0xFFFFFFFFull) out.push_back(static_cast<std::uint32_t>(x));
    }
    return out;
}

std::optional<std::int64_t> parseDateTimeNs(std::string_view t) {
    // YYYY-MM-DDThh:mm:ss[.fff...][Z|(+|-)hh:mm]
    if (t.size() < 19 || t[4] != '-' || t[7] != '-' || (t[10] != 'T' && t[10] != ' ') || t[13] != ':' || t[16] != ':') return std::nullopt;
    auto num = [&](std::size_t pos, std::size_t len) -> std::optional<int> {
        int v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const char c = t[pos + i];
            if (c < '0' || c > '9') return std::nullopt;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    const auto Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), m = num(14, 2), s = num(17, 2);
    if (!Y || !M || !D || !h || !m || !s) return std::nullopt;
    std::size_t i = 19;
    std::int64_t frac_ns = 0;
    if (i < t.size() && t[i] == '.') {
        ++i;
        std::int64_t scale = 100'000'000;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            frac_ns += (t[i] - '0') * scale;
            scale /= 10;
            ++i;
        }
    }
    std::int64_t zone_s = 0;
    if (i < t.size()) {
        if (t[i] == 'Z') {
            ++i;
        } else if ((t[i] == '+' || t[i] == '-') && i + 6 <= t.size() && t[i + 3] == ':') {
            const auto zh = num(i + 1, 2), zm = num(i + 4, 2);
            if (!zh || !zm) return std::nullopt;
            zone_s = (*zh * 3600 + *zm * 60) * (t[i] == '+' ? 1 : -1);
            i += 6;
        } else {
            return std::nullopt;
        }
    }
    if (i != t.size()) return std::nullopt;
    std::tm tm{};
    tm.tm_year = *Y - 1900;
    tm.tm_mon = *M - 1;
    tm.tm_mday = *D;
    tm.tm_hour = *h;
    tm.tm_min = *m;
    tm.tm_sec = *s;
    const std::time_t utc = timegm(&tm);
    if (utc == static_cast<std::time_t>(-1)) return std::nullopt;
    return (static_cast<std::int64_t>(utc) - zone_s) * 1'000'000'000LL + frac_ns;
}

std::optional<double> parseDurationSeconds(std::string_view t) {
    std::size_t i = 0;
    double sign = 1.0;
    if (i < t.size() && t[i] == '-') {
        sign = -1.0;
        ++i;
    }
    if (i >= t.size() || t[i] != 'P') return std::nullopt;
    ++i;
    bool time_part = false;
    double total = 0.0;
    bool any = false;
    while (i < t.size()) {
        if (t[i] == 'T') {
            time_part = true;
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.')) ++i;
        if (i == start || i >= t.size()) return std::nullopt;
        const double v = std::strtod(std::string(t.substr(start, i - start)).c_str(), nullptr);
        const char unit = t[i++];
        if (!time_part) {
            if (unit == 'D') total += v * 86400.0;
            else return std::nullopt;  // Y and M are not supported (ambiguous)
        } else {
            if (unit == 'H') total += v * 3600.0;
            else if (unit == 'M') total += v * 60.0;
            else if (unit == 'S') total += v;
            else return std::nullopt;
        }
        any = true;
    }
    if (!any) return std::nullopt;
    return sign * total;
}

std::string serialize(const pugi::xml_node& n) {
    std::ostringstream os;
    n.print(os, "", pugi::format_raw | pugi::format_no_declaration);
    return os.str();
}

std::string namespaceOf(const pugi::xml_node& n) {
    const std::string_view q(n.name());
    const auto pos = q.find(':');
    const std::string want = pos == std::string_view::npos ? "xmlns" : "xmlns:" + std::string(q.substr(0, pos));
    for (pugi::xml_node cur = n; cur; cur = cur.parent()) {
        if (const pugi::xml_attribute a = cur.attribute(want.c_str())) return a.value();
    }
    return "";
}

}  // namespace atsc3::xml
