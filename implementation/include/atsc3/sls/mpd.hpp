#pragma once
// Light DASH MPD model (spec 5.9.2), template resolution and the offline (static) rewrite used by
// the extraction tool and the web harness.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace atsc3::sls {

struct TimelineEntry {
    std::optional<std::uint64_t> t;
    std::uint64_t d = 0;
    std::int64_t r = 0;  // -1 = until next S / period end
};
struct SegmentTemplate {
    bool present = false;
    std::optional<std::string> initialization, media;
    std::uint32_t timescale = 1;
    std::optional<std::uint64_t> duration;
    std::uint64_t start_number = 1;
    std::optional<std::uint64_t> presentation_time_offset;
    std::vector<TimelineEntry> timeline;
};
enum class Addressing { None, Number, Time };
Addressing addressingOf(const SegmentTemplate& t);

struct Descriptor {
    std::string scheme_id_uri, value;
};
struct Representation {
    std::string id;
    std::optional<std::string> codecs, mime_type, frame_rate, scan_type;
    std::optional<std::uint64_t> bandwidth;
    std::optional<std::uint32_t> width, height, audio_sampling_rate;
    SegmentTemplate segment_template;  // effective (Representation, else AdaptationSet, else Period)
    std::vector<Descriptor> content_protection;
    std::string raw_xml;
};
struct AdaptationSet {
    std::optional<std::string> id, content_type, lang, mime_type, codecs;
    std::vector<Descriptor> roles, accessibility, supplemental_properties, essential_properties, content_protection;
    std::vector<Representation> representations;
};
struct Period {
    std::optional<std::string> id;
    std::optional<double> start_s, duration_s;
    std::vector<std::string> base_urls;
    std::vector<AdaptationSet> adaptation_sets;
};
struct Mpd {
    std::string type = "static";
    std::string profiles;
    std::optional<std::int64_t> availability_start_ns, publish_time_ns;
    std::optional<double> minimum_update_period_s, time_shift_buffer_depth_s, media_presentation_duration_s;
    std::optional<double> min_buffer_time_s, max_segment_duration_s;
    std::vector<std::string> base_urls;
    std::vector<Period> periods;
    std::string raw_xml;
    const Representation* representation(const std::string& id) const;
};
bool parseMpd(const std::string& xml, Mpd& out, std::string* error = nullptr);

/// Expands $RepresentationID$, $Number[%0Nd]$, $Time[%0Nd]$, $Bandwidth[%0Nd]$ and $$.
std::string resolveTemplate(const std::string& tmpl, const std::string& rep_id, std::uint64_t number, std::uint64_t time,
                            std::uint64_t bandwidth = 0);
/// Inverse of resolveTemplate for one $Number$/$Time$ token: returns the key encoded in `name`
/// when it matches the template around the token (the other identifiers are substituted first).
std::optional<std::uint64_t> matchTemplate(const std::string& tmpl, const std::string& rep_id, std::uint64_t bandwidth,
                                           const std::string& name);

/// Rewrites a dynamic broadcast MPD as a static one that references only the captured segments.
/// `segments` maps Representation@id to the sorted $Number$ or $Time$ keys that are available on disk.
/// Representations without captured segments are removed; absolute BaseURLs are dropped.
bool makeStaticMpd(const std::string& mpd_xml, const std::map<std::string, std::vector<std::uint64_t>>& segments,
                   std::string& out_xml, std::string* error = nullptr);

/// A delivered object classified against the MPD: its Representation, its $Number$/$Time$ key, init or media.
struct SegmentMatch {
    std::string rep_id;
    std::uint64_t key = 0;
    bool is_init = false;
};
/// Finds the Representation a ROUTE object belongs to: by name against the channel's MediaInfo@repId templates,
/// then against every Representation's templates, then (repId known, name unmatched) TOI as the key.
std::optional<SegmentMatch> matchSegment(const Mpd& mpd, const std::string& name, const std::optional<std::string>& channel_rep_id,
                                         std::uint64_t toi);

/// Rewrites a broadcast MPD for a local DASH origin (spec 5.17.3): every BaseURL replaced by `base_url`, a UTCTiming
/// `urn:mpeg:dash:utc:http-xsdate:2014` element pointing at `time_url` (replacing any present), a Location element set
/// to `location`, minimumUpdatePeriod clamped to 1 s when a SegmentTimeline is used; everything else (type, times,
/// ContentProtection, templates) kept. `start_numbers` maps Representation@id to a calibrated $Number$ startNumber
/// (spec 5.17.3 "$Number$ calibration"): it is written on that Representation's own SegmentTemplate, which is created
/// as a copy of the inherited AdaptationSet/Period template when the Representation has none.
bool makeOriginMpd(const std::string& mpd_xml, const std::string& base_url, const std::string& time_url, const std::string& location,
                   std::string& out_xml, std::string* error = nullptr,
                   const std::map<std::string, std::uint64_t>& start_numbers = {});

}  // namespace atsc3::sls
