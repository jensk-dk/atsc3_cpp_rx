// ISOBMFF helpers (spec 5.10.3, 5.17.3): box walking, init-segment and moof inspection on the on-air MPU DUs of the
// UniSoft and Sinclair/BWI encoders (spec 3.2), and the MSE rewrites (spec 5.10.3): cleaned init segments and rebuilt fragments, re-parsed and checked byte for byte.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "atsc3/media/isobmff.hpp"
#include "mp4_builders.hpp"

using namespace atsc3;
using namespace atsc3::test::mp4;

namespace {

/// One on-air asset: the FT=0 DU (ftyp mmpu moov) and the FT=1 DU (moof + 8-byte mdat header) with the on-air facts of spec 3.2.
struct OnAir {
    const char* name;
    const char* init;
    const char* moof;
    std::uint32_t movie_timescale;
    std::uint32_t mmpu_seq;
    const char* asset_id;
    std::uint32_t media_track, hint_track;
    const char* handler;
    const char* language;
    const char* entry;
    const char* codecs;
    std::uint32_t tfhd_flags;
    std::uint64_t tfdt;
    std::size_t tfdt_field;
    std::uint32_t sample_count;
    std::int32_t data_offset;
    std::size_t data_offset_field;
    std::uint32_t first_size;
    std::uint64_t total_size, total_duration;
    std::uint64_t mdat_announced;
    std::size_t moof_size, media_traf_size;
    std::size_t clean_size;
};
const OnAir kOnAir[] = {
    {"unisoft/20", "unisoft-20-init.mp4", "unisoft-20-moof.m4s", 1000000, 996, "videoasset01", 1, 2, "vide", "und", "hev1", "hev1.2.4.L120.B0",
     0x020022, 1991990000, 68, 60, 880, 92, 267371, 1925363, 2002020, 1927411, 872, 796, 769},
    {"unisoft/21", "unisoft-21-init.mp4", "unisoft-21-moof.m4s", 1000000, 996, "audioasset02", 2, 3, "soun", "und", "mp4a", "mp4a.40.2",
     0x020002, 1991992000, 64, 94, 1280, 88, 341, 32085, 2005302, 35289, 1272, 1196, 32 + 8 + 108 + 420 + 40},
    {"bwi/100", "bwi-100-init.mp4", "bwi-100-moof.m4s", 240000, 1676551952, "videoasset01", 1, 2, "vide", "und", "hev1", "hev1.2.4.L93.B0",
     0x02002a, 0, 116, 30, 568, 140, 42668, 195692, 240240, 196720, 560, 484, 806},
    {"bwi/200", "bwi-200-init.mp4", "bwi-200-moof.m4s", 240000, 1676551953, "audioasset02", 2, 3, "soun", "eng", "ac-4", "ac-4.02.01.01",
     0x02002a, 0, 116, 30, 684, 140, 401, 12024, 240240, 13052, 676, 600, 32 + 8 + 108 + 418 + 40},
    {"bwi/300", "bwi-300-init.mp4", "bwi-300-moof.m4s", 240000, 1676551953, "subtitleasset04", 4, 5, "subt", "eng", "stpp", "stpp.ttml.im1t",
     0x02002a, 0, 116, 1, 216, 140, 2570, 2570, 240240, 2612, 208, 132, 32 + 8 + 108 + 409 + 40},
};

std::vector<std::string> types(const std::vector<media::Box>& b) {
    std::vector<std::string> t;
    for (const media::Box& x : b) t.push_back(x.type);
    return t;
}
Bytes slice(const Bytes& b, std::size_t off, std::size_t n) { return Bytes(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + n)); }
/// Synthetic sample bytes of the given size (a pattern that depends on the index).
Bytes pattern(std::size_t size, std::uint8_t seed) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(seed + i);
    return b;
}

}  // namespace

TEST_CASE("media isobmff: box walking handles largesize, uuid, size 0 and stops at a bad header", "[media][isobmff]") {
    const Bytes usertype = {0x6d, 0x1d, 0x9b, 0x05, 0x42, 0xd5, 0x44, 0xe6, 0x80, 0xe2, 0x14, 0x1d, 0xaf, 0xf7, 0x57, 0xb2};
    const Bytes data = cat({box("free", ascii("abcd")), largeBox("mdat", ascii("xyz")), uuidBox(usertype, ascii("tfxd")),
                            cat({u32(0), fourcc("skip"), ascii("to the end")})});
    const std::vector<media::Box> b = media::boxes(view(data));
    REQUIRE(b.size() == 4);
    CHECK(b[0].type == "free");
    CHECK(b[0].offset == 0);
    CHECK(b[0].size == 12);
    CHECK(b[0].header == 8);
    CHECK(b[0].payload.size() == 4);
    CHECK(b[1].type == "mdat");
    CHECK(b[1].offset == 12);
    CHECK(b[1].size == 19);
    CHECK(b[1].header == 16);
    CHECK(std::string(b[1].payload.begin(), b[1].payload.end()) == "xyz");
    CHECK(b[2].type == "uuid");
    CHECK(b[2].uuid == "6d1d9b0542d544e680e2141daff757b2");
    CHECK(b[2].header == 24);
    CHECK(b[2].size == 28);
    CHECK(std::string(b[2].payload.begin(), b[2].payload.end()) == "tfxd");
    CHECK(b[3].type == "skip");
    CHECK(b[3].size == 18);
    CHECK(b[3].offset + b[3].size == data.size());
    CHECK(std::string(b[3].payload.begin(), b[3].payload.end()) == "to the end");

    // findBox / findPath / FullBox accessors
    const Bytes nested = box("moov", cat({fullBox("mvhd", 1, 0x123456, zeros(4)), box("trak", box("tkhd", ascii("1234")))}));
    const std::optional<media::Box> mvhd = media::findPath(view(nested), {"moov", "mvhd"});
    REQUIRE(mvhd);
    CHECK(media::fullBoxVersion(*mvhd) == 1);
    CHECK(media::fullBoxFlags(*mvhd) == 0x123456);
    REQUIRE(media::findPath(view(nested), {"moov", "trak", "tkhd"}));
    CHECK_FALSE(media::findPath(view(nested), {"moov", "trak", "mdia"}));
    CHECK_FALSE(media::findBox(view(nested), "trak"));  // direct children only

    // Malformed headers stop the walk: size < header, size past the end, truncated largesize, truncated uuid.
    CHECK(media::boxes(view(cat({box("free", {}), u32(3), fourcc("bad ")}))).size() == 1);
    CHECK(media::boxes(view(cat({box("free", {}), u32(100), fourcc("bad ")}))).size() == 1);
    CHECK(media::boxes(view(cat({box("free", {}), u32(1), fourcc("bad "), u32(0)}))).size() == 1);
    CHECK(media::boxes(view(cat({box("free", {}), u32(20), fourcc("uuid"), zeros(12)}))).size() == 1);
    CHECK(media::boxes(ByteView()).empty());
    CHECK(media::boxes(view(ascii("abc"))).empty());
}

