#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "atsc3/sls/mpd.hpp"
#include "corpus.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
const char* kUniMpd =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><MPD profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" type=\"dynamic\" "
    "availabilityStartTime=\"2019-10-24T21:43:44Z\" minimumUpdatePeriod=\"PT10S\" minBufferTime=\"PT2S\" timeShiftBufferDepth=\"PT20S\" "
    "maxSegmentDuration=\"PT2S\" xmlns=\"urn:mpeg:dash:schema:mpd:2011\"><BaseURL>http://example.com/live/</BaseURL><Period id=\"P0\" start=\"PT0S\">"
    "<AdaptationSet id=\"0\" contentType=\"video\" mimeType=\"video/mp4\" segmentAlignment=\"true\" startWithSAP=\"1\">"
    "<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"main\"/>"
    "<Representation id=\"0\" bandwidth=\"7000000\" width=\"3840\" height=\"2160\" frameRate=\"30000/1001\" codecs=\"hev1.2.4.L150.90\">"
    "<SegmentTemplate media=\"test-$RepresentationID$-$Number$.m4pv\" initialization=\"test-$RepresentationID$-init.mp4v\" duration=\"2002000\" "
    "startNumber=\"3\" timescale=\"1000000\"/></Representation></AdaptationSet>"
    "<AdaptationSet id=\"1\" contentType=\"audio\" mimeType=\"audio/mp4\"><SegmentTemplate media=\"test-$RepresentationID$-$Number$.m4pa\" "
    "initialization=\"test-$RepresentationID$-init.mp4a\" duration=\"2002000\" startNumber=\"3\" timescale=\"1000000\"/>"
    "<Representation id=\"1\" bandwidth=\"384000\" audioSamplingRate=\"48000\" codecs=\"mp4a.40.2\"/>"
    "<Representation id=\"9\" bandwidth=\"96000\" audioSamplingRate=\"48000\" codecs=\"mp4a.40.2\"/></AdaptationSet></Period></MPD>";
}  // namespace

TEST_CASE("MPD: light model of an on-air isoff-live MPD with template inheritance", "[sls][mpd]") {
    sls::Mpd m;
    std::string err;
    REQUIRE(sls::parseMpd(kUniMpd, m, &err));
    CHECK(m.type == "dynamic");
    CHECK(m.profiles == "urn:mpeg:dash:profile:isoff-live:2011");
    CHECK(m.availability_start_ns);
    CHECK(m.minimum_update_period_s == 10.0);
    CHECK(m.base_urls == std::vector<std::string>{"http://example.com/live/"});
    REQUIRE(m.periods.size() == 1);
    REQUIRE(m.periods[0].adaptation_sets.size() == 2);
    const sls::Representation* v = m.representation("0");
    REQUIRE(v);
    CHECK(v->codecs == "hev1.2.4.L150.90");
    CHECK(v->mime_type == "video/mp4");
    CHECK(v->width == 3840u);
    CHECK(v->segment_template.present);
    CHECK(v->segment_template.timescale == 1000000);
    CHECK(v->segment_template.duration == 2002000u);
    CHECK(v->segment_template.start_number == 3);
    CHECK(sls::addressingOf(v->segment_template) == sls::Addressing::Number);
    const sls::Representation* a = m.representation("9");
    REQUIRE(a);
    CHECK(a->segment_template.present);  // inherited from the AdaptationSet
    CHECK(a->segment_template.media == "test-$RepresentationID$-$Number$.m4pa");
    CHECK(m.periods[0].adaptation_sets[0].roles[0].value == "main");
    CHECK(m.representation("nope") == nullptr);
}

TEST_CASE("MPD: template resolution and inverse matching", "[sls][mpd]") {
    CHECK(sls::resolveTemplate("test-$RepresentationID$-$Number$.m4pv", "0", 3821, 0) == "test-0-3821.m4pv");
    CHECK(sls::resolveTemplate("video-$Number%05d$.mp4v", "x", 7, 0) == "video-00007.mp4v");
    CHECK(sls::resolveTemplate("v-$Time$-$Bandwidth$.m4s$$", "x", 0, 7207200, 4000000) == "v-7207200-4000000.m4s$");
    CHECK(sls::resolveTemplate("$RepresentationID$-init.mp4", "d4_4", 0, 0) == "d4_4-init.mp4");
    CHECK(sls::matchTemplate("test-$RepresentationID$-$Number$.m4pv", "0", 0, "test-0-3821.m4pv") == 3821u);
    CHECK(sls::matchTemplate("a0-$RepresentationID$-$Number$.m4s", "a02_2", 0, "a0-a02_2-1676551953.m4s") == 1676551953u);
    CHECK(sls::matchTemplate("video-$Time$.mp4v", "x", 0, "video-7207200.mp4v") == 7207200u);
    CHECK_FALSE(sls::matchTemplate("video-$Time$.mp4v", "x", 0, "audio-7207200.mp4v"));
    CHECK_FALSE(sls::matchTemplate("video-$Time$.mp4v", "x", 0, "video-init.mp4v"));
    CHECK_FALSE(sls::matchTemplate("video-init.mp4v", "x", 0, "video-init.mp4v"));  // no key token
}

