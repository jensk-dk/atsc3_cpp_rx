#include "epg.hpp"

#include <cstring>

#include <vlc_epg.h>

namespace atsc3::vlcplugin {

int pushEpg(es_out_t* out, const esg::Guide& guide, const std::vector<EpgTarget>& targets, std::int64_t now_unix_s, int* events) {
    int programs = 0;
    if (events) *events = 0;
    for (const EpgTarget& t : targets) {
        const esg::Service* es = nullptr;
        for (const esg::Service* s : guide.services()) {
            if (!t.global_service_id.empty() && s->global_service_id && *s->global_service_id == t.global_service_id) {
                es = s;
                break;
            }
        }
        if (!es && t.major && t.minor) {
            for (const esg::Service* s : guide.services()) {
                if (s->major == t.major && s->minor == t.minor) {
                    es = s;
                    break;
                }
            }
        }
        if (!es) continue;
        const std::vector<esg::Programme> progs = guide.programmes(0, INT64_MAX, &es->id);
        if (progs.empty()) continue;
        vlc_epg_t* epg = vlc_epg_New(static_cast<uint32_t>(t.group), static_cast<uint16_t>(t.group & 0xffff));
        if (!epg) continue;
        uint16_t id = 1;
        for (const esg::Programme& p : progs) {
            if (p.end_unix <= p.start_unix) continue;
            vlc_epg_event_t* ev = vlc_epg_event_New(id++, p.start_unix, static_cast<uint32_t>(p.end_unix - p.start_unix));
            if (!ev) continue;
            const std::string name = p.content ? esg::pickText(p.content->names) : std::string();
            const std::string desc = p.content ? esg::pickText(p.content->descriptions) : std::string();
            ev->psz_name = strdup(name.empty() ? "(no title)" : name.c_str());
            if (!desc.empty()) ev->psz_short_description = strdup(desc.c_str());
            if (p.content && !p.content->ratings.empty()) {
                // vlc_epg_event_t::i_rating is the DVB parental age; ATSC ratings are strings: keep them in the description
                std::string r = p.content->ratings.front().description;
                if (!r.empty()) {
                    std::string full = desc.empty() ? r : desc + " [" + r + "]";
                    free(ev->psz_short_description);
                    ev->psz_short_description = strdup(full.c_str());
                }
            }
            if (!vlc_epg_AddEvent(epg, ev)) vlc_epg_event_Delete(ev);
            else if (events) ++*events;
        }
        es_out_Control(out, ES_OUT_SET_GROUP_EPG, t.group, epg);
        vlc_epg_Delete(epg);
        ++programs;
    }
    if (programs) es_out_Control(out, ES_OUT_SET_EPG_TIME, static_cast<int64_t>(now_unix_s));
    return programs;
}

}  // namespace atsc3::vlcplugin