TEST_CASE("media isobmff: parseInitSegment on the on-air MPU metadata DUs", "[media][isobmff]") {
    for (const OnAir& a : kOnAir) {
        INFO(a.name);
        const Bytes init = loadVector(a.init);
        REQUIRE_FALSE(init.empty());
        media::InitSegmentInfo info;
        std::string error;
        REQUIRE(media::parseInitSegment(view(init), info, &error));
        CHECK(error.empty());
        CHECK(info.top_level == std::vector<std::string>{"ftyp", "mmpu", "moov"});
        CHECK(info.major_brand == "mpuf");
        CHECK(info.compatible_brands == std::vector<std::string>{"isom", "mpuf"});
        CHECK(info.movie_timescale == a.movie_timescale);
        CHECK(info.has_mmpu);
        REQUIRE(info.mmpu_sequence_number);
        CHECK(*info.mmpu_sequence_number == a.mmpu_seq);
        CHECK(info.mmpu_asset_id == a.asset_id);
        CHECK(info.warnings.empty());
        REQUIRE(info.tracks.size() == 2);
        const media::TrackInfo& m = info.tracks[0];
        const media::TrackInfo& h = info.tracks[1];
        CHECK(info.mediaTrack() == &m);
        CHECK(info.track(a.media_track) == &m);
        CHECK(info.track(a.hint_track) == &h);
        CHECK(info.track(99) == nullptr);
        CHECK(m.track_id == a.media_track);
        CHECK(m.handler == a.handler);
        CHECK(m.timescale == a.movie_timescale);  // both encoders use the movie timescale for the track
        CHECK(m.language == a.language);
        CHECK(m.sample_entry == a.entry);
        CHECK(m.codecs == a.codecs);
        CHECK_FALSE(m.encrypted);
        CHECK(m.original_format.empty());
        CHECK_FALSE(m.isHint());
        CHECK(m.trex_default_sample_duration == 1);
        CHECK(m.trex_default_sample_size == 0);
        CHECK(m.trex_default_sample_flags == 0x00010000);
        CHECK(h.track_id == a.hint_track);
        CHECK(h.handler == "hint");
        CHECK(h.isHint());
        CHECK(h.kind() == "data");
        CHECK(h.sample_entry == "mmth");
        CHECK(h.codecs.empty());
        CHECK(h.trex_default_sample_size == 34);
        CHECK(h.trex_default_sample_duration == 0);
        CHECK(h.timescale == a.movie_timescale);
    }
    // Per-asset details: video geometry, audio fields, kinds, MIME types, the invalid packed language.
    media::InitSegmentInfo v, s, b, c;
    REQUIRE(media::parseInitSegment(view(loadVector("unisoft-20-init.mp4")), v));
    CHECK(v.tracks[0].width == 1920);
    CHECK(v.tracks[0].height == 1080);
    CHECK_FALSE(v.tracks[0].channels);
    CHECK(v.tracks[0].kind() == "video");
    CHECK(v.tracks[0].mimeType() == "video/mp4; codecs=\"hev1.2.4.L120.B0\"");
    CHECK(v.tracks[0].warnings.empty());
    REQUIRE(media::parseInitSegment(view(loadVector("unisoft-21-init.mp4")), s));
    CHECK(s.tracks[0].channels == 2);
    CHECK(s.tracks[0].sample_rate == 48000);
    CHECK_FALSE(s.tracks[0].width);
    CHECK(s.tracks[0].kind() == "audio");
    CHECK(s.tracks[0].mimeType() == "audio/mp4; codecs=\"mp4a.40.2\"");
    REQUIRE(s.tracks[0].warnings.size() == 1);
    CHECK(s.tracks[0].warnings[0] == "invalid mdhd language 0x94a0");  // the ATEME/Bento4 pad-bit bug -> "und"
    REQUIRE(media::parseInitSegment(view(loadVector("bwi-200-init.mp4")), b));
    CHECK(b.tracks[0].channels == 6);
    CHECK(b.tracks[0].sample_rate == 48000);
    CHECK(b.tracks[0].language == "eng");
    REQUIRE(media::parseInitSegment(view(loadVector("bwi-300-init.mp4")), c));
    CHECK(c.tracks[0].kind() == "subtitles");
    CHECK(c.tracks[0].containerMime() == "application/mp4");
    CHECK(c.tracks[0].mimeType() == "application/mp4; codecs=\"stpp.ttml.im1t\"");
    CHECK(c.tracks[1].mimeType() == "application/mp4");
}

