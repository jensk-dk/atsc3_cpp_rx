// Randomized robustness pass over the ISOBMFF helpers (spec 5.10.3, 5.17.3): the on-air fixtures in tests/vectors/mmt
// are mutated with a fixed-seed PRNG (bit flips, truncation, size-field corruption, box duplication, field overwrite,
// byte smears) and fed to parseInitSegment, cleanInitSegment, parseMoof, buildFragment and codecsFromSampleEntry.
// The only expectations: every call returns (no crash, no sanitizer report, no exception) and whatever it produces
// re-parses consistently. Defects this pass found are pinned by deterministic cases at the end; those CHECKs are
// marked "known defect" and run the offending call in a forked child so a crash cannot take the test binary down.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "atsc3/media/isobmff.hpp"
#include "mp4_builders.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define ATSC3_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ATSC3_TEST_ASAN 1
#endif
#endif
#ifndef ATSC3_TEST_ASAN
#define ATSC3_TEST_ASAN 0
#endif

using namespace atsc3;
using atsc3::test::mp4::Bytes;
using atsc3::test::mp4::loadVector;
using atsc3::test::mp4::view;

namespace {

constexpr std::uint64_t kSeed = 0x5EED00A7u;

/// Deterministic PRNG (mt19937_64 is specified by the standard, so sequences are identical on every platform).
struct Rng {
    std::mt19937_64 g;
    explicit Rng(std::uint64_t seed) : g(seed) {}
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(g() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
    std::uint32_t u32() { return static_cast<std::uint32_t>(g()); }
    std::uint64_t u64() { return g(); }
};

std::uint32_t be32(ByteView v, std::size_t o) noexcept {
    if (o + 4 > v.size()) return 0;
    return (static_cast<std::uint32_t>(v[o]) << 24) | (static_cast<std::uint32_t>(v[o + 1]) << 16) | (static_cast<std::uint32_t>(v[o + 2]) << 8) |
           v[o + 3];
}
void w32(Bytes& b, std::size_t at, std::uint32_t v) {
    if (at + 4 > b.size()) return;
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}

const char* const kInitFixtures[] = {"unisoft-20-init.mp4",
                                     "unisoft-21-init.mp4",
                                     "bwi-100-init.mp4",
                                     "bwi-200-init.mp4",
                                     "bwi-300-init.mp4",
                                     "route-uni-usc02-video-init.mp4v",
                                     "route-uni-usc02-audio-init.mp4a",
                                     "route-bwi-wmar-video-init.mp4",
                                     "route-bwi-wmar-a0-init.mp4",
                                     "route-bwi-wmar-a1-init.mp4",
                                     "route-bwi-wmar-d4-init.mp4",
                                     "route-bwi-wbal-video-init.mp4",
                                     "route-bwi-wbal-a0-init.mp4"};
struct MoofFixture {
    const char* init;
    const char* moof;
};
const MoofFixture kMoofFixtures[] = {{"unisoft-20-init.mp4", "unisoft-20-moof.m4s"},
                                     {"unisoft-21-init.mp4", "unisoft-21-moof.m4s"},
                                     {"bwi-100-init.mp4", "bwi-100-moof.m4s"},
                                     {"bwi-200-init.mp4", "bwi-200-moof.m4s"},
                                     {"bwi-300-init.mp4", "bwi-300-moof.m4s"}};

// ---- box map of a file (absolute offsets) used to aim the mutations ---------------------------------------------

struct BoxRef {
    std::size_t offset = 0, size = 0, header = 0;
    std::string type;
};

bool isContainer(const std::string& t) {
    static const char* const kContainers[] = {"moov", "trak", "mdia", "minf", "stbl", "mvex", "moof", "traf", "dinf", "edts", "udta", "sinf", "schi", "wave"};
    for (const char* c : kContainers)
        if (t == c) return true;
    return false;
}
bool isVisualEntry(const std::string& t) { return t == "hev1" || t == "hvc1" || t == "encv" || t == "avc1" || t == "avc3"; }
bool isAudioEntry(const std::string& t) { return t == "mp4a" || t == "ac-4" || t == "enca" || t == "ec-3"; }

void collectBoxes(ByteView data, std::size_t base, std::vector<BoxRef>& out, int depth) {
    if (depth > 8) return;
    for (const media::Box& b : media::boxes(data)) {
        out.push_back({base + b.offset, b.size, b.header, b.type});
        const std::size_t payload_base = base + b.offset + b.header;
        if (isContainer(b.type)) collectBoxes(b.payload, payload_base, out, depth + 1);
        else if (b.type == "stsd" && b.payload.size() >= 8) collectBoxes(b.payload.subview(8), payload_base + 8, out, depth + 1);
        else if (isVisualEntry(b.type) && b.payload.size() >= 78) collectBoxes(b.payload.subview(78), payload_base + 78, out, depth + 1);
        else if (isAudioEntry(b.type) && b.payload.size() >= 28) collectBoxes(b.payload.subview(28), payload_base + 28, out, depth + 1);
    }
}
std::vector<BoxRef> boxMap(const Bytes& b) {
    std::vector<BoxRef> out;
    collectBoxes(view(b), 0, out, 0);
    return out;
}

// ---- mutators ------------------------------------------------------------------------------------------------------

Bytes flipBits(Bytes b, Rng& r, unsigned n) {
    if (b.empty()) return b;
    for (unsigned i = 0; i < n; ++i) b[r.below(b.size())] ^= static_cast<std::uint8_t>(1u << r.below(8));
    return b;
}
Bytes truncateAt(Bytes b, Rng& r) {
    b.resize(r.below(b.size() + 1));
    return b;
}
Bytes corruptSize(Bytes b, Rng& r) {
    const std::vector<BoxRef> refs = boxMap(b);
    if (refs.empty()) return flipBits(std::move(b), r, 1);
    const BoxRef& ref = refs[r.below(refs.size())];
    const std::uint32_t old = be32(view(b), ref.offset);
    std::uint32_t v = 0;
    switch (r.below(9)) {
        case 0: v = r.u32(); break;
        case 1: v = 0; break;
        case 2: v = 1; break;
        case 3: v = 7; break;
        case 4: v = old + 1; break;
        case 5: v = old - 1; break;
        case 6: v = old + static_cast<std::uint32_t>(r.below(64)); break;
        case 7: v = 0xFFFFFFFFu; break;
        default: v = static_cast<std::uint32_t>(b.size()) + 1; break;
    }
    w32(b, ref.offset, v);
    return b;
}
Bytes duplicateBox(Bytes b, Rng& r) {
    const std::vector<BoxRef> refs = boxMap(b);
    if (refs.empty()) return flipBits(std::move(b), r, 1);
    const BoxRef& ref = refs[r.below(refs.size())];
    if (ref.offset + ref.size > b.size() || ref.size == 0) return b;
    const Bytes copy(b.begin() + static_cast<std::ptrdiff_t>(ref.offset), b.begin() + static_cast<std::ptrdiff_t>(ref.offset + ref.size));
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(ref.offset + ref.size), copy.begin(), copy.end());
    return b;   // parent sizes are left stale on purpose: inconsistent containers are part of the input space
}
Bytes overwriteField(Bytes b, Rng& r) {
    const std::vector<BoxRef> refs = boxMap(b);
    if (refs.empty()) return flipBits(std::move(b), r, 1);
    const BoxRef& ref = refs[r.below(refs.size())];
    const std::size_t payload = ref.offset + ref.header;
    if (ref.size <= ref.header + 4) return flipBits(std::move(b), r, 1);
    const std::size_t fields = (ref.size - ref.header) / 4;
    const std::size_t at = payload + 4 * r.below(fields);
    const std::uint32_t old = be32(view(b), at);
    std::uint32_t v = 0;
    switch (r.below(6)) {
        case 0: v = r.u32(); break;
        case 1: v = 0; break;
        case 2: v = 0xFFFFFFFFu; break;
        case 3: v = old | 0x80000000u; break;
        case 4: v = old ^ (1u << r.below(32)); break;
        default: v = static_cast<std::uint32_t>(r.below(0x20000)); break;
    }
    w32(b, at, v);
    return b;
}
Bytes smear(Bytes b, Rng& r) {
    if (b.empty()) return b;
    const std::size_t at = r.below(b.size());
    const std::size_t n = std::min(b.size() - at, 1 + r.below(16));
    for (std::size_t i = 0; i < n; ++i) b[at + i] = static_cast<std::uint8_t>(r.below(256));
    return b;
}
Bytes mutate(const Bytes& src, Rng& r, std::string& desc) {
    Bytes b = src;
    const std::size_t k = 1 + r.below(3);
    desc.clear();
    for (std::size_t i = 0; i < k; ++i) {
        switch (r.below(6)) {
            case 0: b = flipBits(std::move(b), r, 1 + static_cast<unsigned>(r.below(8))); desc += "flip "; break;
            case 1: b = truncateAt(std::move(b), r); desc += "trunc "; break;
            case 2: b = corruptSize(std::move(b), r); desc += "size "; break;
            case 3: b = duplicateBox(std::move(b), r); desc += "dup "; break;
            case 4: b = overwriteField(std::move(b), r); desc += "field "; break;
            default: b = smear(std::move(b), r); desc += "smear "; break;
        }
    }
    return b;
}

// ---- guards for known unbounded-allocation defects (see the "known defects" case) ------------------------------------

/// parseTrun takes sample_count verbatim when the trun has no per-sample fields (tr_flags & 0xF00 == 0) and reserves
/// and fills that many TrunSamples: 2^31 samples from one flipped bit. Mirrors the parser's own walk (moof/traf/trun).
bool trunCountHazard(ByteView du) {
    const auto moof = media::findBox(du, "moof");
    if (!moof) return false;
    for (const media::Box& t : media::boxes(moof->payload)) {
        if (t.type != "traf") continue;
        for (const media::Box& g : media::boxes(t.payload)) {
            if (g.type != "trun") continue;
            if ((media::fullBoxFlags(g) & 0xF00u) == 0 && be32(g.payload, 4) > 65536u) return true;
        }
    }
    return false;
}
/// buildFragment zero-fills every trun sample beyond the given ones with the (unvalidated) trun size: 4 GiB per sample.
bool zeroFillHazard(const media::TrackFragmentInfo& traf, std::size_t given) {
    std::uint64_t fill = 0;
    for (std::size_t i = given; i < traf.samples.size(); ++i) fill += traf.samples[i].size;
    return fill > (4u << 20);
}

// ---- what the on-air fixtures contain, for the oracle --------------------------------------------------------------

struct MoofOracle {
    Bytes init_du, moof_du;
    std::uint32_t media_track = 0;
    std::optional<std::uint32_t> hint_track;
    std::vector<Bytes> samples;   // one per trun sample, with the trun size
};
MoofOracle loadMoof(const MoofFixture& f) {
    MoofOracle o;
    o.init_du = loadVector(f.init);
    o.moof_du = loadVector(f.moof);
    media::CleanInitResult init;
    REQUIRE(media::cleanInitSegment(view(o.init_du), media::CleanInitOptions{}, init));
    REQUIRE(init.info.mediaTrack());
    o.media_track = init.info.mediaTrack()->track_id;
    o.hint_track = init.hint_track_id;
    media::MoofInfo m;
    REQUIRE(media::parseMoof(view(o.moof_du), m));
    const media::TrackFragmentInfo* t = m.traf(o.media_track);
    REQUIRE(t);
    for (std::size_t i = 0; i < t->samples.size(); ++i) o.samples.emplace_back(t->samples[i].size, static_cast<std::uint8_t>(i + 1));
    return o;
}

/// The stsd entries of every trak of an init segment (walks like the parser does).
std::vector<media::Box> sampleEntries(ByteView data) {
    std::vector<media::Box> out;
    const auto moov = media::findBox(data, "moov");
    if (!moov) return out;
    for (const media::Box& trak : media::boxes(moov->payload)) {
        if (trak.type != "trak") continue;
        const auto stsd = media::findPath(trak.payload, {"mdia", "minf", "stbl", "stsd"});
        if (!stsd || stsd->payload.size() < 8) continue;
        for (media::Box& e : media::boxes(stsd->payload.subview(8))) out.push_back(std::move(e));
    }
    return out;
}

// ---- forked-child harness for the known crashers ---------------------------------------------------------------------

struct ChildResult {
    bool clean = false;
    std::string what;
};
/// Runs `fn` in a forked child: exit 0 when it returns, 3 when it throws; the parent reports signals and stalls.
/// Without ASan the child's address space is capped so a runaway allocation fails fast instead of swapping the host.
template <class F>
ChildResult inChild(F&& fn, unsigned timeout_ms) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    ChildResult res;
    if (pid < 0) {
        res.what = "fork failed";
        return res;
    }
    if (pid == 0) {
#if !ATSC3_TEST_ASAN
        rlimit rl{};
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(3u) << 30;   // 3 GiB
        setrlimit(RLIMIT_AS, &rl);
#endif
        int code = 0;
        try {
            fn();
        } catch (...) {
            code = 3;
        }
        _exit(code);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (true) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            res.what = "stalled (killed after " + std::to_string(timeout_ms) + " ms)";
            return res;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        res.clean = code == 0;
        res.what = code == 0 ? "returned" : code == 3 ? "threw an exception (std::bad_alloc)" : "exit " + std::to_string(code) + " (sanitizer abort)";
    } else if (WIFSIGNALED(status)) {
        res.what = "killed by signal " + std::to_string(WTERMSIG(status));
    }
    return res;
}

