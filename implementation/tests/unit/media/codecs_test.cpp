// RFC 6381 codecs strings (spec 5.17.3): on-air ROUTE and MMT init segments (spec 5.17.3) plus
// synthetic sample entries for the paths the corpus does not exercise.
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "atsc3/media/isobmff.hpp"
#include "mp4_builders.hpp"

using namespace atsc3;
using namespace atsc3::test::mp4;

namespace {

/// The first stsd entry of the first trak of an init segment.
media::Box firstEntry(const Bytes& init) {
    const std::optional<media::Box> stsd = media::findPath(view(init), {"moov", "trak", "mdia", "minf", "stbl", "stsd"});
    REQUIRE(stsd);
    const std::vector<media::Box> entries = media::boxes(stsd->payload.subview(8));
    REQUIRE_FALSE(entries.empty());
    return entries.front();
}
media::Box entryOf(const Bytes& entry_bytes) {
    const std::vector<media::Box> b = media::boxes(view(entry_bytes));
    REQUIRE(b.size() == 1);
    return b.front();
}

}  // namespace

TEST_CASE("media codecs: on-air ROUTE init segments reproduce the broadcasters' MPD strings", "[media][codecs]") {
    struct Case {
        const char* file;
        const char* entry;
        const char* codecs;
    };
    const Case cases[] = {
        {"route-bwi-wmar-video-init.mp4", "hvc1", "hvc1.2.4.L120.B0"},  {"route-bwi-wmar-a0-init.mp4", "ac-4", "ac-4.02.01.01"},
        {"route-bwi-wmar-a1-init.mp4", "ac-4", "ac-4.02.01.00"},         {"route-bwi-wmar-d4-init.mp4", "stpp", "stpp.ttml.im1t"},
        {"route-bwi-wbal-video-init.mp4", "encv", "hvc1.2.4.L123.B0"},  {"route-bwi-wbal-a0-init.mp4", "enca", "ac-4.02.01.01"},
        {"route-uni-usc02-video-init.mp4v", "hev1", "hev1.2.4.L120.B0"}, {"route-uni-usc02-audio-init.mp4a", "mp4a", "mp4a.40.2"},
    };
    for (const Case& c : cases) {
        INFO(c.file);
        const Bytes init = loadVector(c.file);
        REQUIRE_FALSE(init.empty());
        const media::Box entry = firstEntry(init);
        CHECK(entry.type == c.entry);
        std::string warning;
        CHECK(media::codecsFromSampleEntry(entry, &warning) == c.codecs);
        CHECK(warning.empty());
        media::InitSegmentInfo info;
        REQUIRE(media::parseInitSegment(view(init), info));
        REQUIRE(info.tracks.size() == 1);
        CHECK(info.tracks[0].codecs == c.codecs);
        CHECK(info.tracks[0].sample_entry == c.entry);
    }
}

TEST_CASE("media codecs: HEVC string per 14496-15 Annex E.3", "[media][codecs]") {
    // On-air hvcC PTL: space 0, tier L, profile 2, compat 0x20000000 (bit-reversed 0x4), constraints b0 00.., level 120.
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hev1", 1920, 1080, hvcC(0, false, 2, 0x20000000, {0xb0, 0, 0, 0, 0, 0}, 120)))) ==
          "hev1.2.4.L120.B0");
    // All-zero constraint bytes end the string at the level; compat 0x60000000 -> "6".
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hev1", 0, 0, hvcC(0, false, 1, 0x60000000, zeros(6), 93)))) == "hev1.1.6.L93");
    // Profile space A, high tier, a zero byte before the last non-zero one prints as "00".
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hvc1", 0, 0, hvcC(1, true, 1, 0x60000000, {0xb0, 0, 0, 0, 0x23, 0}, 93)))) ==
          "hvc1.A1.6.H93.B0.00.00.00.23");
    // Compat flags 0x00000001 reverse to 0x80000000 and print without leading zeros.
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hvc1", 0, 0, hvcC(0, false, 1, 0x00000001, zeros(6), 30)))) == "hvc1.1.80000000.L30");
    std::string warning;
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hvc1", 0, 0, {})), &warning).empty());
    CHECK(warning == "missing hvcC");
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("hvc1", 0, 0, box("hvcC", zeros(10)))), &warning).empty());
    CHECK(warning == "truncated hvcC");
}