TEST_CASE("media isobmff: parseInitSegment on ROUTE inits incl. CENC encv/enca", "[media][isobmff]") {
    media::InitSegmentInfo video, audio, plain;
    REQUIRE(media::parseInitSegment(view(loadVector("route-bwi-wbal-video-init.mp4")), video));
    CHECK(video.top_level == std::vector<std::string>{"ftyp", "moov"});
    CHECK(video.major_brand == "hvc1");
    CHECK(video.compatible_brands == std::vector<std::string>{"hvc1", "iso6", "dash", "cmfc"});
    CHECK_FALSE(video.has_mmpu);
    CHECK_FALSE(video.mmpu_sequence_number);
    REQUIRE(video.tracks.size() == 1);
    CHECK(video.tracks[0].sample_entry == "encv");
    CHECK(video.tracks[0].original_format == "hvc1");
    CHECK(video.tracks[0].encrypted);
    CHECK(video.tracks[0].codecs == "hvc1.2.4.L123.B0");
    CHECK(video.tracks[0].width == 1920);
    CHECK(video.tracks[0].height == 1080);
    CHECK(video.tracks[0].timescale == 240000);
    CHECK(video.tracks[0].trex_default_sample_duration == 240000);
    CHECK(video.mediaTrack() == &video.tracks[0]);
    REQUIRE(media::parseInitSegment(view(loadVector("route-bwi-wbal-a0-init.mp4")), audio));
    CHECK(audio.major_brand == "M4A ");
    CHECK(audio.tracks[0].sample_entry == "enca");
    CHECK(audio.tracks[0].original_format == "ac-4");
    CHECK(audio.tracks[0].encrypted);
    CHECK(audio.tracks[0].codecs == "ac-4.02.01.01");
    CHECK(audio.tracks[0].channels == 6);
    CHECK(audio.tracks[0].sample_rate == 48000);
    CHECK(audio.tracks[0].language == "eng");
    REQUIRE(media::parseInitSegment(view(loadVector("route-uni-usc02-video-init.mp4v")), plain));
    CHECK(plain.compatible_brands == std::vector<std::string>{"iso6", "dash"});
    CHECK(plain.tracks[0].codecs == "hev1.2.4.L120.B0");
    CHECK(plain.tracks[0].timescale == 1000000);
    CHECK(plain.tracks[0].trex_default_sample_duration == 1000000);
}

TEST_CASE("media isobmff: parseInitSegment synthetic (version 1 headers, missing boxes, no moov)", "[media][isobmff]") {
    media::InitSegmentInfo info;
    std::string error;
    CHECK_FALSE(media::parseInitSegment(view(cat({ftyp("iso6", {"iso6"}), box("free", {})})), info, &error));
    CHECK(error == "no moov box");
    CHECK(info.top_level == std::vector<std::string>{"ftyp", "free"});

    TrakSpec t;
    t.track_id = 7;
    t.handler = "vide";
    t.timescale = 60000;
    t.language = 0x15c7;  // eng
    t.version = 1;
    t.stsd_entry = visualEntry("hvc1", 1280, 720, hvcC(0, false, 1, 0x60000000, {0x90, 0, 0, 0, 0, 0}, 93));
    const Bytes init = cat({ftyp("iso6", {"iso6", "dash"}), box("moov", cat({mvhd(1000, 1), trak(t), box("mvex", trex(7, 1001, 0, 0x01010000))}))});
    REQUIRE(media::parseInitSegment(view(init), info, &error));
    CHECK(info.movie_timescale == 1000);
    REQUIRE(info.tracks.size() == 1);
    CHECK(info.tracks[0].track_id == 7);
    CHECK(info.tracks[0].timescale == 60000);
    CHECK(info.tracks[0].language == "eng");
    CHECK(info.tracks[0].codecs == "hvc1.1.6.L93.90");
    CHECK(info.tracks[0].width == 1280);
    CHECK(info.tracks[0].trex_default_sample_duration == 1001);
    CHECK(info.tracks[0].trex_default_sample_flags == 0x01010000);
    CHECK(info.tracks[0].warnings.empty());

    // A trak with only a tkhd: everything else is reported missing; a moov without trak warns.
    const Bytes bare = box("moov", cat({mvhd(1000), box("trak", fullBox("tkhd", 0, 7, cat({u32(0), u32(0), u32(3), zeros(68)})))}));
    REQUIRE(media::parseInitSegment(view(bare), info));
    REQUIRE(info.tracks.size() == 1);
    CHECK(info.tracks[0].track_id == 3);
    CHECK(info.tracks[0].warnings == std::vector<std::string>{"missing mdia"});
    CHECK(info.tracks[0].language == "und");
    REQUIRE(media::parseInitSegment(view(box("moov", mvhd(1000))), info));
    CHECK(info.tracks.empty());
    CHECK(info.warnings == std::vector<std::string>{"moov without trak"});
    // Without hdlr the handler comes from the sample-entry class; a truncated mmpu is flagged.
    const Bytes nohdlr = cat({box("mmpu", zeros(6)), box("moov", box("trak", cat({fullBox("tkhd", 0, 7, cat({u32(0), u32(0), u32(2), zeros(68)})),
                                                                                       box("mdia", box("minf", box("stbl", fullBox("stsd", 0, 0, cat({u32(1), audioEntry("ac-3", 0, 2, 48000, {}, {})})))))})))});
    REQUIRE(media::parseInitSegment(view(nohdlr), info));
    CHECK(info.has_mmpu);
    CHECK(info.warnings == std::vector<std::string>{"truncated mmpu"});
    REQUIRE(info.tracks.size() == 1);
    CHECK(info.tracks[0].handler == "soun");
    CHECK(info.tracks[0].codecs == "ac-3");
    CHECK(info.tracks[0].channels == 2);
    CHECK(info.tracks[0].warnings == std::vector<std::string>{"missing hdlr", "missing mdhd"});
}

