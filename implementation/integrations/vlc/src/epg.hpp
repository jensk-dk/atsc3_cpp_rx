#pragma once
// ESG schedule -> VLC EPG (spec 5.19.2 "ESG schedule/content" row): one vlc_epg_t per program built from the esg
// library's programme index, matched to SLT services by globalServiceID.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <vlc_common.h>
#include <vlc_es_out.h>

#include "atsc3/esg/esg.hpp"
#include "atsc3/service/engine.hpp"

namespace atsc3::vlcplugin {

struct EpgTarget {
    int group = 0;
    std::string global_service_id;                   // joined first (A/332 Service@globalServiceID == SLT)
    std::optional<std::uint16_t> major, minor;       // then by channel number (ATSC3ServiceExtension)
};

/// Pushes the guide's programmes to VLC for every target whose globalServiceID the guide knows. Returns the number of
/// programs that received an EPG; `events` the total events sent.
int pushEpg(es_out_t* out, const esg::Guide& guide, const std::vector<EpgTarget>& targets, std::int64_t now_unix_s, int* events);

}  // namespace atsc3::vlcplugin
