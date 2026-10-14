#include "model.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace atsc3::stb {

std::string Channel::number() const {
    if (major && minor) return std::to_string(*major) + "." + std::to_string(*minor);
    return "#" + std::to_string(service_id);
}

std::string Channel::label() const { return number() + " " + name; }

namespace {
bool hasAv(const service::ServiceInfo& s) {
    if (s.protocol != "ROUTE" && s.protocol != "MMTP") return false;
    if (s.category == 1 || s.category == 2) return true;
    for (const service::ComponentInfo& c : s.components)
        if (c.kind == "video" || c.kind == "audio") return true;
    return false;
}
std::string nameOf(const service::ServiceInfo& s) {
    if (!s.short_name.empty()) return s.short_name;
    if (!s.usbd_name.empty()) return s.usbd_name;
    return "service " + std::to_string(s.service_id);
}
}  // namespace

std::vector<Channel> channelsOf(const std::vector<service::ServiceInfo>& services, bool show_hidden) {
    std::vector<Channel> out;
    for (const service::ServiceInfo& s : services) {
        if (!hasAv(s)) continue;
        if (s.hidden && !show_hidden) continue;
        Channel c;
        c.bsid = s.bsid;
        c.service_id = s.service_id;
        c.major = s.major;
        c.minor = s.minor;
        c.name = nameOf(s);
        c.protocol = s.protocol;
        c.global_service_id = s.global_service_id;
        c.protected_flag = s.protected_flag;
        c.hidden = s.hidden;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Channel& a, const Channel& b) {
        const auto ka = std::make_tuple(a.major.value_or(0xffff), a.minor.value_or(0xffff), a.bsid, a.service_id);
        const auto kb = std::make_tuple(b.major.value_or(0xffff), b.minor.value_or(0xffff), b.bsid, b.service_id);
        return ka < kb;
    });
    return out;
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parseChannelNumber(const std::string& text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) return std::nullopt;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != dot && (text[i] < '0' || text[i] > '9')) return std::nullopt;
    const unsigned long a = std::strtoul(text.substr(0, dot).c_str(), nullptr, 10);
    const unsigned long b = std::strtoul(text.substr(dot + 1).c_str(), nullptr, 10);
    if (a > 0xffff || b > 0xffff) return std::nullopt;
    return std::make_pair(static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b));
}

std::optional<Channel> findChannel(const std::vector<Channel>& channels, const std::string& spec) {
    if (spec.empty()) return std::nullopt;
    if (const auto num = parseChannelNumber(spec)) {
        for (const Channel& c : channels)
            if (c.major == num->first && c.minor == num->second) return c;
        return std::nullopt;
    }
    for (const char ch : spec)
        if (ch < '0' || ch > '9') return std::nullopt;
    const unsigned long id = std::strtoul(spec.c_str(), nullptr, 10);
    for (const Channel& c : channels)
        if (c.service_id == id) return c;
    return std::nullopt;
}

NowNext nowNext(const esg::Guide& guide, const Channel& ch, std::int64_t now_unix) {
    NowNext out;
    const esg::Service* es = nullptr;
    if (ch.global_service_id) {
        for (const esg::Service* s : guide.services())
            if (s->global_service_id && *s->global_service_id == *ch.global_service_id) {
                es = s;
                break;
            }
    }
    if (!es && ch.major && ch.minor) {
        for (const esg::Service* s : guide.services())
            if (s->major == ch.major && s->minor == ch.minor) {
                es = s;
                break;
            }
    }
    if (!es) return out;
    const std::vector<esg::Programme> progs = guide.programmes(0, INT64_MAX, &es->id);   // sorted by start
    const auto titleOf = [](const esg::Programme& p) {
        const std::string t = p.content ? esg::pickText(p.content->names) : std::string();
        return t.empty() ? std::string("(no title)") : t;
    };
    for (const esg::Programme& p : progs) {
        if (p.end_unix <= p.start_unix) continue;
        if (p.start_unix <= now_unix && now_unix < p.end_unix) {
            if (!out.now) out.now = Programme{titleOf(p), p.start_unix, p.end_unix};
        } else if (p.start_unix > now_unix) {
            if (!out.next || p.start_unix < out.next->start_unix) out.next = Programme{titleOf(p), p.start_unix, p.end_unix};
        }
    }
    return out;
}