TEST_CASE("media isobmff: parseMoof on the on-air FT=1 DUs (moof + mdat header)", "[media][isobmff]") {
    for (const OnAir& a : kOnAir) {
        INFO(a.name);
        const Bytes du = loadVector(a.moof);
        REQUIRE(du.size() == a.moof_size + 8);
        media::MoofInfo m;
        std::string error;
        REQUIRE(media::parseMoof(view(du), m, &error));
        CHECK(error.empty());
        CHECK(m.warnings.empty());
        CHECK(m.sequence_number == 1);
        CHECK(m.moof_offset == 0);
        CHECK(m.moof_size == a.moof_size);
        REQUIRE(m.mdat_size);
        CHECK(*m.mdat_size == a.mdat_announced);
        CHECK(m.mdat_header_size == 8);
        REQUIRE(m.trafs.size() == 2);
        const media::TrackFragmentInfo& media_traf = m.trafs[0];
        const media::TrackFragmentInfo& hint = m.trafs[1];
        CHECK(m.traf(a.media_track) == &media_traf);
        CHECK(m.traf(a.hint_track) == &hint);
        CHECK(m.traf(42) == nullptr);
        CHECK(media_traf.track_id == a.media_track);
        CHECK(media_traf.tfhd_flags == a.tfhd_flags);
        CHECK(media_traf.default_base_is_moof);
        CHECK_FALSE(media_traf.base_data_offset);
        REQUIRE(media_traf.tfdt);
        CHECK(*media_traf.tfdt == a.tfdt);
        CHECK(media_traf.tfdt_version == 1);
        CHECK(media_traf.tfdt_field_offset == a.tfdt_field);
        REQUIRE(media_traf.data_offset);
        CHECK(*media_traf.data_offset == a.data_offset);
        CHECK(*media_traf.data_offset == static_cast<std::int32_t>(a.moof_size + 8));
        CHECK(media_traf.data_offset_field_offset == a.data_offset_field);
        CHECK(media_traf.trun_count == 1);
        REQUIRE(media_traf.samples.size() == a.sample_count);
        CHECK(media_traf.samples[0].size == a.first_size);
        CHECK(media_traf.total_size == a.total_size);
        CHECK(media_traf.total_duration == a.total_duration);
        CHECK(media_traf.traf_offset == 24);
        CHECK(media_traf.traf_size == a.media_traf_size);
        // The announced mdat holds the media samples plus one 34-byte hint sample per sample (spec 3.2).
        CHECK(*m.mdat_size == 8 + a.total_size + 34ull * a.sample_count);
        // Hint traf: tfhd 0x020018 (default duration 1, size 34), trun with data_offset only.
        CHECK(hint.track_id == a.hint_track);
        CHECK(hint.tfhd_flags == 0x020018);
        CHECK_FALSE(hint.tfdt);
        CHECK(hint.tfdt_field_offset == 0);
        REQUIRE(hint.samples.size() == a.sample_count);
        CHECK(hint.samples[0].size == 34);
        CHECK(hint.samples[0].duration == 1);
        CHECK(hint.total_size == 34ull * a.sample_count);
        CHECK(hint.traf_offset + hint.traf_size == a.moof_size);
    }
    // Per-encoder sample details: UniSoft video has first_sample_flags and composition offsets, tfhd default flags
    // for the rest; BWI video gets its duration from the tfhd default; BWI audio carries per-sample flags.
    media::MoofInfo u, b, s;
    REQUIRE(media::parseMoof(view(loadVector("unisoft-20-moof.m4s")), u));
    CHECK(u.trafs[0].samples[0].flags == 0x02000000);
    CHECK(u.trafs[0].samples[1].flags == 0x01010000);
    CHECK(u.trafs[0].samples[0].duration == 33367);
    CHECK(u.trafs[0].samples[0].composition_offset == 133467);
    CHECK(u.trafs[0].samples[1].composition_offset == 433767);
    CHECK(u.trafs[0].samples[1].size == 102492);
    REQUIRE(media::parseMoof(view(loadVector("bwi-100-moof.m4s")), b));
    CHECK(b.trafs[0].samples[0].duration == 8008);
    CHECK(b.trafs[0].samples[29].duration == 8008);
    CHECK(b.trafs[0].samples[0].flags == 0x02000000);
    CHECK(b.trafs[0].samples[1].flags == 0x01010000);
    CHECK(b.trafs[0].samples[0].composition_offset == 8008);
    CHECK(b.trafs[0].samples[2].composition_offset == 0);
    REQUIRE(media::parseMoof(view(loadVector("bwi-200-moof.m4s")), s));
    CHECK(s.trafs[0].samples[0].flags == 0);
    CHECK(s.trafs[0].samples[1].size == 400);
    CHECK(s.trafs[0].samples[0].composition_offset == 0);
}

TEST_CASE("media isobmff: parseMoof synthetic (tfdt v0, defaults, base_data_offset, two truns, largesize mdat)", "[media][isobmff]") {
    const std::vector<TrunSampleSpec> a = {{10, 100, 0x01010000, -5}, {10, 200, 0x01010000, 7}};
    const std::vector<TrunSampleSpec> b = {{0, 300, 0, 0}};
    const Bytes traf = box("traf", cat({tfhd(1, 0x1 | 0x8 | 0x10 | 0x20 | 0x10000, 4096, 1, 20, 50, 0x00010000), tfdt(0, 12345),
                                        trun(0x1 | 0x4 | 0x200 | 0x800, a, 64, 0x02000000, 1), trun(0x200, b)}));
    const Bytes du = cat({box("free", {}), moof(9, {traf}), mdatHeader(600, true)});
    media::MoofInfo m;
    REQUIRE(media::parseMoof(view(du), m));
    CHECK(m.sequence_number == 9);
    CHECK(m.moof_offset == 8);
    CHECK(m.moof_size == moof(9, {traf}).size());
    REQUIRE(m.trafs.size() == 1);
    const media::TrackFragmentInfo& t = m.trafs[0];
    CHECK(t.track_id == 1);
    CHECK(t.tfhd_flags == 0x010039);
    CHECK_FALSE(t.default_base_is_moof);
    REQUIRE(t.base_data_offset);
    CHECK(*t.base_data_offset == 4096);
    CHECK(t.tfdt_version == 0);
    REQUIRE(t.tfdt);
    CHECK(*t.tfdt == 12345);
    CHECK(t.tfdt_field_offset == 8 + 8 + 16 + 8 + 36 + 8 + 4);  // free, moof hdr, mfhd, traf hdr, tfhd(36), tfdt hdr, FullBox
    CHECK(t.trun_count == 2);
    REQUIRE(t.data_offset);
    CHECK(*t.data_offset == 64);
    CHECK(t.data_offset_field_offset == t.tfdt_field_offset + 4 + 8 + 8);  // tfdt value, trun hdr, FullBox + sample_count
    REQUIRE(t.samples.size() == 3);
    CHECK(t.samples[0].duration == 20);  // tfhd default
    CHECK(t.samples[0].size == 100);
    CHECK(t.samples[0].flags == 0x02000000);  // first_sample_flags
    CHECK(t.samples[0].composition_offset == -5);  // version 1: signed
    CHECK(t.samples[1].flags == 0x00010000);  // tfhd default
    CHECK(t.samples[1].composition_offset == 7);
    CHECK(t.samples[2].size == 300);
    CHECK(t.samples[2].duration == 20);
    CHECK(t.samples[2].flags == 0x00010000);
    CHECK(t.samples[2].composition_offset == 0);
    CHECK(t.total_size == 600);
    CHECK(t.total_duration == 60);
    REQUIRE(m.mdat_size);
    CHECK(*m.mdat_size == 616);
    CHECK(m.mdat_header_size == 16);

    // Version-0 composition offsets are unsigned; without an mdat header nothing is announced; no moof fails.
    const Bytes v0 = moof(1, {box("traf", cat({tfhd(1, 0x20000), trun(0x200 | 0x800, {{0, 1, 0, -1}})}))});
    REQUIRE(media::parseMoof(view(v0), m));
    CHECK(m.trafs[0].samples[0].composition_offset == 0xffffffff);
    CHECK(m.trafs[0].default_base_is_moof);
    CHECK_FALSE(m.trafs[0].data_offset);
    CHECK(m.trafs[0].data_offset_field_offset == 0);
    CHECK_FALSE(m.mdat_size);
    CHECK(m.mdat_header_size == 0);
    std::string error;
    CHECK_FALSE(media::parseMoof(view(box("free", {})), m, &error));
    CHECK(error == "no moof box");
    // A truncated trun is reported and the readable samples kept.
    const Bytes cut = moof(1, {box("traf", cat({tfhd(1, 0x20000), fullBox("trun", 0, 0x200, cat({u32(5), u32(1), u32(2)}))}))});
    REQUIRE(media::parseMoof(view(cut), m));
    CHECK(m.warnings == std::vector<std::string>{"truncated trun of track 1"});
    CHECK(m.trafs[0].samples.size() == 2);
}

