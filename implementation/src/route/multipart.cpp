#include "atsc3/route/multipart.hpp"

#include <algorithm>
#include <cctype>

namespace atsc3::route {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}
// Finds "\n" (accepting "\r\n"); returns position of the line end and sets next line start.
std::size_t lineEnd(ByteView d, std::size_t from, std::size_t& next) {
    for (std::size_t i = from; i < d.size(); ++i) {
        if (d[i] == '\n') {
            next = i + 1;
            return (i > from && d[i - 1] == '\r') ? i - 1 : i;
        }
    }
    next = d.size();
    return d.size();
}
std::size_t find(ByteView d, std::size_t from, const std::string& needle) {
    if (needle.empty() || d.size() < needle.size()) return std::string::npos;
    for (std::size_t i = from; i + needle.size() <= d.size(); ++i) {
        if (d[i] == static_cast<std::uint8_t>(needle[0]) &&
            std::equal(needle.begin(), needle.end(), d.begin() + static_cast<std::ptrdiff_t>(i),
                       [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }))
            return i;
    }
    return std::string::npos;
}
}  // namespace

std::optional<std::string> EntityHeaders::get(const std::string& name) const {
    const auto it = fields.find(lower(name));
    if (it == fields.end()) return std::nullopt;
    return it->second;
}

std::string EntityHeaders::contentType() const {
    const auto ct = get("content-type");
    if (!ct) return "";
    const auto semi = ct->find(';');
    return lower(trim(semi == std::string::npos ? *ct : ct->substr(0, semi)));
}

std::optional<std::string> EntityHeaders::contentTypeParam(const std::string& name) const {
    const auto ct = get("content-type");
    if (!ct) return std::nullopt;
    const std::string want = lower(name);
    std::size_t pos = ct->find(';');
    while (pos != std::string::npos) {
        std::size_t next = ct->find(';', pos + 1);
        const std::string param = ct->substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        const auto eq = param.find('=');
        if (eq != std::string::npos && lower(trim(param.substr(0, eq))) == want) return unquote(param.substr(eq + 1));
        pos = next;
    }
    return std::nullopt;
}

std::size_t parseEntityHeaders(ByteView d, EntityHeaders& out) {
    out = EntityHeaders{};
    std::size_t pos = 0;
    std::string name, value;
    bool any = false;
    auto flush = [&] {
        if (!name.empty()) {
            out.fields[lower(name)] = trim(value);
            any = true;
        }
        name.clear();
        value.clear();
    };
    while (pos < d.size()) {
        std::size_t next;
        const std::size_t end = lineEnd(d, pos, next);
        if (end == pos) {  // empty line terminates the header block
            flush();
            return any ? next : 0;
        }
        const std::string line(reinterpret_cast<const char*>(d.data() + pos), end - pos);
        if (line[0] == ' ' || line[0] == '\t') {  // folded continuation
            if (name.empty()) return 0;
            value += " " + trim(line);
        } else {
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                // not a header line: a block without a blank-line terminator ends here
                flush();
                return any ? pos : 0;
            }
            flush();
            name = trim(line.substr(0, colon));
            value = line.substr(colon + 1);
            for (char c : name) {
                if (c <= ' ' || c > '~') return 0;
            }
        }
        pos = next;
    }
    flush();
    return any ? pos : 0;
}

bool parseMultipart(ByteView d, Multipart& out, std::string* error) {
    out = Multipart{};
    std::size_t body_start = 0;
    EntityHeaders top;
    const std::size_t consumed = parseEntityHeaders(d, top);
    std::string boundary;
    if (consumed > 0 && top.get("content-type")) {
        out.headers = top;
        body_start = consumed;
        if (const auto b = top.contentTypeParam("boundary")) boundary = *b;
        out.is_signed = top.contentType() == "multipart/signed";
    }
    if (boundary.empty()) {
        // No usable top header: take the boundary from the first "--xxx" line.
        std::size_t next;
        const std::size_t end = lineEnd(d, body_start, next);
        if (end <= body_start + 2 || d[body_start] != '-' || d[body_start + 1] != '-') {
            if (error) *error = "no multipart boundary";
            return false;
        }
        boundary.assign(reinterpret_cast<const char*>(d.data() + body_start + 2), end - body_start - 2);
    }
    out.boundary = boundary;
    const std::string delim = "--" + boundary;
    std::size_t pos = find(d, body_start, delim);
    if (pos == std::string::npos) {
        if (error) *error = "boundary not found";
        return false;
    }
    for (;;) {
        pos += delim.size();
        if (pos + 2 <= d.size() && d[pos] == '-' && d[pos + 1] == '-') break;  // closing delimiter
        std::size_t next;
        lineEnd(d, pos, next);  // skip to end of boundary line
        pos = next;
        const std::size_t part_end = find(d, pos, "\n" + delim);
        std::size_t body_end = part_end == std::string::npos ? d.size() : part_end;
        if (body_end > pos && d[body_end - 1] == '\r') --body_end;
        MimePart part;
        const ByteView part_view(d.data() + pos, body_end - pos);
        std::size_t hl = parseEntityHeaders(part_view, part.headers);
        if (hl == 0) {  // no header fields: the part still starts with the empty separator line
            if (part_view.size() >= 2 && part_view[0] == '\r' && part_view[1] == '\n') hl = 2;
            else if (!part_view.empty() && part_view[0] == '\n') hl = 1;
        }
        part.body = part_view.subview(hl);
        out.parts.push_back(part);
        if (part_end == std::string::npos) break;
        pos = part_end + 1;
    }
    if (out.parts.empty()) {
        if (error) *error = "no parts";
        return false;
    }
    return true;
}

}  // namespace atsc3::route