TEST_CASE("MPD: static rewrite for $Number$ keeps the longest contiguous run and drops absolute BaseURLs", "[sls][mpd]") {
    std::map<std::string, std::vector<std::uint64_t>> segs;
    segs["0"] = {3821, 3822, 3823, 3824, 3825};
    segs["1"] = {3819, 3821, 3822, 3823};  // gap: 3819 is dropped
    std::string out, err;
    REQUIRE(sls::makeStaticMpd(kUniMpd, segs, out, &err));
    sls::Mpd m;
    REQUIRE(sls::parseMpd(out, m, &err));
    CHECK(m.type == "static");
    CHECK_FALSE(m.availability_start_ns);
    CHECK_FALSE(m.minimum_update_period_s);
    CHECK(m.base_urls.empty());
    REQUIRE(m.media_presentation_duration_s);
    CHECK(*m.media_presentation_duration_s > 6.0);  // min over reps: 3 audio segments of 2.002 s
    CHECK(*m.media_presentation_duration_s < 6.1);
    const sls::Representation* v = m.representation("0");
    REQUIRE(v);
    CHECK(v->segment_template.start_number == 3821);
    CHECK(v->segment_template.duration == 2002000u);
    CHECK(v->segment_template.presentation_time_offset == (3821ull - 3ull) * 2002000ull);
    const sls::Representation* a = m.representation("1");
    REQUIRE(a);
    CHECK(a->segment_template.start_number == 3821);
    CHECK(a->segment_template.media == "test-$RepresentationID$-$Number$.m4pa");
    CHECK(m.representation("9") == nullptr);  // no captured segments -> removed
    CHECK(out.find("<SegmentTemplate") != std::string::npos);
    // nothing captured -> error
    CHECK_FALSE(sls::makeStaticMpd(kUniMpd, {}, out, &err));
}

TEST_CASE("MPD: static rewrite for $Time$ (Triveni SegmentTimeline sample) builds an exact timeline", "[sls][mpd][schema]") {
    const std::string p = schema("sls/route/ROUTE_SLS-OTT-delivery-Triveni-SegmentTimeline-fixed-MPD.xml");
    if (!exists(p)) SKIP("schemas not available");
    const std::string xml = slurp(p);
    sls::Mpd orig;
    REQUIRE(sls::parseMpd(xml, orig));
    const sls::Representation* v = orig.representation("Video1_1");
    REQUIRE(v);
    CHECK(sls::addressingOf(v->segment_template) == sls::Addressing::Time);
    REQUIRE(v->segment_template.timeline.size() == 1);
    CHECK(v->segment_template.timeline[0].t == 7207200u);
    CHECK(v->segment_template.timeline[0].r == 19);
    std::map<std::string, std::vector<std::uint64_t>> segs;
    segs["Video1_1"] = {7207200, 7447440, 7687680, 7927920};
    segs["22"] = {7207200, 7447440, 7687680, 8168160};  // one segment missing in the middle
    std::string out, err;
    REQUIRE(sls::makeStaticMpd(xml, segs, out, &err));
    sls::Mpd m;
    REQUIRE(sls::parseMpd(out, m, &err));
    CHECK(m.type == "static");
    CHECK(m.base_urls.empty());
    const sls::Representation* nv = m.representation("Video1_1");
    REQUIRE(nv);
    CHECK_FALSE(nv->segment_template.duration);
    REQUIRE(nv->segment_template.timeline.size() == 1);
    CHECK(nv->segment_template.timeline[0].t == 7207200u);
    CHECK(nv->segment_template.timeline[0].d == 240240);
    CHECK(nv->segment_template.timeline[0].r == 3);
    CHECK(nv->segment_template.presentation_time_offset == 7207200u);
    const sls::Representation* na = m.representation("22");
    REQUIRE(na);
    REQUIRE(na->segment_template.timeline.size() == 3);  // 2x240240 run, 480480 gap entry, final 240240
    CHECK(na->segment_template.timeline[0].r == 1);
    CHECK(na->segment_template.timeline[1].d == 480480);
    CHECK(na->segment_template.timeline[2].t == 8168160u);
    CHECK(m.representation("a3_3") == nullptr);
    REQUIRE(m.media_presentation_duration_s);
    CHECK(*m.media_presentation_duration_s > 4.0);
    CHECK(*m.media_presentation_duration_s < 4.1);
}

