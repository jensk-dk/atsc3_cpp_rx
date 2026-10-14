#include "mrl.hpp"

#include <cctype>
#include <cstdlib>
#include <sys/stat.h>

namespace atsc3::vlcplugin {

namespace {
std::string percentDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}
bool fileExists(const std::string& p) {
    struct stat st {};
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
}  // namespace

Mrl parseMrl(const std::string& location) {
    Mrl m;
    std::string base = location;
    const auto q = base.find('?');
    std::string query;
    if (q != std::string::npos) {
        query = base.substr(q + 1);
        base.erase(q);
    }
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string term = amp == std::string::npos ? query : query.substr(0, amp);
        query = amp == std::string::npos ? "" : query.substr(amp + 1);
        if (term.empty()) continue;
        const auto eq = term.find('=');
        const std::string key = percentDecode(eq == std::string::npos ? term : term.substr(0, eq));
        const std::string value = eq == std::string::npos ? "1" : percentDecode(term.substr(eq + 1));
        m.options[key] = value;
    }
    base = percentDecode(base);
    if (base.empty()) {
        m.error = "empty atsc3:// location";
        return m;
    }
    if (base.rfind("udp@", 0) == 0) {
        m.kind = Mrl::Kind::Udp;
        m.target = base.substr(4);
        return m;
    }
    if (base.rfind("tuner/", 0) == 0) {
        m.kind = Mrl::Kind::Tuner;
        m.target = base.substr(6);
        return m;
    }
    // VLC's MRL split hands "host/path" for atsc3://host/path and "/path" for atsc3:///path
    if (!fileExists(base) && base.front() != '/' && fileExists("/" + base)) base = "/" + base;
    m.kind = Mrl::Kind::Capture;
    m.target = base;
    return m;
}

void applyOptions(const Mrl& mrl, Options& o, std::string* unknown) {
    for (const auto& [k, v] : mrl.options) {
        if (k == "service" || k == "channel") o.service = v;
        else if (k == "bsid") o.bsid = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "pacing") o.realtime = v != "nonrealtime" && v != "nrt";
        else if (k == "speed") o.speed = std::strtod(v.c_str(), nullptr);
        else if (k == "loop") o.loop = truthy(v);
        else if (k == "start") o.start_s = std::strtod(v.c_str(), nullptr);
        else if (k == "sidecar") o.sidecar = v;
        else if (k == "esg") o.esg = truthy(v);
        else if (k == "alerts") o.alerts = v;
        else if (k == "live-delay") o.live_delay_ms = static_cast<int>(std::strtol(v.c_str(), nullptr, 10));
        else if (k == "verbose-stats") o.verbose_stats = truthy(v);
        else if (k == "show-hidden") o.show_hidden = truthy(v);
        else if (k == "iface" || k == "freq" || k == "plp") continue;   // UDP / tuner terms, reserved
        else if (unknown) {
            if (!unknown->empty()) *unknown += ", ";
            *unknown += k;
        }
    }
    if (o.speed <= 0.0) o.speed = 1.0;
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parseChannel(const std::string& text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) return std::nullopt;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != dot && !std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
    return std::make_pair(static_cast<std::uint16_t>(std::strtoul(text.substr(0, dot).c_str(), nullptr, 10)),
                          static_cast<std::uint16_t>(std::strtoul(text.substr(dot + 1).c_str(), nullptr, 10)));
}

}  // namespace atsc3::vlcplugin
