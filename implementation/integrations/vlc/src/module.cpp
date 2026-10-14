// VLC module descriptor and option table for the atsc3:// access-demux (spec 5.19.1, 5.19.4). VLC 3.0 line: stream-less
// demuxers keyed by the URL scheme register the "access_demux" capability (VLC 4 merged it into "demux").
#include <cstdlib>
#include <string>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_variables.h>

#include "demux.hpp"

static int Open(vlc_object_t*);
static void Close(vlc_object_t*);

#define SERVICE_TEXT "Initial service"
#define SERVICE_LONGTEXT "Service selected at start: a serviceId or a major.minor channel number. Default: the first A/V service, or VLC's --program."
#define PACING_TEXT "Pacing"
#define PACING_LONGTEXT "realtime replays the capture at broadcast pace; nonrealtime feeds as fast as VLC takes it (for --sout dumps)."
#define LOOP_TEXT "Loop capture"
#define LOOP_LONGTEXT "Replay the capture file endlessly."
#define SPEED_TEXT "Replay speed"
#define SPEED_LONGTEXT "Real-time pacing multiplier."
#define START_TEXT "Start offset (s)"
#define START_LONGTEXT "Seconds of the capture to skip before delivery starts."
#define SIDECAR_TEXT "Sidecar"
#define SIDECAR_LONGTEXT "DekTec .xa3 profile beside the capture: auto, off or a path."
#define ESG_TEXT "Feed the EPG"
#define ESG_LONGTEXT "Open the ESG services and push their schedule as VLC EPG."
#define ALERTS_TEXT "Alerts"
#define ALERTS_LONGTEXT "osd (text track), log or off."
#define DELAY_TEXT "Live delay (ms)"
#define DELAY_LONGTEXT "Distance between delivery and presentation. 0 = twice the longest segment."
#define STATS_TEXT "Verbose statistics"
#define STATS_LONGTEXT "Log stack counters every few seconds."
#define HIDDEN_TEXT "Show hidden services"
#define HIDDEN_LONGTEXT "List ESG, EA and hidden services as programs too."

vlc_module_begin()
    set_shortname("ATSC3")
    set_description("ATSC 3.0 receiver (atsc3:// captures)")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access_demux", 10)
    add_shortcut("atsc3")
    add_string("atsc3-service", "", SERVICE_TEXT, SERVICE_LONGTEXT, false)
    add_string("atsc3-pacing", "realtime", PACING_TEXT, PACING_LONGTEXT, false)
    add_bool("atsc3-loop", false, LOOP_TEXT, LOOP_LONGTEXT, false)
    add_float("atsc3-speed", 1.0, SPEED_TEXT, SPEED_LONGTEXT, true)
    add_float("atsc3-start", 0.0, START_TEXT, START_LONGTEXT, true)
    add_string("atsc3-sidecar", "auto", SIDECAR_TEXT, SIDECAR_LONGTEXT, true)
    add_bool("atsc3-esg", true, ESG_TEXT, ESG_LONGTEXT, true)
    add_string("atsc3-alerts", "log", ALERTS_TEXT, ALERTS_LONGTEXT, true)
    add_integer("atsc3-live-delay", 0, DELAY_TEXT, DELAY_LONGTEXT, true)
    add_bool("atsc3-verbose-stats", false, STATS_TEXT, STATS_LONGTEXT, true)
    add_bool("atsc3-show-hidden", false, HIDDEN_TEXT, HIDDEN_LONGTEXT, true)
    set_callbacks(Open, Close)
vlc_module_end()

namespace {
std::string inheritString(vlc_object_t* obj, const char* name) {
    char* v = var_InheritString(obj, name);
    std::string s = v ? v : "";
    free(v);
    return s;
}
}  // namespace

static int Demux(demux_t* d) { return d->p_sys->impl->demux(); }
static int Control(demux_t* d, int query, va_list args) { return d->p_sys->impl->control(query, args); }

static int Open(vlc_object_t* obj) {
    demux_t* d = reinterpret_cast<demux_t*>(obj);
    if (d->b_preparsing) return VLC_EGENERIC;   // the capture is not opened for metadata probing
    if (!d->psz_location || !*d->psz_location) return VLC_EGENERIC;
    atsc3::vlcplugin::Mrl mrl = atsc3::vlcplugin::parseMrl(d->psz_location);
    if (mrl.kind == atsc3::vlcplugin::Mrl::Kind::Invalid) {
        msg_Err(d, "atsc3: error: %s", mrl.error.c_str());
        return VLC_EGENERIC;
    }
    atsc3::vlcplugin::Options o;
    o.service = inheritString(obj, "atsc3-service");
    o.realtime = inheritString(obj, "atsc3-pacing") != "nonrealtime";
    o.loop = var_InheritBool(obj, "atsc3-loop");
    o.speed = static_cast<double>(var_InheritFloat(obj, "atsc3-speed"));
    o.start_s = static_cast<double>(var_InheritFloat(obj, "atsc3-start"));
    o.sidecar = inheritString(obj, "atsc3-sidecar");
    o.esg = var_InheritBool(obj, "atsc3-esg");
    o.alerts = inheritString(obj, "atsc3-alerts");
    o.live_delay_ms = static_cast<int>(var_InheritInteger(obj, "atsc3-live-delay"));
    o.verbose_stats = var_InheritBool(obj, "atsc3-verbose-stats");
    o.show_hidden = var_InheritBool(obj, "atsc3-show-hidden");
    o.program = static_cast<int>(var_InheritInteger(obj, "program"));
    std::string unknown;
    atsc3::vlcplugin::applyOptions(mrl, o, &unknown);
    if (!unknown.empty()) msg_Warn(d, "atsc3: unknown MRL options ignored: %s", unknown.c_str());
    auto* sys = new (std::nothrow) demux_sys_t;
    if (!sys) return VLC_ENOMEM;
    sys->impl = new (std::nothrow) atsc3::vlcplugin::Demux(d, std::move(mrl), std::move(o));
    if (!sys->impl) {
        delete sys;
        return VLC_ENOMEM;
    }
    std::string err;
    if (!sys->impl->open(&err)) {
        msg_Err(d, "atsc3: error: %s", err.c_str());
        delete sys->impl;
        delete sys;
        return VLC_EGENERIC;
    }
    d->p_sys = sys;
    d->pf_demux = Demux;
    d->pf_control = Control;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t* obj) {
    demux_t* d = reinterpret_cast<demux_t*>(obj);
    if (!d->p_sys) return;
    delete d->p_sys->impl;
    delete d->p_sys;
    d->p_sys = nullptr;
}