/// Absolute offset of the sample_count field of the trun of the `traf_index`-th traf, 0 when absent.
std::size_t trunCountOffset(const Bytes& du, std::size_t traf_index) {
    const auto moof = media::findBox(view(du), "moof");
    if (!moof) return 0;
    std::size_t ti = 0;
    for (const media::Box& t : media::boxes(moof->payload)) {
        if (t.type != "traf" || ti++ != traf_index) continue;
        for (const media::Box& g : media::boxes(t.payload))
            if (g.type == "trun") return moof->offset + moof->header + t.offset + t.header + g.offset + g.header + 4;
    }
    return 0;
}
/// Sets every sample size of the media trun (per-sample size field present) to `size`, starting at sample `from` (0-based).
bool setTrunSizes(Bytes& du, std::size_t traf_index, std::size_t from, std::uint32_t size) {
    const auto moof = media::findBox(view(du), "moof");
    if (!moof) return false;
    std::size_t ti = 0;
    for (const media::Box& t : media::boxes(moof->payload)) {
        if (t.type != "traf" || ti++ != traf_index) continue;
        for (const media::Box& g : media::boxes(t.payload)) {
            if (g.type != "trun") continue;
            const std::uint32_t flags = media::fullBoxFlags(g);
            if (!(flags & 0x200u)) return false;
            const std::uint32_t count = be32(g.payload, 4);
            std::size_t off = 8 + (flags & 0x1u ? 4u : 0u) + (flags & 0x4u ? 4u : 0u);
            const std::size_t per = 4 * ((flags & 0x100u ? 1u : 0u) + 1u + (flags & 0x400u ? 1u : 0u) + (flags & 0x800u ? 1u : 0u));
            const std::size_t size_at = (flags & 0x100u ? 4u : 0u);
            const std::size_t base = moof->offset + moof->header + t.offset + t.header + g.offset + g.header;
            for (std::size_t i = from; i < count; ++i) w32(du, base + off + i * per + size_at, size);
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("ISOBMFF robustness: mutated on-air init segments never crash and the cleaned output re-parses", "[media][robustness]") {
    Rng rng(kSeed);
    std::size_t iterations = 0, cleaned = 0, parsed = 0, reparse_failures = 0, idempotence_failures = 0;
    std::string first_failure;
    for (const char* name : kInitFixtures) {
        const Bytes src = loadVector(name);
        REQUIRE_FALSE(src.empty());
        for (int it = 0; it < 110; ++it, ++iterations) {
            std::string desc;
            const Bytes m = mutate(src, rng, desc);
            media::InitSegmentInfo info;
            std::string err;
            const bool ok = media::parseInitSegment(view(m), info, &err);
            if (ok) {
                ++parsed;
                for (const media::TrackInfo& t : info.tracks) {
                    (void)t.kind();
                    (void)t.mimeType();
                }
                (void)info.mediaTrack();
            }
            for (const media::Box& e : sampleEntries(view(m))) {
                std::string warn;
                (void)media::codecsFromSampleEntry(e, &warn);
            }
            media::CleanInitResult res;
            if (!media::cleanInitSegment(view(m), media::CleanInitOptions{}, res, &err)) {
                CHECK(res.bytes.empty());
                continue;
            }
            ++cleaned;
            // the output must re-parse, keep at least one (non-hint) track - the single media track when the input had
            // one - and contain no hint track or mmpu box
            media::InitSegmentInfo again;
            const bool unique_media = ok && info.mediaTrack() != nullptr;
            if (!media::parseInitSegment(view(res.bytes), again, &err) || again.has_mmpu || again.tracks.empty() || (unique_media && !again.mediaTrack())) {
                if (++reparse_failures == 1) first_failure = std::string(name) + " it=" + std::to_string(it) + " [" + desc + "] " + err;
            } else {
                for (const media::TrackInfo& t : again.tracks) CHECK_FALSE(t.isHint());
                // cleaning is idempotent
                media::CleanInitResult twice;
                if (!media::cleanInitSegment(view(res.bytes), media::CleanInitOptions{}, twice, &err) || twice.bytes != res.bytes) {
                    if (++idempotence_failures == 1) first_failure = std::string(name) + " it=" + std::to_string(it) + " [" + desc + "] not idempotent";
                }
            }
            // every option combination must also return
            media::CleanInitOptions o;
            o.rewrite_ftyp = rng.chance(50);
            o.drop_hint_tracks = rng.chance(50);
            o.keep_only_ftyp_moov = rng.chance(50);
            media::CleanInitResult any;
            (void)media::cleanInitSegment(view(m), o, any, &err);
        }
    }
    INFO("first failure: " << first_failure);
    CHECK(reparse_failures == 0);
    CHECK(idempotence_failures == 0);
    CHECK(iterations == 110 * (sizeof kInitFixtures / sizeof kInitFixtures[0]));
    CHECK(parsed > iterations / 2);   // most mutations keep a moov
    CHECK(cleaned > iterations / 2);
}

TEST_CASE("ISOBMFF robustness: mutated on-air movie fragments never crash and built fragments re-parse", "[media][robustness]") {
    Rng rng(kSeed + 1);
    std::size_t iterations = 0, built = 0, guarded = 0, reparse_failures = 0, size_failures = 0, offset_failures = 0, offset_no_traf = 0, offset_unset = 0;
    std::string first_failure;
    for (const MoofFixture& f : kMoofFixtures) {
        const MoofOracle o = loadMoof(f);
        std::vector<ByteView> given;
        for (const Bytes& s : o.samples) given.push_back(view(s));
        for (int it = 0; it < 300; ++it, ++iterations) {
            std::string desc;
            const Bytes m = mutate(o.moof_du, rng, desc);
            if (trunCountHazard(view(m))) {   // known defect (see the last case): skip instead of allocating gigabytes
                ++guarded;
                continue;
            }
            media::MoofInfo in;
            std::string err;
            if (!media::parseMoof(view(m), in, &err)) continue;
            for (const media::TrackFragmentInfo& t : in.trafs) (void)in.traf(t.track_id);
            media::BuildFragmentOptions opts;
            if (o.hint_track) opts.drop_track_ids.insert(*o.hint_track);
            if (rng.chance(80)) opts.media_track_id = o.media_track;
            if (rng.chance(30)) opts.tfdt = rng.chance(50) ? rng.u64() : static_cast<std::uint64_t>(rng.u32());
            opts.prepend_styp = rng.chance(85);
            // sometimes fewer samples than the trun has (zero-fill), sometimes more (ignored)
            std::vector<ByteView> samples = given;
            if (rng.chance(20)) samples.resize(rng.below(samples.size() + 1));
            else if (rng.chance(10)) samples.push_back(view(o.init_du));
            // the traf buildFragment will treat as the media traf, and the mdat it will write
            const media::TrackFragmentInfo* chosen = opts.media_track_id ? in.traf(*opts.media_track_id) : nullptr;
            if (!opts.media_track_id) {
                for (const media::TrackFragmentInfo& t : in.trafs) {
                    if (opts.drop_track_ids.count(t.track_id)) continue;
                    chosen = &t;
                    break;
                }
            }
            if (!chosen) {
                media::BuildFragmentResult none;
                CHECK_FALSE(media::buildFragment(view(m), samples, opts, none, &err));
                continue;
            }
            std::uint64_t expected = 0;
            for (std::size_t i = 0; i < chosen->samples.size(); ++i) expected += i < samples.size() ? samples[i].size() : chosen->samples[i].size;
            if (zeroFillHazard(*chosen, samples.size()) || expected > (16u << 20)) {   // known defect: skip the multi-GiB zero-fill
                ++guarded;
                continue;
            }
            media::BuildFragmentResult res;
            if (!media::buildFragment(view(m), samples, opts, res, &err)) {
                CHECK(res.bytes.empty());
                continue;
            }
            ++built;
            media::MoofInfo out;
            const std::size_t styp = opts.prepend_styp ? 24 : 0;
            if (!media::parseMoof(view(res.bytes), out, &err) || out.moof_offset != styp || out.mdat_header_size == 0) {
                if (++reparse_failures == 1) first_failure = std::string(f.moof) + " it=" + std::to_string(it) + " [" + desc + "] " + err;
                continue;
            }
            if (res.mdat_payload != expected || res.bytes.size() != styp + res.moof_size + out.mdat_header_size + res.mdat_payload ||
                out.mdat_size != out.mdat_header_size + res.mdat_payload || out.moof_size != res.moof_size) {
                if (++size_failures == 1) first_failure = std::string(f.moof) + " it=" + std::to_string(it) + " [" + desc + "] sizes";
            }
            // a well-formed input (no parser warning) with a trun in its media traf yields a media traf whose first
            // trun points at the mdat payload
            if (in.warnings.empty() && chosen->trun_count >= 1) {
                const media::TrackFragmentInfo* t = out.traf(chosen->track_id);
                if (!t) ++offset_no_traf;
                else if (t->trun_count == 0 || !t->data_offset) ++offset_unset;
                else if (*t->data_offset != static_cast<std::int32_t>(res.moof_size + out.mdat_header_size)) {
                    if (++offset_failures == 1) first_failure = std::string(f.moof) + " it=" + std::to_string(it) + " [" + desc + "] data_offset " + std::to_string(*t->data_offset) + " != " + std::to_string(res.moof_size + out.mdat_header_size);
                }
                if (o.hint_track && *o.hint_track != chosen->track_id) CHECK(out.traf(*o.hint_track) == nullptr);
            }
        }
    }
    INFO("first failure: " << first_failure);
    CHECK(reparse_failures == 0);
    CHECK(size_failures == 0);
    CHECK(offset_failures == 0);
    INFO("media traf missing in the output: " << offset_no_traf << ", trun or data_offset missing: " << offset_unset);
    CHECK(offset_no_traf == 0);
    CHECK(offset_unset == 0);
    CHECK(iterations == 300 * (sizeof kMoofFixtures / sizeof kMoofFixtures[0]));
    CHECK(built > iterations / 3);
    CHECK(guarded < iterations / 10);
}

TEST_CASE("ISOBMFF robustness: random bytes and random box shells never crash the parsers", "[media][robustness]") {
    Rng rng(kSeed + 2);
    static const char* const kTypes[] = {"ftyp", "moov", "moof", "mdat", "trak", "mdia", "hdlr", "stsd", "hev1", "mp4a", "ac-4", "stpp", "encv", "enca",
                                         "esds", "hvcC", "dac4", "avcC", "trun", "tfhd", "tfdt", "mfhd", "traf", "uuid", "mmpu", "trex", "mvex", "mvhd",
                                         "tkhd", "mdhd", "sinf", "frma", "wave", "Opus", "fLaC", "mhm1", "vp09", "av01", "tx3g", "wvtt", "ec-3", "\0\0\0\0"};
    for (int it = 0; it < 400; ++it) {
        Bytes b(rng.below(600));
        for (std::uint8_t& x : b) x = static_cast<std::uint8_t>(rng.below(256));
        // sprinkle plausible headers so the walkers get past the first box
        for (std::size_t k = 0, n = rng.below(6); k < n && b.size() >= 8; ++k) {
            const std::size_t at = rng.below(b.size() - 7);
            const std::uint32_t size = rng.chance(50) ? static_cast<std::uint32_t>(rng.below(b.size() + 16)) : rng.u32();
            w32(b, at, rng.chance(10) ? (rng.chance(50) ? 0u : 1u) : size);
            std::memcpy(b.data() + at + 4, kTypes[rng.below(sizeof kTypes / sizeof kTypes[0])], 4);
        }
        (void)media::boxes(view(b));
        (void)media::findPath(view(b), {"moov", "trak", "mdia", "minf", "stbl", "stsd"});
        media::InitSegmentInfo info;
        (void)media::parseInitSegment(view(b), info);
        if (!trunCountHazard(view(b))) {   // known defect guard, see below
            media::MoofInfo m;
            (void)media::parseMoof(view(b), m);
            media::CleanInitResult c;
            (void)media::cleanInitSegment(view(b), media::CleanInitOptions{}, c);
            bool hazard = false;
            for (const media::TrackFragmentInfo& t : m.trafs) hazard = hazard || zeroFillHazard(t, 0);
            if (!hazard) {
                media::BuildFragmentResult r;
                (void)media::buildFragment(view(b), {}, media::BuildFragmentOptions{}, r);
            }
        }
        // a sample entry shell around random bytes, every known 4CC
        media::Box e;
        e.type = kTypes[rng.below(sizeof kTypes / sizeof kTypes[0])];
        e.header = 8;
        e.size = b.size();
        e.payload = view(b);
        std::string warn;
        (void)media::codecsFromSampleEntry(e, &warn);
        (void)media::sameCodecs(warn, e.type);
    }
    // the real sample entries with their codec config boxes damaged
    for (const char* name : kInitFixtures) {
        const Bytes src = loadVector(name);
        REQUIRE_FALSE(sampleEntries(view(src)).empty());
        // mutate the whole init and call the codecs deriver on every entry the mutated stsd exposes
        for (int it = 0; it < 60; ++it) {
            std::string desc;
            const Bytes m = mutate(src, rng, desc);
            for (const media::Box& e : sampleEntries(view(m))) {
                std::string warn, warn2;
                const std::string c = media::codecsFromSampleEntry(e, &warn);
                CHECK((c.empty() || warn.empty()));
                media::Box copy = e;
                copy.payload = e.payload.subview(0, rng.below(e.payload.size() + 1));   // truncated entry payload
                (void)media::codecsFromSampleEntry(copy, &warn2);
            }
        }
    }
    SUCCEED("parsers survived random input");
}

TEST_CASE("ISOBMFF robustness: known defects (single-bit corruptions that allocate without bound)", "[media][robustness]") {
    // Each call runs in a forked child so the current behaviour (std::bad_alloc escaping a documented no-throw
    // function, a sanitizer OOM abort, or a multi-second loop) cannot take the test binary down.
    SECTION("parseMoof: bit 31 of the hint trun sample_count (tr_flags 0x000001, no per-sample fields)") {
        Bytes du = loadVector("unisoft-20-moof.m4s");
        const std::size_t at = trunCountOffset(du, 1);
        REQUIRE(at != 0);
        REQUIRE(be32(view(du), at) == 60u);
        du[at] ^= 0x80;   // sample_count 60 -> 2147483708
        const ChildResult r = inChild(
            [&] {
                media::MoofInfo m;
                (void)media::parseMoof(view(du), m);
            },
            700);
        INFO("parseMoof on the flipped on-air DU: " << r.what);
        // known defect: src/media/isobmff.cpp parseTrun reserves/pushes sample_count TrunSamples when the trun has no
        // per-sample fields (bad_alloc in a Debug build, a 3.5 GB / 10 s parse under ASan for bit 27)
        CHECK(r.clean);
    }
    SECTION("buildFragment: trun sample sizes of 0xFFFFFFFF for the samples that are zero-filled") {
        Bytes du = loadVector("bwi-100-moof.m4s");
        REQUIRE(setTrunSizes(du, 0, 1, 0xFFFFFFFFu));
        media::MoofInfo m;
        REQUIRE(media::parseMoof(view(du), m));
        REQUIRE(m.trafs[0].samples.size() == 30);
        REQUIRE(m.trafs[0].samples[1].size == 0xFFFFFFFFu);
        const Bytes sample1(100, 1);
        const ChildResult r = inChild(
            [&] {
                media::BuildFragmentOptions o;
                o.drop_track_ids.insert(2);
                o.media_track_id = 1;
                media::BuildFragmentResult res;
                (void)media::buildFragment(view(du), {view(sample1)}, o, res);
            },
            700);
        INFO("buildFragment with 29 missing 4 GiB samples: " << r.what);
        // known defect: src/media/isobmff.cpp buildFragment reserves 8 + sum(trun sizes) and zero-fills every missing
        // sample with its unvalidated trun size (124 GiB here: bad_alloc / ASan out-of-memory abort)
        CHECK(r.clean);
    }
}
