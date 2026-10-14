#include "alerts.hpp"

#include "atsc3/lls/tables.hpp"

namespace atsc3::vlcplugin {

namespace {
std::string pick(const std::vector<lls::LangText>& texts) {
    for (const lls::LangText& t : texts)
        if (t.lang.rfind("en", 0) == 0 && !t.text.empty()) return t.text;
    return texts.empty() ? std::string() : texts.front().text;
}
}  // namespace

std::vector<AlertLine> newAlerts(const std::vector<service::LlsTableInfo>& tables, std::set<std::string>& seen) {
    std::vector<AlertLine> out;
    for (const service::LlsTableInfo& t : tables) {
        if (t.name != "AEAT") continue;
        lls::Aeat aeat;
        if (!lls::parseAeat(t.xml, aeat)) continue;
        for (const lls::Aea& a : aeat.aeas) {
            const std::string key = std::to_string(t.group) + "/" + a.aea_id + "/" + a.aea_type + "/" + (a.effective_ns ? std::to_string(*a.effective_ns) : "-");
            if (!seen.insert(key).second) continue;
            AlertLine line;
            line.id = a.aea_id;
            line.type = a.aea_type;
            line.priority = a.priority;
            line.wakeup = a.wakeup;
            line.expires_ns = a.expires_ns.value_or(0);
            std::string text = pick(a.aea_text);
            if (text.empty()) text = pick(a.event_desc);
            if (text.empty() && a.event_code) text = *a.event_code;
            line.text = text;
            out.push_back(std::move(line));
        }
    }
    return out;
}

}  // namespace atsc3::vlcplugin
