#pragma once
// Stack track description -> VLC es_format_t (spec 5.19.2 "Component" row) and fragment samples -> VLC blocks.
// The in-plugin CMAF/MPU path (allowed by 5.19.2 / Q10): the media layer's decoderConfigs() and fragmentSamples()
// supply what VLC's own mp4 demuxer would derive, so no chained demuxer and no es_out wrapper are needed.
#include <cstdint>
#include <string>

#include <vlc_common.h>
#include <vlc_es.h>

#include "atsc3/media/isobmff.hpp"

namespace atsc3::vlcplugin {

struct EsFormatHints {
    int group = -1;
    int es_id = -1;
    std::string language;      // ISO 639 from the MPD / MP table, overrides the mdhd language when set
    std::string description;   // role / component name shown next to the track
};

/// Fills `fmt` (es_format_Init'ed) from a decoder configuration. False for tracks VLC cannot represent at all
/// (hint/data tracks); unsupported codecs still get an ES so VLC lists them as "no decoder".
bool formatFromConfig(const media::DecoderConfig& dc, const EsFormatHints& hints, es_format_t& fmt);

/// vlc_fourcc_t of a sample entry 4CC.
vlc_fourcc_t fourccOf(const std::string& entry);

}  // namespace atsc3::vlcplugin