TEST_CASE("media isobmff: cleanInitSegment turns the on-air MPU metadata into ftyp+moov with one track", "[media][isobmff]") {
    for (const OnAir& a : kOnAir) {
        INFO(a.name);
        const Bytes init = loadVector(a.init);
        media::CleanInitResult r;
        std::string error;
        REQUIRE(media::cleanInitSegment(view(init), media::CleanInitOptions{}, r, &error));
        CHECK(error.empty());
        CHECK(r.info.tracks.size() == 2);  // the input
        CHECK(r.info.has_mmpu);
        REQUIRE(r.hint_track_id);
        CHECK(*r.hint_track_id == a.hint_track);
        REQUIRE(r.hint_sample_size);
        CHECK(*r.hint_sample_size == 34);
        CHECK(r.dropped == std::vector<std::string>{"mmpu", "trak#" + std::to_string(a.hint_track), "trep#" + std::to_string(a.media_track),
                                                    "trex#" + std::to_string(a.hint_track)});
        CHECK(r.bytes.size() == a.clean_size);
        // Exact layout: the MSE ftyp, then moov{mvhd, media trak verbatim, mvex{media trex}}.
        const std::vector<media::Box> top = media::boxes(view(r.bytes));
        CHECK(types(top) == std::vector<std::string>{"ftyp", "moov"});
        CHECK(slice(r.bytes, 0, 32) == media::ftypBox("iso6", 0, {"iso6", "isom", "dash", "cmfc"}));
        const std::vector<media::Box> moov_children = media::boxes(top[1].payload);
        CHECK(types(moov_children) == std::vector<std::string>{"mvhd", "trak", "mvex"});
        const std::optional<media::Box> in_moov = media::findBox(view(init), "moov");
        REQUIRE(in_moov);
        const std::vector<media::Box> in_children = media::boxes(in_moov->payload);
        REQUIRE(in_children.size() == 4);  // mvhd trak trak mvex
        CHECK(in_children[1].type == "trak");
        CHECK(std::equal(moov_children[1].payload.begin(), moov_children[1].payload.end(), in_children[1].payload.begin(), in_children[1].payload.end()));
        CHECK(moov_children[1].size == in_children[1].size);
        CHECK(moov_children[2].size == 8 + 32);
        const std::vector<media::Box> mvex = media::boxes(moov_children[2].payload);
        REQUIRE(mvex.size() == 1);
        CHECK(mvex[0].type == "trex");
        CHECK(top[1].size == 8 + in_children[0].size + in_children[1].size + 40);
        // Re-parsed: one media track with the same codecs, no mmpu.
        media::InitSegmentInfo info;
        REQUIRE(media::parseInitSegment(view(r.bytes), info));
        CHECK(info.top_level == std::vector<std::string>{"ftyp", "moov"});
        CHECK(info.major_brand == "iso6");
        CHECK(info.compatible_brands == std::vector<std::string>{"iso6", "isom", "dash", "cmfc"});
        CHECK_FALSE(info.has_mmpu);
        CHECK(info.movie_timescale == a.movie_timescale);
        REQUIRE(info.tracks.size() == 1);
        CHECK(info.mediaTrack() == &info.tracks[0]);
        CHECK(info.tracks[0].track_id == a.media_track);
        CHECK(info.tracks[0].codecs == a.codecs);
        CHECK(info.tracks[0].handler == a.handler);
        CHECK(info.tracks[0].trex_default_sample_flags == 0x00010000);
        CHECK(info.tracks[0].trex_default_sample_duration == 1);
    }
    // Options: keep everything (only the trep goes), keep the ftyp, keep the hint track.
    const Bytes init = loadVector("unisoft-20-init.mp4");
    media::CleanInitResult r;
    media::CleanInitOptions o;
    o.rewrite_ftyp = false;
    o.keep_only_ftyp_moov = false;
    REQUIRE(media::cleanInitSegment(view(init), o, r));
    CHECK(types(media::boxes(view(r.bytes))) == std::vector<std::string>{"ftyp", "mmpu", "moov"});
    CHECK(slice(r.bytes, 0, 24 + 37) == slice(init, 0, 24 + 37));
    CHECK(r.bytes.size() == init.size() - 363 - 32 - 16);
    CHECK(r.dropped == std::vector<std::string>{"trak#2", "trep#1", "trex#2"});
    o.drop_hint_tracks = false;
    REQUIRE(media::cleanInitSegment(view(init), o, r));
    CHECK(r.bytes == init);
    CHECK(r.dropped.empty());
    CHECK(r.hint_track_id == 2);
    // Failures: no moov, or only hint tracks.
    std::string error;
    CHECK_FALSE(media::cleanInitSegment(view(slice(init, 0, 24)), media::CleanInitOptions{}, r, &error));
    CHECK(error == "no moov box");
    TrakSpec hint;
    hint.handler = "hint";
    hint.stsd_entry = plainEntry("mmth");
    CHECK_FALSE(media::cleanInitSegment(view(initSegment(hint)), media::CleanInitOptions{}, r, &error));
    CHECK(error == "no media track");
}

