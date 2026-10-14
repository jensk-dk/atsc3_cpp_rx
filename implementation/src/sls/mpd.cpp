#include "atsc3/sls/mpd.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "xml/xml.hpp"

namespace atsc3::sls {

using namespace atsc3::xml;

namespace {
ByteView bytesOf(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

std::vector<Descriptor> descriptors(const pugi::xml_node& n, const char* local) {
    std::vector<Descriptor> v;
    for (const pugi::xml_node& d : children(n, local)) v.push_back(Descriptor{attrOr(d, "schemeIdUri", ""), attrOr(d, "value", "")});
    return v;
}

void mergeTemplate(SegmentTemplate& into, const pugi::xml_node& st) {
    if (!st) return;
    into.present = true;
    if (const auto v = attr(st, "initialization")) into.initialization = v;
    if (const auto v = attr(st, "media")) into.media = v;
    if (const auto v = attrUInt(st, "timescale")) into.timescale = *v;
    if (const auto v = attrInt(st, "duration"); v && *v >= 0) into.duration = static_cast<std::uint64_t>(*v);
    if (const auto v = attrInt(st, "startNumber"); v && *v >= 0) into.start_number = static_cast<std::uint64_t>(*v);
    if (const auto v = attrInt(st, "presentationTimeOffset"); v && *v >= 0) into.presentation_time_offset = static_cast<std::uint64_t>(*v);
    if (const pugi::xml_node tl = child(st, "SegmentTimeline")) {
        into.timeline.clear();
        for (const pugi::xml_node& s : children(tl, "S")) {
            TimelineEntry e;
            if (const auto t = attrInt(s, "t"); t && *t >= 0) e.t = static_cast<std::uint64_t>(*t);
            if (const auto d = attrInt(s, "d"); d && *d >= 0) e.d = static_cast<std::uint64_t>(*d);
            e.r = attrInt(s, "r").value_or(0);
            into.timeline.push_back(e);
        }
    }
}

std::string durationIso(double seconds) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "PT%.3fS", seconds);
    return buf;
}
}  // namespace

Addressing addressingOf(const SegmentTemplate& t) {
    if (!t.media) return Addressing::None;
    if (t.media->find("$Number") != std::string::npos) return Addressing::Number;
    if (t.media->find("$Time") != std::string::npos) return Addressing::Time;
    return Addressing::None;
}

const Representation* Mpd::representation(const std::string& id) const {
    for (const Period& p : periods)
        for (const AdaptationSet& a : p.adaptation_sets)
            for (const Representation& r : a.representations)
                if (r.id == id) return &r;
    return nullptr;
}

bool parseMpd(const std::string& text, Mpd& out, std::string* error) {
    out = Mpd{};
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    const pugi::xml_node mpd = root(doc, "MPD");
    if (!mpd) {
        if (error) *error = "root is not MPD";
        return false;
    }
    out.type = attrOr(mpd, "type", "static");
    out.profiles = attrOr(mpd, "profiles", "");
    if (const auto v = attr(mpd, "availabilityStartTime")) out.availability_start_ns = parseDateTimeNs(*v);
    if (const auto v = attr(mpd, "publishTime")) out.publish_time_ns = parseDateTimeNs(*v);
    if (const auto v = attr(mpd, "minimumUpdatePeriod")) out.minimum_update_period_s = parseDurationSeconds(*v);
    if (const auto v = attr(mpd, "timeShiftBufferDepth")) out.time_shift_buffer_depth_s = parseDurationSeconds(*v);
    if (const auto v = attr(mpd, "mediaPresentationDuration")) out.media_presentation_duration_s = parseDurationSeconds(*v);
    if (const auto v = attr(mpd, "minBufferTime")) out.min_buffer_time_s = parseDurationSeconds(*v);
    if (const auto v = attr(mpd, "maxSegmentDuration")) out.max_segment_duration_s = parseDurationSeconds(*v);
    for (const pugi::xml_node& b : children(mpd, "BaseURL")) out.base_urls.emplace_back(b.text().as_string());
    for (const pugi::xml_node& pn : children(mpd, "Period")) {
        Period p;
        p.id = attr(pn, "id");
        if (const auto v = attr(pn, "start")) p.start_s = parseDurationSeconds(*v);
        if (const auto v = attr(pn, "duration")) p.duration_s = parseDurationSeconds(*v);
        for (const pugi::xml_node& b : children(pn, "BaseURL")) p.base_urls.emplace_back(b.text().as_string());
        SegmentTemplate period_tmpl;
        mergeTemplate(period_tmpl, child(pn, "SegmentTemplate"));
        for (const pugi::xml_node& an : children(pn, "AdaptationSet")) {
            AdaptationSet a;
            a.id = attr(an, "id");
            a.content_type = attr(an, "contentType");
            a.lang = attr(an, "lang");
            a.mime_type = attr(an, "mimeType");
            a.codecs = attr(an, "codecs");
            a.roles = descriptors(an, "Role");
            a.accessibility = descriptors(an, "Accessibility");
            a.supplemental_properties = descriptors(an, "SupplementalProperty");
            a.essential_properties = descriptors(an, "EssentialProperty");
            a.content_protection = descriptors(an, "ContentProtection");
            SegmentTemplate as_tmpl = period_tmpl;
            mergeTemplate(as_tmpl, child(an, "SegmentTemplate"));
            for (const pugi::xml_node& rn : children(an, "Representation")) {
                Representation r;
                r.id = attrOr(rn, "id", "");
                r.codecs = attr(rn, "codecs");
                if (!r.codecs) r.codecs = a.codecs;
                r.mime_type = attr(rn, "mimeType");
                if (!r.mime_type) r.mime_type = a.mime_type;
                r.frame_rate = attr(rn, "frameRate");
                r.scan_type = attr(rn, "scanType");
                if (const auto v = attrInt(rn, "bandwidth"); v && *v >= 0) r.bandwidth = static_cast<std::uint64_t>(*v);
                r.width = attrUInt(rn, "width");
                r.height = attrUInt(rn, "height");
                r.audio_sampling_rate = attrUInt(rn, "audioSamplingRate");
                r.segment_template = as_tmpl;
                mergeTemplate(r.segment_template, child(rn, "SegmentTemplate"));
                r.content_protection = descriptors(rn, "ContentProtection");
                r.raw_xml = serialize(rn);
                a.representations.push_back(std::move(r));
            }
            p.adaptation_sets.push_back(std::move(a));
        }
        out.periods.push_back(std::move(p));
    }
    out.raw_xml = serialize(mpd);
    return true;
}

