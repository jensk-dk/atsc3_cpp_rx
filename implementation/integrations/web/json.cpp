#include "atsc3/web/json.hpp"

#include <cmath>
#include <cstdio>

namespace atsc3::web {

std::string Json::escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o.push_back('"');
    auto emit = [&](unsigned char c) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(static_cast<char>(c));
                }
        }
    };
    // Validate UTF-8 so binary payloads cannot produce invalid JSON; bad bytes become U+FFFD.
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if (c < 0x80) n = 1;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) n = 4;
        bool ok = n > 0 && i + n <= s.size();
        for (std::size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            o += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        if (n == 1) emit(c);
        else o.append(s, i, n);
        i += n;
    }
    o.push_back('"');
    return o;
}

void Json::sep() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_.push_back(',');
        first_.back() = false;
    }
}
Json& Json::beginObject() {
    sep();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}
Json& Json::endObject() {
    out_.push_back('}');
    first_.pop_back();
    return *this;
}
Json& Json::beginArray() {
    sep();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}
Json& Json::endArray() {
    out_.push_back(']');
    first_.pop_back();
    return *this;
}
Json& Json::key(const std::string& k) {
    sep();
    out_ += escape(k);
    out_.push_back(':');
    after_key_ = true;
    return *this;
}
Json& Json::value(const std::string& s) {
    sep();
    out_ += escape(s);
    return *this;
}
Json& Json::value(const char* s) { return value(std::string(s)); }
Json& Json::value(bool b) {
    sep();
    out_ += b ? "true" : "false";
    return *this;
}
Json& Json::value(double d) {
    sep();
    if (!std::isfinite(d)) {
        out_ += "null";
        return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    out_ += buf;
    return *this;
}
Json& Json::value(std::int64_t v) {
    sep();
    out_ += std::to_string(v);
    return *this;
}
Json& Json::value(std::uint64_t v) {
    sep();
    out_ += std::to_string(v);
    return *this;
}
Json& Json::null() {
    sep();
    out_ += "null";
    return *this;
}
Json& Json::raw(const std::string& json) {
    sep();
    out_ += json;
    return *this;
}

}  // namespace atsc3::web