TEST_CASE("media isobmff: buildFragment rebuilds the on-air moof without the hint traf and writes the mdat", "[media][isobmff]") {
    for (const OnAir& a : kOnAir) {
        INFO(a.name);
        const Bytes du = loadVector(a.moof);
        media::MoofInfo in;
        REQUIRE(media::parseMoof(view(du), in));
        std::vector<Bytes> sample_bytes;
        std::vector<ByteView> samples;
        for (std::size_t i = 0; i < in.trafs[0].samples.size(); ++i) sample_bytes.push_back(pattern(in.trafs[0].samples[i].size, static_cast<std::uint8_t>(i)));
        for (const Bytes& s : sample_bytes) samples.push_back(view(s));
        media::BuildFragmentOptions o;
        o.drop_track_ids = {a.hint_track};
        media::BuildFragmentResult r;
        std::string error;
        REQUIRE(media::buildFragment(view(du), samples, o, r, &error));
        CHECK(error.empty());
        CHECK(r.dropped == std::vector<std::string>{"traf#" + std::to_string(a.hint_track)});
        CHECK(r.moof.trafs.size() == 2);  // the input
        CHECK(r.moof_size == a.moof_size - 52);  // the hint traf is 52 bytes on both encoders
        CHECK(r.mdat_payload == a.total_size);
        CHECK(r.bytes.size() == 24 + r.moof_size + 8 + a.total_size);
        CHECK(slice(r.bytes, 0, 24) == media::stypBox());
        // Re-parse the output after the styp: one traf, data_offset repaired, everything else as received.
        media::MoofInfo out;
        REQUIRE(media::parseMoof(view(r.bytes).subview(24), out));
        CHECK(out.warnings.empty());
        CHECK(out.moof_offset == 0);
        CHECK(out.moof_size == r.moof_size);
        CHECK(out.sequence_number == 1);
        REQUIRE(out.trafs.size() == 1);
        const media::TrackFragmentInfo& t = out.trafs[0];
        CHECK(t.track_id == a.media_track);
        CHECK(t.tfhd_flags == a.tfhd_flags);
        CHECK(t.traf_size == a.media_traf_size);
        REQUIRE(t.data_offset);
        CHECK(*t.data_offset == static_cast<std::int32_t>(r.moof_size + 8));
        REQUIRE(t.tfdt);
        CHECK(*t.tfdt == a.tfdt);
        CHECK(t.tfdt_version == 1);
        REQUIRE(t.samples.size() == a.sample_count);
        CHECK(t.total_size == a.total_size);
        CHECK(t.total_duration == a.total_duration);
        REQUIRE(out.mdat_size);
        CHECK(*out.mdat_size == 8 + a.total_size);
        CHECK(out.mdat_header_size == 8);
        // The mdat payload is the samples in order, starting exactly at data_offset.
        const std::size_t mdat_payload_at = 24 + static_cast<std::size_t>(*t.data_offset);
        std::size_t pos = mdat_payload_at;
        for (const Bytes& s : sample_bytes) {
            CHECK(slice(r.bytes, pos, s.size()) == s);
            pos += s.size();
        }
        CHECK(pos == r.bytes.size());
        // The media traf bytes are verbatim apart from the data_offset field (uuid tfxd on BWI included).
        const std::optional<media::Box> in_moof = media::findBox(view(du), "moof");
        const std::vector<media::Box> in_children = media::boxes(in_moof->payload);
        const std::vector<media::Box> out_children = media::boxes(media::findBox(view(r.bytes).subview(24), "moof")->payload);
        REQUIRE(in_children.size() == 3);
        REQUIRE(out_children.size() == 2);
        CHECK(types(out_children) == std::vector<std::string>{"mfhd", "traf"});
        Bytes expect(in_children[1].payload.begin(), in_children[1].payload.end());
        Bytes got(out_children[1].payload.begin(), out_children[1].payload.end());
        REQUIRE(expect.size() == got.size());
        const std::size_t field = a.data_offset_field - 24 - 8;  // within the traf payload
        const std::uint32_t off = static_cast<std::uint32_t>(r.moof_size + 8);
        expect[field] = static_cast<std::uint8_t>(off >> 24);
        expect[field + 1] = static_cast<std::uint8_t>(off >> 16);
        expect[field + 2] = static_cast<std::uint8_t>(off >> 8);
        expect[field + 3] = static_cast<std::uint8_t>(off);
        CHECK(expect == got);
    }
}