TEST_CASE("media codecs: AVC avc1.PPCCLL", "[media][codecs]") {
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("avc1", 1920, 1080, avcC(0x64, 0x00, 0x1f)))) == "avc1.64001F");
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("avc3", 0, 0, avcC(0x42, 0xc0, 0x1e)))) == "avc3.42C01E");
    std::string warning;
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("avc1", 0, 0, {})), &warning).empty());
    CHECK(warning == "missing avcC");
}

TEST_CASE("media codecs: AAC object types from the esds AudioSpecificConfig", "[media][codecs]") {
    // Returns the entry bytes (not a Box): the view handed to codecsFromSampleEntry must outlive the call.
    auto mp4a = [](const Bytes& esds_box) { return audioEntry("mp4a", 0, 2, 48000, {}, esds_box); };
    // On-air UniSoft esds (DSI 11 90: AOT 2, sfi 3, 2 channels).
    const Bytes onair = fullBox("esds", 0, 0, {0x03, 0x19, 0x00, 0x00, 0x00, 0x04, 0x11, 0x40, 0x15, 0x00, 0x20, 0x00, 0x00, 0x01, 0xf4,
                                               0x00, 0x00, 0x01, 0xf4, 0x00, 0x05, 0x02, 0x11, 0x90, 0x06, 0x01, 0x02});
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(onair))) == "mp4a.40.2");
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 2))))) == "mp4a.40.2");
    // Explicit hierarchical HE-AAC v1 / v2.
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(5, 3, 2))))) == "mp4a.40.5");
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(29, 3, 2))))) == "mp4a.40.29");
    // Backward-compatible signalling: sync 0x2B7 + SBR, then 0x548 + PS.
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 2, true, true))))) == "mp4a.40.5");
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 2, true, true, true, true))))) == "mp4a.40.29");
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 2, true, false))))) == "mp4a.40.2");
    // AOT escape (31 + 6 bits).
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(42, 3, 2))))) == "mp4a.40.42");
    // PCE inline (channelConfiguration 0): no attempt to skip the config.
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 0))))) == "mp4a.40.2");
    // 4-byte expandable descriptor sizes (0x80 continuation bytes).
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, audioSpecificConfig(2, 3, 2), true, 3)))) == "mp4a.40.2");
    // Other object type indications are mp4a.XX.
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x6b, {})))) == "mp4a.6B");
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x67, {0x11, 0x90})))) == "mp4a.67");
    std::string warning;
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(esds(0x40, {}, false))), &warning).empty());
    CHECK(warning == "missing DecoderSpecificInfo");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("mp4a", 0, 2, 48000, {}, {})), &warning).empty());
    CHECK(warning == "missing esds");
    // QuickTime: esds wrapped in a 'wave' child.
    const Bytes wave = box("wave", cat({box("frma", fourcc("mp4a")), esds(0x40, audioSpecificConfig(2, 3, 2)), u32(0), u32(0)}));
    CHECK(media::codecsFromSampleEntry(entryOf(mp4a(wave))) == "mp4a.40.2");
}