namespace {
const char* kOriginMpd =
    "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" availabilityStartTime=\"1970-01-01T00:00:00Z\" "
    "minimumUpdatePeriod=\"PT24H\" timeShiftBufferDepth=\"PT6S\" profiles=\"urn:mpeg:dash:profile:isoff-broadcast:2015\">"
    "<BaseURL>http://broadcaster.example/</BaseURL><UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:ntp:2014\" value=\"pool.ntp.org\"/>"
    "<Period id=\"P0\" start=\"PT0S\"><BaseURL>p0/</BaseURL>"
    "<AdaptationSet contentType=\"video\" mimeType=\"video/mp4\"><Representation id=\"Video1_1\" bandwidth=\"4500000\" codecs=\"hvc1.2.4.L120.B0\" width=\"1280\" height=\"720\">"
    "<SegmentTemplate duration=\"60060\" initialization=\"video-init.mp4\" media=\"video-$Number$.mp4v\" startNumber=\"1\" timescale=\"60000\"/></Representation></AdaptationSet>"
    "<AdaptationSet contentType=\"audio\" lang=\"eng\"><Representation id=\"a02_2\" bandwidth=\"96000\" codecs=\"ac-4.02.01.01\">"
    "<SegmentTemplate duration=\"240240\" initialization=\"a0-$RepresentationID$-init.mp4\" media=\"a0-$RepresentationID$-$Number$.m4s\" startNumber=\"1\" timescale=\"240000\"/></Representation></AdaptationSet>"
    "<AdaptationSet contentType=\"video\"><Representation id=\"tl\" bandwidth=\"1\"><SegmentTemplate media=\"t-$Time$.m4s\" initialization=\"t-init.mp4\" timescale=\"1000\">"
    "<SegmentTimeline><S t=\"0\" d=\"2000\" r=\"2\"/></SegmentTimeline></SegmentTemplate></Representation></AdaptationSet></Period></MPD>";
}  // namespace

TEST_CASE("MPD: matchSegment classifies init and media objects by channel repId, by any template, then by TOI", "[sls][mpd]") {
    sls::Mpd m;
    REQUIRE(sls::parseMpd(kOriginMpd, m));
    auto r = sls::matchSegment(m, "video-init.mp4", std::string("Video1_1"), 7);
    REQUIRE(r);
    CHECK(r->rep_id == "Video1_1");
    CHECK(r->is_init);
    r = sls::matchSegment(m, "video-1676551955.mp4v", std::nullopt, 7);   // no repId on the channel: every template is tried
    REQUIRE(r);
    CHECK(r->rep_id == "Video1_1");
    CHECK_FALSE(r->is_init);
    CHECK(r->key == 1676551955u);
    r = sls::matchSegment(m, "a0-a02_2-1676551956.m4s", std::string("a02_2"), 9);
    REQUIRE(r);
    CHECK(r->rep_id == "a02_2");
    CHECK(r->key == 1676551956u);
    r = sls::matchSegment(m, "t-4000.m4s", std::nullopt, 0);
    REQUIRE(r);
    CHECK(r->rep_id == "tl");
    CHECK(r->key == 4000u);
    r = sls::matchSegment(m, "opaque-name.bin", std::string("a02_2"), 4242);   // repId known, name unmatched: TOI is the number
    REQUIRE(r);
    CHECK(r->rep_id == "a02_2");
    CHECK(r->key == 4242u);
    CHECK_FALSE(sls::matchSegment(m, "opaque-name.bin", std::nullopt, 1));
    CHECK_FALSE(sls::matchSegment(m, "opaque-name.bin", std::string("nope"), 1));
}