TEST_CASE("media isobmff: buildFragment tfdt rewrite, zero-filled tail, media_track_id and no styp", "[media][isobmff]") {
    const Bytes du = loadVector("bwi-100-moof.m4s");
    media::MoofInfo in;
    REQUIRE(media::parseMoof(view(du), in));
    const std::vector<media::TrunSample>& ts = in.trafs[0].samples;
    // Only the first two samples are given: the other 28 are zero-filled with their trun sizes (spec 5.10.3).
    const Bytes s0 = pattern(ts[0].size, 1), s1 = pattern(ts[1].size, 2);
    media::BuildFragmentOptions o;
    o.media_track_id = 1;
    o.drop_track_ids = {2};
    o.tfdt = 240240;  // BWI: tfdt restarts at 0 every MPU, rewritten from the MPU timestamp (spec 5.10.3 timeline rule)
    o.prepend_styp = false;
    media::BuildFragmentResult r;
    REQUIRE(media::buildFragment(view(du), {view(s0), view(s1)}, o, r));
    CHECK(r.moof_size == 560 - 52);
    CHECK(r.mdat_payload == 195692);
    CHECK(r.bytes.size() == r.moof_size + 8 + 195692);
    media::MoofInfo out;
    REQUIRE(media::parseMoof(view(r.bytes), out));
    REQUIRE(out.trafs.size() == 1);
    REQUIRE(out.trafs[0].tfdt);
    CHECK(*out.trafs[0].tfdt == 240240);
    CHECK(out.trafs[0].tfdt_version == 1);
    CHECK(out.trafs[0].tfdt_field_offset == 116 - 0);  // unchanged position: the tfdt was already version 1
    CHECK(*out.trafs[0].data_offset == static_cast<std::int32_t>(r.moof_size + 8));
    CHECK(out.trafs[0].samples.size() == 30);
    CHECK(slice(r.bytes, r.moof_size + 8, s0.size()) == s0);
    CHECK(slice(r.bytes, r.moof_size + 8 + s0.size(), s1.size()) == s1);
    const Bytes tail = slice(r.bytes, r.moof_size + 8 + s0.size() + s1.size(), 195692 - s0.size() - s1.size());
    CHECK(std::all_of(tail.begin(), tail.end(), [](std::uint8_t b) { return b == 0; }));
    // Without a sample list at all the mdat is entirely zero-filled.
    REQUIRE(media::buildFragment(view(du), {}, o, r));
    CHECK(r.mdat_payload == 195692);
    // Nothing dropped when the hint traf is not listed: it is copied verbatim and the first traf is the media one.
    media::BuildFragmentOptions keep;
    REQUIRE(media::buildFragment(view(du), {}, keep, r));
    CHECK(r.moof_size == 560);
    CHECK(r.dropped.empty());
    REQUIRE(media::parseMoof(view(r.bytes).subview(24), out));
    CHECK(out.trafs.size() == 2);
    CHECK(*out.trafs[0].data_offset == 568);
    // Failures: unknown media track, every traf dropped, no moof.
    std::string error;
    media::BuildFragmentOptions bad;
    bad.media_track_id = 9;
    CHECK_FALSE(media::buildFragment(view(du), {}, bad, r, &error));
    CHECK(error == "no media traf");
    bad.media_track_id.reset();
    bad.drop_track_ids = {1, 2};
    CHECK_FALSE(media::buildFragment(view(du), {}, bad, r, &error));
    CHECK(error == "no media traf");
    CHECK_FALSE(media::buildFragment(view(slice(du, 560, 8)), {}, keep, r, &error));
    CHECK(error == "no moof box");
}

TEST_CASE("media isobmff: buildFragment widens a version-0 tfdt and adds a missing data_offset field", "[media][isobmff]") {
    // Two truns without data_offset, tfdt version 0, a hint traf to drop.
    const std::vector<TrunSampleSpec> a = {{10, 3, 0, 0}, {10, 4, 0, 0}};
    const std::vector<TrunSampleSpec> b = {{10, 5, 0, 0}};
    const Bytes media_traf = box("traf", cat({tfhd(1, 0x20000), tfdt(0, 1000), trun(0x100 | 0x200, a), trun(0x100 | 0x200, b)}));
    const Bytes hint_traf = box("traf", cat({tfhd(2, 0x20018, 0, 1, 1, 34), trun(0x1, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 999)}));
    const Bytes du = cat({moof(5, {media_traf, hint_traf}), mdatHeader(12 + 3 * 34)});
    const Bytes s0 = ascii("abc"), s1 = ascii("defg"), s2 = ascii("hijkl");
    media::BuildFragmentOptions o;
    o.drop_track_ids = {2};
    o.tfdt = 0x100000000ull;  // needs 64 bits -> the box becomes version 1 (+4 bytes)
    media::BuildFragmentResult r;
    REQUIRE(media::buildFragment(view(du), {view(s0), view(s1), view(s2)}, o, r));
    CHECK(r.dropped == std::vector<std::string>{"traf#2"});
    CHECK(r.moof_size == moof(5, {media_traf}).size() + 4 + 4 + 4);  // tfdt widened, two data_offset fields added
    media::MoofInfo out;
    REQUIRE(media::parseMoof(view(r.bytes).subview(24), out));
    REQUIRE(out.trafs.size() == 1);
    const media::TrackFragmentInfo& t = out.trafs[0];
    CHECK(t.tfdt_version == 1);
    CHECK(*t.tfdt == 0x100000000ull);
    CHECK(t.trun_count == 2);
    CHECK(t.samples.size() == 3);
    CHECK(*t.data_offset == static_cast<std::int32_t>(r.moof_size + 8));
    CHECK(*out.mdat_size == 8 + 12);
    CHECK(slice(r.bytes, 24 + r.moof_size + 8, 12) == ascii("abcdefghijkl"));
    // The second trun points at its own first sample (3 + 4 bytes in).
    const std::vector<media::Box> traf_children = media::boxes(media::findPath(view(r.bytes).subview(24), {"moof", "traf"})->payload);
    REQUIRE(traf_children.size() == 4);
    CHECK(traf_children[1].type == "tfdt");
    CHECK(traf_children[1].size == 20);
    CHECK(traf_children[3].type == "trun");
    CHECK((media::fullBoxFlags(traf_children[3]) & 0x1) == 0x1);
    const std::uint32_t second = (static_cast<std::uint32_t>(traf_children[3].payload[8]) << 24) | (static_cast<std::uint32_t>(traf_children[3].payload[9]) << 16) |
                                 (static_cast<std::uint32_t>(traf_children[3].payload[10]) << 8) | traf_children[3].payload[11];
    CHECK(second == r.moof_size + 8 + 7);
    // A small value keeps the version-0 tfdt as is (no size change).
    o.tfdt = 2000;
    REQUIRE(media::buildFragment(view(du), {view(s0), view(s1), view(s2)}, o, r));
    CHECK(r.moof_size == moof(5, {media_traf}).size() + 4 + 4);
    REQUIRE(media::parseMoof(view(r.bytes).subview(24), out));
    CHECK(out.trafs[0].tfdt_version == 0);
    CHECK(*out.trafs[0].tfdt == 2000);
    // A traf without tfdt gets a version-1 tfdt after the tfhd when a rewrite is requested.
    const Bytes no_tfdt = cat({moof(5, {box("traf", cat({tfhd(1, 0x20000), trun(0x1 | 0x200, {{0, 2, 0, 0}}, 0)}))}), mdatHeader(2)});
    REQUIRE(media::buildFragment(view(no_tfdt), {view(s0)}, o, r));
    REQUIRE(media::parseMoof(view(r.bytes).subview(24), out));
    CHECK(out.trafs[0].tfdt_version == 1);
    CHECK(*out.trafs[0].tfdt == 2000);
    CHECK(types(media::boxes(media::findPath(view(r.bytes).subview(24), {"moof", "traf"})->payload)) == std::vector<std::string>{"tfhd", "tfdt", "trun"});
    CHECK(r.mdat_payload == 3);  // the given sample wins over the trun size
}