namespace {
// Splits "$Ident%05d$" into ident/width; returns false for malformed tokens.
bool parseToken(const std::string& t, std::size_t start, std::size_t& end, std::string& ident, int& width) {
    end = t.find('$', start + 1);
    if (end == std::string::npos) return false;
    std::string body = t.substr(start + 1, end - start - 1);
    width = 0;
    const auto pct = body.find('%');
    if (pct != std::string::npos) {
        const std::string fmt = body.substr(pct);
        body.erase(pct);
        if (fmt.size() < 3 || fmt[1] != '0' || fmt.back() != 'd') return false;
        for (std::size_t k = 2; k + 1 < fmt.size(); ++k) {
            if (fmt[k] < '0' || fmt[k] > '9') return false;
            width = width * 10 + (fmt[k] - '0');
        }
    }
    ident = body;
    return true;
}
std::string formatNumber(std::uint64_t v, int width) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%0*llu", width, static_cast<unsigned long long>(v));
    return buf;
}
}  // namespace

std::string resolveTemplate(const std::string& t, const std::string& rep_id, std::uint64_t number, std::uint64_t time,
                            std::uint64_t bandwidth) {
    std::string out;
    std::size_t i = 0;
    while (i < t.size()) {
        if (t[i] != '$') {
            out.push_back(t[i++]);
            continue;
        }
        if (i + 1 < t.size() && t[i + 1] == '$') {
            out.push_back('$');
            i += 2;
            continue;
        }
        std::size_t end;
        std::string ident;
        int width;
        if (!parseToken(t, i, end, ident, width)) {
            out.append(t, i, std::string::npos);
            break;
        }
        if (ident == "RepresentationID") out += rep_id;
        else if (ident == "Number") out += formatNumber(number, width);
        else if (ident == "Time") out += formatNumber(time, width);
        else if (ident == "Bandwidth") out += formatNumber(bandwidth, width);
        else out.append(t, i, end - i + 1);
        i = end + 1;
    }
    return out;
}