TEST_CASE("media codecs: AudioSampleEntry version 1/2 layouts (ISO and QuickTime)", "[media][codecs]") {
    const Bytes es = esds(0x40, audioSpecificConfig(2, 3, 2));
    // ISO 14496-12:2015 AudioSampleEntryV1: version 1, children at 28.
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("mp4a", 1, 2, 48000, {}, es))) == "mp4a.40.2");
    // QuickTime sound description v1: 16 extra bytes, children at 44.
    const Bytes qt1 = cat({u32(1024), u32(0), u32(0), u32(2)});
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("mp4a", 1, 2, 48000, qt1, es))) == "mp4a.40.2");
    // QuickTime v2: 36 extra bytes with the real rate (double) and channel count (u32); children at 64.
    const double rate = 96000.0;
    std::uint64_t rate_bits = 0;
    static_assert(sizeof rate == sizeof rate_bits);
    std::memcpy(&rate_bits, &rate, sizeof rate_bits);
    const Bytes qt2 = cat({u32(72), u64(rate_bits), u32(8), u32(0x7f000000), u32(16), u32(0), u32(0), u32(0)});  // 36 bytes
    const Bytes entry = audioEntry("mp4a", 2, 3, 1, qt2, es);
    CHECK(media::codecsFromSampleEntry(entryOf(entry)) == "mp4a.40.2");
    TrakSpec t;
    t.track_id = 2;
    t.handler = "soun";
    t.timescale = 96000;
    t.stsd_entry = entry;
    media::InitSegmentInfo info;
    REQUIRE(media::parseInitSegment(view(initSegment(t)), info));
    REQUIRE(info.tracks.size() == 1);
    CHECK(info.tracks[0].channels == 8);
    CHECK(info.tracks[0].sample_rate == 96000);
    CHECK(info.tracks[0].codecs == "mp4a.40.2");
}

TEST_CASE("media codecs: AC-4 from dac4 (TS 103 190-2 Annex E), lower-case hex as broadcast", "[media][codecs]") {
    // On-air Sinclair dac4 (29 bytes): bitstream_version 2, presentation_version 1, mdcompat 1 (5.1).
    const Bytes onair = box("dac4", {0x20, 0xa6, 0x01, 0x40, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xff, 0xff, 0xe0, 0x01, 0x0f, 0xf9, 0x80, 0x00, 0x00, 0x48,
                                     0x00, 0x00, 0x8e, 0x50, 0x10, 0x00, 0x00, 0x8f, 0x00, 0x80});
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 6, 48000, {}, onair))) == "ac-4.02.01.01");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(2, 1, 31, 0)))) == "ac-4.02.01.00");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(2, 2, 31, 3)))) == "ac-4.02.02.03");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(2, 1, 31, 1, true)))) == "ac-4.02.01.01");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(1, 0, 1, 2)))) == "ac-4.01.00.02");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(0x1f, 1, 31, 0x7)))) == "ac-4.1f.01.07");
    std::string warning;
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, dac4(2, 1, 6, 0))), &warning).empty());
    CHECK(warning == "missing mdcompat");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, {})), &warning).empty());
    CHECK(warning == "missing dac4");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-4", 0, 2, 48000, {}, box("dac4", {0x20, 0xa6}))), &warning).empty());
    CHECK(warning == "truncated dac4");
}