Key keyOf(const std::string& name) {
    if (name == "Up" || name == "KP_Up") return Key::Up;
    if (name == "Down" || name == "KP_Down") return Key::Down;
    if (name == "Page_Up" || name == "KP_Page_Up" || name == "plus" || name == "KP_Add") return Key::PageUp;
    if (name == "Page_Down" || name == "KP_Page_Down" || name == "minus" || name == "KP_Subtract") return Key::PageDown;
    if (name == "Return" || name == "KP_Enter" || name == "space") return Key::Select;
    if (name == "Escape" || name == "BackSpace") return Key::Back;
    if (name == "l" || name == "L" || name == "Menu" || name == "Tab") return Key::List;
    if (name == "i" || name == "I") return Key::Info;
    if (name == "q" || name == "Q") return Key::Quit;
    return Key::None;
}

// ---- UiModel ------------------------------------------------------------------------------------------------------

int UiModel::indexOf(const Channel& ch) const {
    for (std::size_t i = 0; i < channels_.size(); ++i)
        if (channels_[i].sameService(ch)) return static_cast<int>(i);
    return -1;
}

void UiModel::setChannels(std::vector<Channel> channels) {
    const std::optional<Channel> tuned = tuned_ >= 0 && tuned_ < static_cast<int>(channels_.size()) ? std::optional<Channel>(channels_[static_cast<std::size_t>(tuned_)]) : std::nullopt;
    const std::optional<Channel> hl = highlight_ >= 0 && highlight_ < static_cast<int>(channels_.size()) ? std::optional<Channel>(channels_[static_cast<std::size_t>(highlight_)]) : std::nullopt;
    channels_ = std::move(channels);
    tuned_ = tuned ? indexOf(*tuned) : -1;
    highlight_ = hl ? indexOf(*hl) : -1;
    if (highlight_ < 0 && !channels_.empty()) highlight_ = tuned_ >= 0 ? tuned_ : 0;
}

std::optional<Channel> UiModel::tuned() const {
    if (tuned_ < 0 || tuned_ >= static_cast<int>(channels_.size())) return std::nullopt;
    return channels_[static_cast<std::size_t>(tuned_)];
}

void UiModel::setTuned(const Channel& ch, double now_s) {
    tuned_ = indexOf(ch);
    if (highlight_ < 0) highlight_ = tuned_;
    showBanner(now_s);
}

Action UiModel::tuneTo(int index, double now_s) {
    Action a;
    if (channels_.empty()) return a;
    const int n = static_cast<int>(channels_.size());
    index = ((index % n) + n) % n;
    a.kind = Action::Kind::Tune;
    a.channel = channels_[static_cast<std::size_t>(index)];
    highlight_ = index;
    showBanner(now_s);
    return a;
}

Action UiModel::onKey(Key key, double now_s) {
    Action none;
    if (key == Key::Quit) {
        none.kind = Action::Kind::Quit;
        return none;
    }
    const int n = static_cast<int>(channels_.size());
    if (list_visible_) {
        switch (key) {
            case Key::Up: if (highlight_ > 0) --highlight_; return none;
            case Key::Down: if (highlight_ + 1 < n) ++highlight_; return none;
            case Key::PageUp: highlight_ = std::max(0, highlight_ - page_rows); return none;
            case Key::PageDown: highlight_ = std::min(n - 1, highlight_ + page_rows); return none;
            case Key::Select: {
                list_visible_ = false;
                if (highlight_ < 0 || highlight_ >= n) return none;
                if (highlight_ == tuned_) {   // already on it: just the banner
                    showBanner(now_s);
                    return none;
                }
                return tuneTo(highlight_, now_s);
            }
            case Key::Back:
            case Key::List: list_visible_ = false; return none;
            default: return none;
        }
    }
    switch (key) {
        case Key::Up:
        case Key::PageUp: return n ? tuneTo((tuned_ < 0 ? -1 : tuned_) + 1, now_s) : none;
        case Key::Down:
        case Key::PageDown: return n ? tuneTo((tuned_ < 0 ? n : tuned_) - 1, now_s) : none;
        case Key::Select:
        case Key::List: showList(); return none;
        case Key::Info: showBanner(now_s); return none;
        case Key::Back: hideBanner(); return none;
        default: return none;
    }
}

std::string clockText(std::int64_t unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    if (!localtime_r(&t, &tm)) return "--:--";
    char buf[8];
    if (std::strftime(buf, sizeof buf, "%H:%M", &tm) == 0) return "--:--";
    return buf;
}

}  // namespace atsc3::stb