TEST_CASE("media isobmff: writers", "[media][isobmff]") {
    Bytes b;
    media::putU32(b, 0x01020304);
    media::putU64(b, 0x05060708090a0b0cull);
    CHECK(b == Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    b.clear();
    media::putBox(b, "free", view(ascii("xy")));
    CHECK(b == cat({u32(10), fourcc("free"), ascii("xy")}));
    CHECK(media::stypBox() == cat({u32(24), fourcc("styp"), fourcc("msdh"), u32(0), fourcc("msdh"), fourcc("msix")}));
    CHECK(media::stypBox().size() == 24);
    CHECK(media::ftypBox("iso6", 0, {"iso6", "isom", "dash", "cmfc"}) ==
          cat({u32(32), fourcc("ftyp"), fourcc("iso6"), u32(0), fourcc("iso6"), fourcc("isom"), fourcc("dash"), fourcc("cmfc")}));
    CHECK(media::ftypBox("M4A", 7, {}) == cat({u32(16), fourcc("ftyp"), fourcc("M4A "), u32(7)}));
}

TEST_CASE("media player views: decoder configs and fragment samples of the on-air MMT and ROUTE fixtures", "[media][player]") {
    using namespace atsc3;
    auto load = [](const std::string& rel) {
        std::ifstream f(std::string(ATSC3_VECTOR_DIR) + "/mmt/" + rel, std::ios::binary);
        REQUIRE(f);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    // WNUV pid 100: HEVC with the MMT hint track in the raw DU
    const auto vinit = load("bwi-100-init.mp4");
    std::vector<media::DecoderConfig> cfg;
    REQUIRE(media::decoderConfigs(ByteView(vinit.data(), vinit.size()), cfg));
    REQUIRE(cfg.size() == 1);   // the hint track is skipped
    CHECK(cfg[0].track.kind() == "video");
    CHECK(cfg[0].original_entry == "hev1");
    CHECK(cfg[0].extradata_box == "hvcC");
    CHECK(cfg[0].extradata.size() > 23);
    CHECK((cfg[0].extradata[0] == 1));   // configurationVersion
    CHECK(cfg[0].track.timescale == 240000u);   // WNUV video runs a 240 kHz track clock
    CHECK(cfg[0].track.width == 1280u);
    const auto vfrag = load("bwi-100-moof.m4s");
    media::FragmentSamples fs;
    REQUIRE(media::fragmentSamples(ByteView(vfrag.data(), vfrag.size()), cfg[0].track.track_id, fs));
    CHECK(fs.track_id == cfg[0].track.track_id);
    CHECK(fs.tfdt);
    CHECK(fs.samples.size() + fs.truncated > 0);
    CHECK(fs.duration > 0);
    if (!fs.samples.empty()) {
        CHECK(fs.samples[0].dts == *fs.tfdt);
        CHECK(fs.samples[0].size > 0);
        // length-prefixed NAL units: the first sample starts with a 4-byte length that fits in the sample
        const media::SampleRef& s0 = fs.samples[0];
        const std::uint32_t nal_len = (std::uint32_t(vfrag[s0.offset]) << 24) | (std::uint32_t(vfrag[s0.offset + 1]) << 16) |
                                      (std::uint32_t(vfrag[s0.offset + 2]) << 8) | vfrag[s0.offset + 3];
        CHECK(nal_len + 4 <= s0.size);
        for (std::size_t i = 1; i < fs.samples.size(); ++i) CHECK(fs.samples[i].dts == fs.samples[i - 1].dts + fs.samples[i - 1].duration);
    }
    // WNUV pid 200: AC-4 -> dac4 extradata
    const auto ainit = load("bwi-200-init.mp4");
    REQUIRE(media::decoderConfigs(ByteView(ainit.data(), ainit.size()), cfg));
    REQUIRE(cfg.size() == 1);
    CHECK(cfg[0].track.kind() == "audio");
    CHECK(cfg[0].original_entry == "ac-4");
    CHECK(cfg[0].extradata_box == "dac4");
    CHECK(!cfg[0].extradata.empty());
    // WNUV pid 300: stpp captions with a namespace
    const auto tinit = load("bwi-300-init.mp4");
    REQUIRE(media::decoderConfigs(ByteView(tinit.data(), tinit.size()), cfg));
    REQUIRE(cfg.size() == 1);
    CHECK(cfg[0].track.kind() == "subtitles");
    CHECK(cfg[0].original_entry == "stpp");
    CHECK(cfg[0].xml_namespace == "subtitle");   // on air the namespace field carries "subtitle", the role sits in schema_location
    // ROUTE WBAL video init: encrypted (encv) with the original format and the hvcC still readable
    const auto einit = load("route-bwi-wbal-video-init.mp4");
    REQUIRE(media::decoderConfigs(ByteView(einit.data(), einit.size()), cfg));
    REQUIRE(cfg.size() == 1);
    CHECK(cfg[0].track.encrypted);
    CHECK((cfg[0].original_entry == "hvc1" || cfg[0].original_entry == "hev1"));
    CHECK(cfg[0].extradata_box == "hvcC");
    // ROUTE WMAR audio init: AAC -> AudioSpecificConfig from the esds
    const auto aac = load("route-bwi-wmar-a0-init.mp4");
    REQUIRE(media::decoderConfigs(ByteView(aac.data(), aac.size()), cfg));
    REQUIRE(cfg.size() == 1);
    CHECK(cfg[0].track.kind() == "audio");
    if (cfg[0].original_entry == "mp4a") {
        CHECK(cfg[0].extradata_box == "esds");
        REQUIRE(cfg[0].extradata.size() >= 2);
        CHECK((cfg[0].extradata[0] >> 3) == 2);   // AudioObjectType 2 = AAC LC
    } else {
        CHECK(cfg[0].original_entry == "ac-4");
        CHECK(cfg[0].extradata_box == "dac4");
    }
}
