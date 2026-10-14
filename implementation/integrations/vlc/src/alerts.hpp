#pragma once
// AEAT alerts -> VLC (spec 5.19.2 "AEAT alerts" row): each new AEA is logged; with alerts=osd its text is also sent
// on a text ES of the current program for a bounded window.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <vlc_common.h>
#include <vlc_es_out.h>

#include "atsc3/service/engine.hpp"

namespace atsc3::vlcplugin {

struct AlertLine {
    std::string id, type, text;
    std::optional<std::uint8_t> priority;
    bool wakeup = false;
    std::int64_t expires_ns = 0;   // 0 = unknown
};

/// New AEAs of the current LLS tables (those not in `seen`, which it updates). Only AEAT tables are read.
std::vector<AlertLine> newAlerts(const std::vector<service::LlsTableInfo>& tables, std::set<std::string>& seen);

}  // namespace atsc3::vlcplugin