TEST_CASE("MPD: origin rewrite replaces BaseURLs, adds Location and UTCTiming, clamps minimumUpdatePeriod for timelines", "[sls][mpd]") {
    std::string out, err;
    REQUIRE(sls::makeOriginMpd(kOriginMpd, "/media/1409/5004/", "/api/v1/time/xsdate", "/media/1409/5004/mpd.mpd", out, &err));
    INFO(out);
    CHECK(out.find("<BaseURL>/media/1409/5004/</BaseURL>") != std::string::npos);
    CHECK(out.find("broadcaster.example") == std::string::npos);
    CHECK(out.find("<BaseURL>p0/</BaseURL>") == std::string::npos);
    CHECK(out.find("<Location>/media/1409/5004/mpd.mpd</Location>") != std::string::npos);
    CHECK(out.find("urn:mpeg:dash:utc:http-xsdate:2014") != std::string::npos);
    CHECK(out.find("value=\"/api/v1/time/xsdate\"") != std::string::npos);
    CHECK(out.find("pool.ntp.org") == std::string::npos);
    CHECK(out.find("type=\"dynamic\"") != std::string::npos);
    CHECK(out.find("availabilityStartTime=\"1970-01-01T00:00:00Z\"") != std::string::npos);
    CHECK(out.find("minimumUpdatePeriod=\"PT1S\"") != std::string::npos);   // a SegmentTimeline is present
    CHECK(out.find("ac-4.02.01.01") != std::string::npos);                  // representations untouched
    // schema order: BaseURL and Location before the Period, UTCTiming after it
    CHECK(out.find("<BaseURL>") < out.find("<Period"));
    CHECK(out.find("<Location>") < out.find("<Period"));
    CHECK(out.find("<UTCTiming") > out.find("</Period>"));
    sls::Mpd m;
    REQUIRE(sls::parseMpd(out, m, &err));
    CHECK(m.base_urls == std::vector<std::string>{"/media/1409/5004/"});
    CHECK_FALSE(sls::makeOriginMpd("<Other/>", "/m/", "/t", "/m/mpd.mpd", out, &err));
}

TEST_CASE("MPD: origin rewrite writes calibrated $Number$ startNumbers on the Representation's own template", "[sls][mpd]") {
    // UniSoft usc01 shape: the video template is inherited from the AdaptationSet, the audio ones are per Representation
    const std::string mpd =
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" availabilityStartTime=\"2019-11-05T16:04:46Z\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" minimumUpdatePeriod=\"PT10S\">"
        "<Period id=\"P0\" start=\"PT0S\">"
        "<AdaptationSet id=\"0\" mimeType=\"video/mp4\">"
        "<SegmentTemplate media=\"v-$Number$.m4s\" initialization=\"v-init.mp4\" duration=\"2002000\" startNumber=\"67\" timescale=\"1000000\"/>"
        "<Representation id=\"v\" bandwidth=\"7000000\" codecs=\"hev1.2.4.L150.90\"/>"
        "</AdaptationSet>"
        "<AdaptationSet id=\"1\" mimeType=\"audio/mp4\">"
        "<Representation id=\"a\" bandwidth=\"384000\" codecs=\"mp4a.40.2\">"
        "<SegmentTemplate media=\"a-$Number$.m4s\" initialization=\"a-init.mp4\" duration=\"2002000\" startNumber=\"65\" timescale=\"1000000\"/>"
        "</Representation>"
        "<Representation id=\"a2\" bandwidth=\"96000\" codecs=\"mp4a.40.2\">"
        "<SegmentTemplate media=\"a2-$Number$.m4s\" initialization=\"a2-init.mp4\" duration=\"2002000\" startNumber=\"65\" timescale=\"1000000\"/>"
        "</Representation>"
        "</AdaptationSet>"
        "</Period></MPD>";
    std::string out, err;
    REQUIRE(sls::makeOriginMpd(mpd, "/media/24/1/", "/media/24/1/time.xsdate", "/media/24/1/mpd.mpd", out, &err, {{"v", 0}, {"a", 0}}));
    INFO(out);
    auto count = [&](const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t p = out.find(needle); p != std::string::npos; p = out.find(needle, p + needle.size())) ++n;
        return n;
    };
    CHECK(count("startNumber=\"0\"") == 2);
    CHECK(count("startNumber=\"67\"") == 1);   // the inherited AdaptationSet template is left alone
    CHECK(count("startNumber=\"65\"") == 1);   // a2 was not calibrated
    CHECK(count("v-$Number$.m4s") == 2);         // the copy carries the inherited attributes
    sls::Mpd m;
    REQUIRE(sls::parseMpd(out, m, &err));
    REQUIRE(m.representation("v"));
    CHECK(m.representation("v")->segment_template.start_number == 0);
    CHECK(m.representation("v")->segment_template.media == std::optional<std::string>("v-$Number$.m4s"));
    REQUIRE(m.representation("a"));
    CHECK(m.representation("a")->segment_template.start_number == 0);
    REQUIRE(m.representation("a2"));
    CHECK(m.representation("a2")->segment_template.start_number == 65);
    // no map: byte-for-byte the plain rewrite
    std::string plain;
    REQUIRE(sls::makeOriginMpd(mpd, "/media/24/1/", "/media/24/1/time.xsdate", "/media/24/1/mpd.mpd", plain, &err));
    CHECK(plain.find("startNumber=\"0\"") == std::string::npos);
}