TEST_CASE("media codecs: bare 4CC codecs, MPEG-H, captions, VP9, AV1, CENC and unknown entries", "[media][codecs]") {
    std::string warning;
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ec-3", 0, 6, 48000, {}, box("dec3", zeros(5))))) == "ec-3");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("ac-3", 0, 2, 48000, {}, box("dac3", zeros(3))))) == "ac-3");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("Opus", 0, 2, 48000, {}, box("dOps", zeros(11))))) == "opus");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("fLaC", 0, 2, 48000, {}, {}))) == "flac");
    CHECK(media::codecsFromSampleEntry(entryOf(plainEntry("wvtt", box("vttC", ascii("WEBVTT"))))) == "wvtt");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("mhm1", 0, 2, 48000, {}, box("mhaC", {0x01, 0x0d, 0x13, 0x00, 0x00})))) == "mhm1.0x0D");
    CHECK(media::codecsFromSampleEntry(entryOf(audioEntry("mhm1", 0, 2, 48000, {}, {})), &warning).empty());
    CHECK(warning == "missing mhaC");
    CHECK(media::codecsFromSampleEntry(entryOf(stppEntry())) == "stpp.ttml.im1t");
    CHECK(media::codecsFromSampleEntry(entryOf(stppEntry(box("btrt", zeros(12))))) == "stpp.ttml.im1t");
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("vp09", 0, 0, fullBox("vpcC", 1, 0, {0x00, 0x1f, 0x80, 1, 1, 1, 0, 0})))) == "vp09.00.31.08");
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("av01", 0, 0, box("av1C", {0x81, 0x08, 0x40, 0x00})))) == "av01.0.08M.10");
    // CENC: the codec config stays a direct child, the original format comes from sinf/frma.
    const Bytes sinf = box("sinf", cat({box("frma", fourcc("hvc1")), fullBox("schm", 0, 0, cat({fourcc("cenc"), u32(0x10000)})),
                                        box("schi", fullBox("tenc", 0, 0, cat({u8(0), u8(0), u8(1), u8(8), zeros(16)})))}));
    const Bytes encv = visualEntry("encv", 1920, 1080, cat({hvcC(0, false, 2, 0x20000000, {0xb0, 0, 0, 0, 0, 0}, 123), sinf}));
    CHECK(media::codecsFromSampleEntry(entryOf(encv)) == "hvc1.2.4.L123.B0");
    CHECK(media::codecsFromSampleEntry(entryOf(visualEntry("encv", 0, 0, hvcC(0, false, 2, 0x20000000, zeros(6), 123))), &warning).empty());
    CHECK(warning == "missing sinf/frma");
    CHECK(media::codecsFromSampleEntry(entryOf(plainEntry("xyz ")), &warning).empty());
    CHECK(warning == "unknown sample entry xyz ");
}

TEST_CASE("media codecs: sameCodecs compares numerically", "[media][codecs]") {
    CHECK(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.4.L120.b0"));
    CHECK(media::sameCodecs("hvc1.2.4.L120.B0.0.0", "hvc1.2.4.L120.B0"));
    CHECK(media::sameCodecs("hvc1.2.4.L120.B0.00.00.00.00.00", "hvc1.2.4.L120.B0"));
    CHECK(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.04.l120.B0"));
    CHECK(media::sameCodecs("hev1.1.6.L93", "hev1.1.06.L93.0"));
    CHECK(media::sameCodecs("hvc1.A1.6.H93.B0.00.00.00.23", "hvc1.a1.6.h93.b0.0.0.0.23"));
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.4.L120.90"));  // the UniSoft MPD mismatch is real
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hvc1.2.4.L120.B0"));
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.4.L123.B0"));
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.4.H120.B0"));
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.6.L120.B0"));
    CHECK_FALSE(media::sameCodecs("hev1.2.4.L120.B0", "hev1.2.4.L120.B0.00.23"));
    CHECK(media::sameCodecs("mp4a.40.2", "mp4a.40.02"));
    CHECK(media::sameCodecs("mp4a.6B", "mp4a.6b"));
    CHECK_FALSE(media::sameCodecs("mp4a.40.2", "mp4a.40.5"));
    CHECK(media::sameCodecs("ac-4.02.01.01", "ac-4.2.1.1"));
    CHECK_FALSE(media::sameCodecs("ac-4.02.01.01", "ac-4.02.01.00"));
    CHECK(media::sameCodecs("avc1.64001F", "avc1.64001f"));
    CHECK(media::sameCodecs("mhm1.0x0D", "mhm1.0x0d"));
    CHECK(media::sameCodecs("stpp.ttml.im1t", "stpp.ttml.imt1"));  // the on-air typo
    CHECK_FALSE(media::sameCodecs("stpp.ttml.im1t", "stpp.ttml.im1i"));
    CHECK_FALSE(media::sameCodecs("", "hev1"));
    CHECK_FALSE(media::sameCodecs("hev1", "hev1.2"));
}