std::optional<std::uint64_t> matchTemplate(const std::string& t, const std::string& rep_id, std::uint64_t bandwidth,
                                           const std::string& name) {
    // Resolve everything except the first $Number$/$Time$ token, which is replaced by a marker.
    std::string prefix, suffix;
    bool seen = false;
    std::size_t i = 0;
    std::string* cur = &prefix;
    while (i < t.size()) {
        if (t[i] != '$') {
            cur->push_back(t[i++]);
            continue;
        }
        if (i + 1 < t.size() && t[i + 1] == '$') {
            cur->push_back('$');
            i += 2;
            continue;
        }
        std::size_t end;
        std::string ident;
        int width;
        if (!parseToken(t, i, end, ident, width)) return std::nullopt;
        if (ident == "RepresentationID") *cur += rep_id;
        else if (ident == "Bandwidth") *cur += formatNumber(bandwidth, width);
        else if ((ident == "Number" || ident == "Time") && !seen) {
            seen = true;
            cur = &suffix;
        } else {
            return std::nullopt;
        }
        i = end + 1;
    }
    if (!seen) return std::nullopt;
    if (name.size() < prefix.size() + suffix.size() + 1) return std::nullopt;
    if (name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return std::nullopt;
    const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (digits.empty() || digits.size() > 19) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

bool makeStaticMpd(const std::string& text, const std::map<std::string, std::vector<std::uint64_t>>& segments,
                   std::string& out_xml, std::string* error) {
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    pugi::xml_node mpd = root(doc, "MPD");
    if (!mpd) {
        if (error) *error = "root is not MPD";
        return false;
    }
    auto setAttr = [](pugi::xml_node n, const char* name, const std::string& value) {
        pugi::xml_attribute a = n.attribute(name);
        if (!a) a = n.append_attribute(name);
        a.set_value(value.c_str());
    };
    auto removeAttr = [](pugi::xml_node n, const char* local) {
        for (pugi::xml_attribute a = n.first_attribute(); a;) {
            pugi::xml_attribute next = a.next_attribute();
            if (localName(a.name()) == local) n.remove_attribute(a);
            a = next;
        }
    };
    auto dropAbsoluteBaseUrls = [](pugi::xml_node n) {
        for (const pugi::xml_node& b : children(n, "BaseURL")) {
            const std::string u = b.text().as_string();
            if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0 || (!u.empty() && u[0] == '/')) n.remove_child(b);
        }
    };
    setAttr(mpd, "type", "static");
    for (const char* a : {"availabilityStartTime", "minimumUpdatePeriod", "timeShiftBufferDepth", "publishTime",
                          "suggestedPresentationDelay", "availabilityEndTime"})
        removeAttr(mpd, a);
    dropAbsoluteBaseUrls(mpd);

    double presentation_s = 0.0;
    bool any = false;
    for (pugi::xml_node period : children(mpd, "Period")) {
        dropAbsoluteBaseUrls(period);
        removeAttr(period, "duration");
        const pugi::xml_node period_tmpl = child(period, "SegmentTemplate");
        for (pugi::xml_node as : children(period, "AdaptationSet")) {
            dropAbsoluteBaseUrls(as);
            const pugi::xml_node as_tmpl = child(as, "SegmentTemplate");
            std::optional<double> as_duration_s;
            for (pugi::xml_node rep : children(as, "Representation")) {
                const std::string id = attrOr(rep, "id", "");
                const auto segs = segments.find(id);
                if (segs == segments.end() || segs->second.empty()) {
                    as.remove_child(rep);
                    continue;
                }
                // Give the Representation its own effective template (inheritance flattened).
                pugi::xml_node tmpl = child(rep, "SegmentTemplate");
                if (!tmpl) {
                    if (as_tmpl) tmpl = rep.append_copy(as_tmpl);
                    else if (period_tmpl) tmpl = rep.append_copy(period_tmpl);
                }
                if (!tmpl) continue;  // SegmentList/SegmentBase: left untouched
                if (as_tmpl) {  // inherit attributes the inner template does not set
                    for (pugi::xml_attribute a = as_tmpl.first_attribute(); a; a = a.next_attribute())
                        if (!tmpl.attribute(a.name())) tmpl.append_copy(a);
                    if (!child(tmpl, "SegmentTimeline") && child(as_tmpl, "SegmentTimeline")) tmpl.append_copy(child(as_tmpl, "SegmentTimeline"));
                }
                SegmentTemplate st;
                mergeTemplate(st, tmpl);
                std::vector<std::uint64_t> keys = segs->second;
                std::sort(keys.begin(), keys.end());
                keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
                const double timescale = st.timescale ? static_cast<double>(st.timescale) : 1.0;
                double rep_duration_s = 0.0;
                if (addressingOf(st) == Addressing::Number) {
                    std::uint64_t d = st.duration.value_or(0);
                    if (d == 0 && !st.timeline.empty()) d = st.timeline.front().d;
                    if (d == 0) {
                        as.remove_child(rep);
                        continue;
                    }
                    // $Number$ cannot express gaps: keep the longest contiguous run.
                    std::size_t best_b = 0, best_n = 1, b = 0;
                    for (std::size_t k = 1; k <= keys.size(); ++k) {
                        if (k == keys.size() || keys[k] != keys[k - 1] + 1) {
                            if (k - b > best_n) {
                                best_n = k - b;
                                best_b = b;
                            }
                            b = k;
                        }
                    }
                    const std::uint64_t first = keys[best_b];
                    const std::uint64_t t_base = st.timeline.empty() ? 0 : st.timeline.front().t.value_or(0);
                    const std::uint64_t t0 = t_base + (first >= st.start_number ? (first - st.start_number) * d : 0);
                    removeAttr(tmpl, "duration");
                    if (pugi::xml_node tl = child(tmpl, "SegmentTimeline")) tmpl.remove_child(tl);
                    setAttr(tmpl, "duration", std::to_string(d));
                    setAttr(tmpl, "startNumber", std::to_string(first));
                    setAttr(tmpl, "presentationTimeOffset", std::to_string(t0));
                    rep_duration_s = static_cast<double>(best_n) * static_cast<double>(d) / timescale;
                } else if (addressingOf(st) == Addressing::Time) {
                    std::uint64_t d_last = st.duration.value_or(0);
                    if (!st.timeline.empty()) d_last = st.timeline.back().d;
                    if (d_last == 0 && keys.size() >= 2) d_last = keys.back() - keys[keys.size() - 2];
                    removeAttr(tmpl, "duration");
                    if (pugi::xml_node tl = child(tmpl, "SegmentTimeline")) tmpl.remove_child(tl);
                    pugi::xml_node tl = tmpl.append_child("SegmentTimeline");
                    std::uint64_t end = keys.front();
                    for (std::size_t k = 0; k < keys.size(); ++k) {
                        const std::uint64_t d = k + 1 < keys.size() ? keys[k + 1] - keys[k] : d_last;
                        pugi::xml_node last = tl.last_child();
                        if (last && std::string(last.attribute("d").value()) == std::to_string(d) && end == keys[k]) {
                            last.attribute("r").set_value(last.attribute("r").as_llong() + 1);
                        } else {
                            pugi::xml_node s = tl.append_child("S");
                            s.append_attribute("t").set_value(std::to_string(keys[k]).c_str());
                            s.append_attribute("d").set_value(std::to_string(d).c_str());
                            s.append_attribute("r").set_value("0");
                        }
                        end = keys[k] + d;
                    }
                    setAttr(tmpl, "presentationTimeOffset", std::to_string(keys.front()));
                    rep_duration_s = static_cast<double>(end - keys.front()) / timescale;
                } else {
                    as.remove_child(rep);
                    continue;
                }
                any = true;
                as_duration_s = as_duration_s ? std::min(*as_duration_s, rep_duration_s) : rep_duration_s;
            }
            if (as_tmpl) as.remove_child(as_tmpl);
            if (children(as, "Representation").empty()) {
                period.remove_child(as);
                continue;
            }
            if (as_duration_s) presentation_s = presentation_s == 0.0 ? *as_duration_s : std::min(presentation_s, *as_duration_s);
        }
        if (period_tmpl) period.remove_child(period_tmpl);
        if (children(period, "AdaptationSet").empty()) mpd.remove_child(period);
    }
    if (!any) {
        if (error) *error = "no captured segments match the MPD";
        return false;
    }
    setAttr(mpd, "mediaPresentationDuration", durationIso(presentation_s));
    std::ostringstream os;
    doc.save(os, "  ", pugi::format_default | pugi::format_write_bom * 0);
    out_xml = os.str();
    return true;
}

std::optional<SegmentMatch> matchSegment(const Mpd& mpd, const std::string& name, const std::optional<std::string>& channel_rep_id,
                                         std::uint64_t toi) {
    auto tryRep = [&](const Representation& r) -> std::optional<SegmentMatch> {
        const SegmentTemplate& t = r.segment_template;
        if (!t.present) return std::nullopt;
        const std::uint64_t bw = r.bandwidth.value_or(0);
        if (t.initialization && resolveTemplate(*t.initialization, r.id, 0, 0, bw) == name) return SegmentMatch{r.id, 0, true};
        if (t.media) {
            if (const auto k = matchTemplate(*t.media, r.id, bw, name)) return SegmentMatch{r.id, *k, false};
        }
        return std::nullopt;
    };
    if (channel_rep_id) {
        if (const Representation* r = mpd.representation(*channel_rep_id)) {
            if (auto m = tryRep(*r)) return m;
        }
    }
    for (const Period& p : mpd.periods)
        for (const AdaptationSet& a : p.adaptation_sets)
            for (const Representation& r : a.representations)
                if (auto m = tryRep(r)) return m;
    if (channel_rep_id && mpd.representation(*channel_rep_id)) return SegmentMatch{*channel_rep_id, toi, false};
    return std::nullopt;
}

bool makeOriginMpd(const std::string& text, const std::string& base_url, const std::string& time_url, const std::string& location,
                   std::string& out_xml, std::string* error, const std::map<std::string, std::uint64_t>& start_numbers) {
    pugi::xml_document doc;
    if (!load(doc, bytesOf(text), error)) return false;
    pugi::xml_node mpd = root(doc, "MPD");
    if (!mpd) {
        if (error) *error = "root is not MPD";
        return false;
    }
    auto setAttr = [](pugi::xml_node n, const char* name, const std::string& value) {
        pugi::xml_attribute a = n.attribute(name);
        if (!a) a = n.append_attribute(name);
        a.set_value(value.c_str());
    };
    // BaseURLs: one at MPD level pointing at the origin, none below (segment names are relative to it).
    for (pugi::xml_node n : {mpd}) {
        for (const pugi::xml_node& b : children(n, "BaseURL")) n.remove_child(b);
    }
    for (pugi::xml_node period : children(mpd, "Period")) {
        for (const pugi::xml_node& b : children(period, "BaseURL")) period.remove_child(b);
        for (pugi::xml_node as : children(period, "AdaptationSet")) {
            for (const pugi::xml_node& b : children(as, "BaseURL")) as.remove_child(b);
            for (pugi::xml_node rep : children(as, "Representation"))
                for (const pugi::xml_node& b : children(rep, "BaseURL")) rep.remove_child(b);
        }
    }
    for (const pugi::xml_node& u : children(mpd, "UTCTiming")) mpd.remove_child(u);
    for (const pugi::xml_node& l : children(mpd, "Location")) mpd.remove_child(l);
    // Schema order: ... BaseURL*, Location*, ..., Period+, ..., UTCTiming* ; put BaseURL/Location first, UTCTiming last.
    pugi::xml_node first = mpd.first_child();
    pugi::xml_node base = first ? mpd.insert_child_before("BaseURL", first) : mpd.append_child("BaseURL");
    base.text().set(base_url.c_str());
    pugi::xml_node loc = mpd.insert_child_after("Location", base);
    loc.text().set(location.c_str());
    pugi::xml_node utc = mpd.append_child("UTCTiming");
    setAttr(utc, "schemeIdUri", "urn:mpeg:dash:utc:http-xsdate:2014");
    setAttr(utc, "value", time_url);
    // $Number$ calibration: the origin found that the numbering of these representations disagrees with the media
    // timeline (tfdt); the effective startNumber goes onto the Representation's own SegmentTemplate so that the other
    // representations sharing an inherited template are untouched.
    for (pugi::xml_node period : children(mpd, "Period")) {
        for (pugi::xml_node as : children(period, "AdaptationSet")) {
            for (pugi::xml_node rep : children(as, "Representation")) {
                const auto it = start_numbers.find(attrOr(rep, "id", ""));
                if (it == start_numbers.end()) continue;
                pugi::xml_node tmpl = child(rep, "SegmentTemplate");
                if (!tmpl) {
                    pugi::xml_node inherited = child(as, "SegmentTemplate");
                    if (!inherited) inherited = child(period, "SegmentTemplate");
                    tmpl = inherited ? rep.append_copy(inherited) : rep.append_child("SegmentTemplate");
                }
                setAttr(tmpl, "startNumber", std::to_string(it->second));
            }
        }
    }
    // Broadcast MPDs with a SegmentTimeline change every segment while advertising long update periods.
    bool timeline = false;
    for (const pugi::xml_node& period : children(mpd, "Period")) {
        if (child(child(period, "SegmentTemplate"), "SegmentTimeline")) timeline = true;
        for (const pugi::xml_node& as : children(period, "AdaptationSet")) {
            if (child(child(as, "SegmentTemplate"), "SegmentTimeline")) timeline = true;
            for (const pugi::xml_node& rep : children(as, "Representation"))
                if (child(child(rep, "SegmentTemplate"), "SegmentTimeline")) timeline = true;
        }
    }
    if (timeline && attrOr(mpd, "type", "static") == "dynamic") {
        const auto mup = parseDurationSeconds(attrOr(mpd, "minimumUpdatePeriod", "PT0S")).value_or(0.0);
        if (mup > 1.0) setAttr(mpd, "minimumUpdatePeriod", "PT1S");
    }
    std::ostringstream os;
    doc.save(os, "  ", pugi::format_default | pugi::format_write_bom * 0);
    out_xml = os.str();
    return true;
}

}  // namespace atsc3::sls
