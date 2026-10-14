#include "es_mapper.hpp"

#include <cstdlib>
#include <cstring>

#include <vlc_fourcc.h>

namespace atsc3::vlcplugin {

vlc_fourcc_t fourccOf(const std::string& entry) {
    char c[4] = {' ', ' ', ' ', ' '};
    for (std::size_t i = 0; i < 4 && i < entry.size(); ++i) c[i] = entry[i];
    return VLC_FOURCC(c[0], c[1], c[2], c[3]);
}

namespace {
vlc_fourcc_t codecOf(const std::string& entry) {
    if (entry == "hev1" || entry == "hvc1" || entry == "dvhe" || entry == "dvh1") return VLC_CODEC_HEVC;
    if (entry == "avc1" || entry == "avc3") return VLC_CODEC_H264;
    if (entry == "av01") return VLC_CODEC_AV1;
    if (entry == "vp09") return VLC_CODEC_VP9;
    if (entry == "mp4a") return VLC_CODEC_MP4A;
    if (entry == "ec-3") return VLC_CODEC_EAC3;
    if (entry == "ac-3") return VLC_CODEC_A52;
    if (entry == "Opus") return VLC_CODEC_OPUS;
    if (entry == "stpp") return VLC_CODEC_TTML;
    if (entry == "wvtt") return VLC_CODEC_WEBVTT;
    return fourccOf(entry);   // ac-4, mhm1/mhm2 ...: listed, VLC 3 has no decoder
}
char* dupOrNull(const std::string& s) { return s.empty() ? nullptr : strdup(s.c_str()); }
}  // namespace

bool formatFromConfig(const media::DecoderConfig& dc, const EsFormatHints& hints, es_format_t& fmt) {
    const std::string kind = dc.track.kind();
    const std::string& entry = dc.original_entry;
    if (kind == "video") es_format_Init(&fmt, VIDEO_ES, codecOf(entry));
    else if (kind == "audio") es_format_Init(&fmt, AUDIO_ES, codecOf(entry));
    else if (kind == "subtitles") es_format_Init(&fmt, SPU_ES, codecOf(entry));
    else return false;
    fmt.i_original_fourcc = fourccOf(dc.track.sample_entry);
    fmt.i_group = hints.group;
    fmt.i_id = hints.es_id;
    fmt.b_packetized = true;
    const std::string lang = !hints.language.empty() ? hints.language : (dc.track.language != "und" ? dc.track.language : "");
    fmt.psz_language = dupOrNull(lang);
    std::string desc = hints.description;
    if (dc.track.encrypted) desc += desc.empty() ? "encrypted" : " (encrypted)";
    fmt.psz_description = dupOrNull(desc);
    if (kind == "video") {
        fmt.video.i_width = fmt.video.i_visible_width = dc.track.width.value_or(0);
        fmt.video.i_height = fmt.video.i_visible_height = dc.track.height.value_or(0);
    } else if (kind == "audio") {
        fmt.audio.i_rate = dc.track.sample_rate.value_or(0);
        fmt.audio.i_channels = static_cast<uint8_t>(dc.track.channels.value_or(0));
    }
    if (!dc.extradata.empty()) {
        fmt.p_extra = malloc(dc.extradata.size());
        if (fmt.p_extra) {
            memcpy(fmt.p_extra, dc.extradata.data(), dc.extradata.size());
            fmt.i_extra = static_cast<int>(dc.extradata.size());
        }
    }
    return true;
}

}  // namespace atsc3::vlcplugin
