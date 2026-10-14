// Randomized robustness pass over the MPU assembler (spec 5.10.3): fixed-seed streams of MMTP MPU-mode packets built
// around the on-air MPU metadata / movie-fragment DUs in tests/vectors/mmt, with random loss, duplication, reordering,
// random fragment counters and fragment types, MPU sequence jumps and wraps near 2^32, aggregated DUs with bad lengths
// and MMTHSample prefixes with wrong sample numbers. Invariants checked after every packet: bounded pending bytes and
// MPUs, emitted fragments in increasing MPU order, every emitted fragment re-parses with data_offset == moof' + 8 and an
// mdat holding exactly the trun sample sizes. The packet helpers are copies of tests/unit/mmtp/mpu_test.cpp. Defects
// this pass found are pinned by deterministic cases at the end (CHECKs marked "known defect").
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../media/mp4_builders.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/media/isobmff.hpp"
#include "atsc3/mmtp/mpu.hpp"

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
using namespace atsc3::test;
using namespace atsc3::test::mp4;

namespace {

constexpr std::uint64_t kSeed = 0x5EED0B10u;

struct Rng {
    std::mt19937_64 g;
    explicit Rng(std::uint64_t seed) : g(seed) {}
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(g() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
    std::uint32_t u32() { return static_cast<std::uint32_t>(g()); }
    std::uint64_t u64() { return g(); }
};

// ---- packet helpers (copies of tests/unit/mmtp/mpu_test.cpp) -------------------------------------------------------

/// One MMTP version-01 MPU-mode packet (no packet_counter, QoS word present) carrying `data` after the MPU payload header.
Bytes mpuPacket(std::uint16_t pid, std::uint32_t psn, std::uint8_t ft, std::uint8_t fi, std::uint8_t counter, std::uint32_t mpu_seq, const Bytes& data,
                bool timed = true, bool aggregation = false) {
    Bytes p = cat({u8(0x40), u8(0x00), u16(pid), u32(0x12345678), u32(psn), u16(0x8000)});
    const std::uint16_t length = static_cast<std::uint16_t>(6 + data.size());
    const std::uint8_t flags = static_cast<std::uint8_t>((ft << 4) | (timed ? 0x08 : 0) | ((fi & 3) << 1) | (aggregation ? 1 : 0));
    Bytes payload = cat({u16(length), u8(flags), u8(counter), u32(mpu_seq), data});
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

/// Packets for one DU: complete when it fits `chunk` bytes, else f_i 1/2/3 fragments with the frag_counter counting
/// down and the 14-byte MFU DU header repeated in every fragment, as both encoders do.
std::vector<Bytes> duPackets(std::uint16_t pid, std::uint32_t& psn, std::uint8_t ft, std::uint32_t mpu_seq, const Bytes& data, std::size_t chunk,
                             int skip_fragment = -1, bool timed = true) {
    std::vector<Bytes> out;
    if (data.size() <= chunk) {
        out.push_back(mpuPacket(pid, psn++, ft, 0, 0, mpu_seq, data, timed));
        return out;
    }
    const Bytes head(data.begin(), data.begin() + 14);
    std::vector<Bytes> parts;
    std::size_t off = 0;
    bool first = true;
    while (off < data.size()) {
        const std::size_t n = std::min(chunk, data.size() - off);
        Bytes part;
        if (!first) part = head;
        part.insert(part.end(), data.begin() + static_cast<std::ptrdiff_t>(off), data.begin() + static_cast<std::ptrdiff_t>(off + n));
        parts.push_back(std::move(part));
        off += n;
        first = false;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::uint8_t fi = i == 0 ? 1 : (i + 1 == parts.size() ? 3 : 2);
        const std::uint8_t counter = static_cast<std::uint8_t>(parts.size() - 1 - i);
        const std::uint32_t this_psn = psn++;
        if (static_cast<int>(i) == skip_fragment) continue;
        out.push_back(mpuPacket(pid, this_psn, ft, fi, counter, mpu_seq, parts[i], timed));
    }
    return out;
}

/// Sample content: deterministic bytes so the mdat can be checked.
Bytes sampleBytes(std::uint32_t number, std::uint32_t size) {
    Bytes b(size);
    for (std::uint32_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>((number * 31u + i) & 0xFF);
    return b;
}

/// MFU DU for timed media: 14-byte DU header + 34-byte MMTHSample + the sample. `prefix_number` is the samplenumber
/// written into the MMTHSample (the DU header always carries `sample_number`).
Bytes mfuDu(std::uint32_t sample_number, std::uint32_t offset, const Bytes& sample, std::uint8_t track_id, bool with_hint = true,
            std::uint32_t prefix_number = 0) {
    Bytes du = cat({u32(1), u32(sample_number), u32(0), u8(1), u8(0)});   // offset is 0 on air
    if (with_hint) {
        const std::uint32_t pn = prefix_number ? prefix_number : sample_number;
        Bytes hint = cat({u32(sample_number - 1), u8(track_id), u32(1), u32(pn), u8(1), u8(static_cast<std::uint8_t>(sample_number)), u32(offset),
                          u32(static_cast<std::uint32_t>(sample.size())), u32(11), fourcc("muli"), u8(0), u8(0), u8(0)});
        REQUIRE(hint.size() == 34);
        du.insert(du.end(), hint.begin(), hint.end());
    }
    du.insert(du.end(), sample.begin(), sample.end());
    return du;
}

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

struct Fixture {
    Bytes init_du, moof_du;
    media::MoofInfo moof;
    std::uint32_t media_track = 0;
    std::uint32_t timescale = 0;
    std::size_t sizes_base = 0, sizes_stride = 0;   // absolute offset of sample 1's size field in moof_du, bytes per sample
    Fixture(const std::string& init_name, const std::string& moof_name) : init_du(loadVector(init_name)), moof_du(loadVector(moof_name)) {
        media::InitSegmentInfo info;
        REQUIRE(media::parseInitSegment(view(init_du), info));
        REQUIRE(info.mediaTrack());
        media_track = info.mediaTrack()->track_id;
        timescale = info.mediaTrack()->timescale;
        REQUIRE(media::parseMoof(view(moof_du), moof));
        const media::TrackFragmentInfo* t = moof.traf(media_track);
        REQUIRE(t);
        // locate the per-sample size field of the media trun (every fixture carries sizes in the trun)
        const auto mf = media::findBox(view(moof_du), "moof");
        REQUIRE(mf);
        for (const media::Box& traf : media::boxes(mf->payload)) {
            if (traf.type != "traf") continue;
            const auto tfhd = media::findBox(traf.payload, "tfhd");
            if (!tfhd || be32(tfhd->payload, 4) != media_track) continue;
            for (const media::Box& g : media::boxes(traf.payload)) {
                if (g.type != "trun") continue;
                const std::uint32_t flags = media::fullBoxFlags(g);
                REQUIRE((flags & 0x200u) != 0);
                const std::size_t off = 8 + (flags & 0x1u ? 4u : 0u) + (flags & 0x4u ? 4u : 0u) + (flags & 0x100u ? 4u : 0u);
                sizes_stride = 4 * ((flags & 0x100u ? 1u : 0u) + 1u + (flags & 0x400u ? 1u : 0u) + (flags & 0x800u ? 1u : 0u));
                sizes_base = mf->offset + mf->header + traf.offset + traf.header + g.offset + g.header + off;
            }
        }
        REQUIRE(sizes_base != 0);
    }
    const media::TrackFragmentInfo& traf() const { return *moof.traf(media_track); }
    std::size_t sampleCount() const { return traf().samples.size(); }
    /// A copy of the moof DU with new per-sample sizes and baseMediaDecodeTime.
    Bytes moofWith(const std::vector<std::uint32_t>& sizes, std::uint64_t tfdt) const {
        Bytes b = moof_du;
        REQUIRE(sizes.size() == sampleCount());
        for (std::size_t i = 0; i < sizes.size(); ++i) w32(b, sizes_base + i * sizes_stride, sizes[i]);
        const std::size_t at = traf().tfdt_field_offset;
        REQUIRE(at != 0);
        REQUIRE(traf().tfdt_version == 1);
        const Bytes v = u64(tfdt);
        std::copy(v.begin(), v.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
        return b;
    }
    /// A copy of the init DU with another mpu_sequence_number in its mmpu box.
    Bytes initWithSeq(std::uint32_t seq) const {
        Bytes b = init_du;
        const auto mmpu = media::findBox(view(init_du), "mmpu");
        REQUIRE(mmpu);
        const Bytes v = u32(seq);
        std::copy(v.begin(), v.end(), b.begin() + static_cast<std::ptrdiff_t>(mmpu->offset + mmpu->header + 4));
        return b;
    }
};

struct Sink {
    std::vector<std::shared_ptr<const mmtp::MpuInitSegment>> inits;
    std::vector<std::shared_ptr<const mmtp::MpuFragment>> frags;
    mmtp::MpuAssemblerCallbacks callbacks() {
        mmtp::MpuAssemblerCallbacks cb;
        cb.onInit = [this](std::shared_ptr<const mmtp::MpuInitSegment> i) { inits.push_back(std::move(i)); };
        cb.onFragment = [this](std::shared_ptr<const mmtp::MpuFragment> f) { frags.push_back(std::move(f)); };
        return cb;
    }
};

/// Feeds packets through the real header parsers like the session does.
struct Feeder {
    mmtp::MpuAssembler& a;
    std::int64_t t_ns = 1'000'000;
    void packet(const Bytes& bytes) {
        mmtp::PacketHeader h;
        REQUIRE(mmtp::parsePacket(view(bytes), h) == mmtp::ParseStatus::Ok);
        mmtp::MpuPayloadHeader m;
        REQUIRE(mmtp::parseMpuPayload(h.payload, m) == mmtp::ParseStatus::Ok);
        a.onPacket(h, m, t_ns);
        t_ns += 1000;
    }
};

/// Structure check of an emitted fragment against what the moof of that MPU announced.
struct FragmentCheck {
    bool structure = true;    // styp moof mdat, one traf of the media track, data_offset == moof' + 8
    bool mdat_matches = true; // mdat payload == sum of the trun sample sizes
    std::string why;
};
FragmentCheck checkFragment(const mmtp::MpuFragment& f, std::uint32_t media_track, const std::vector<std::uint32_t>& sizes) {
    FragmentCheck c;
    const auto top = media::boxes(view(f.bytes));
    if (top.size() != 3 || top[0].type != "styp" || top[1].type != "moof" || top[2].type != "mdat") {
        c.structure = false;
        c.why = "top-level boxes";
        return c;
    }
    media::MoofInfo out;
    if (!media::parseMoof(ByteView(f.bytes.data() + top[1].offset, f.bytes.size() - top[1].offset), out) || out.trafs.size() != 1 ||
        out.trafs[0].track_id != media_track || !out.trafs[0].data_offset || *out.trafs[0].data_offset != static_cast<std::int32_t>(top[1].size + 8) ||
        out.mdat_size != top[2].size || out.trafs[0].samples.size() != sizes.size()) {
        c.structure = false;
        c.why = "moof' re-parse";
        return c;
    }
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        expected += sizes[i];
        if (out.trafs[0].samples[i].size != sizes[i]) c.structure = false;
    }
    if (!c.structure) c.why = "trun sizes changed";
    if (top[2].payload.size() != expected || f.sample_count != sizes.size() || f.damaged != (f.samples_missing != 0)) {
        c.mdat_matches = false;
        c.why = "mdat " + std::to_string(top[2].payload.size()) + " != " + std::to_string(expected);
    }
    return c;
}

// ---- the randomized stream ------------------------------------------------------------------------------------------

struct MpuPlan {
    std::uint32_t seq = 0;
    std::vector<std::uint32_t> sizes;
    bool perturbed = false;   // a sample whose bytes differ in size from the trun (wrong MMTHSample number, bad aggregation)
};

struct RunStats {
    std::size_t packets = 0, mpus = 0, perturbed_mpus = 0, pending_violations = 0, order_violations = 0, structure_failures = 0, mdat_clean_failures = 0,
                mdat_perturbed_failures = 0, flush_overflows = 0;
    std::string first_pending, first_order, first_structure, first_mdat_clean, first_mdat_perturbed;
};
void note(std::string& slot, const std::string& msg) {
    if (slot.empty()) slot = msg;
}

void runStream(std::uint64_t seed, const std::vector<Fixture>& fixtures, RunStats& rs) {
    Rng r(seed);
    const Fixture& fx = fixtures[r.below(fixtures.size())];
    const std::size_t n = fx.sampleCount();
    mmtp::MpuAssemblerConfig cfg;
    const std::size_t mpus_choices[] = {1, 2, 3, 5};
    const std::size_t bytes_choices[] = {4096, 65536, 1u << 20, 48u << 20};
    cfg.max_pending_mpus = mpus_choices[r.below(4)];   // 0 is a known defect (use-after-free), see the last case
    cfg.max_pending_bytes = bytes_choices[r.below(4)];
    cfg.drop_without_first_sample = r.chance(70);
    cfg.keep_raw_init = r.chance(20);
    const std::uint16_t pid = static_cast<std::uint16_t>(r.below(65536));
    const std::uint64_t anchor = (2208988800ull + 1678228503ull) << 32;
    Sink sink;
    mmtp::MpuAssembler a(pid, sink.callbacks(), cfg);
    if (r.chance(50)) a.setAnchor(anchor);
    Feeder feed{a};
    std::map<std::uint32_t, MpuPlan> plans;
    const unsigned p_loss = static_cast<unsigned>(r.below(4) * 10);   // 0, 10, 20, 30 %
    const unsigned p_dup = static_cast<unsigned>(r.below(3) * 4);
    const unsigned p_reorder = static_cast<unsigned>(r.below(3) * 4);
    const std::size_t chunk_choices[] = {200, 500, 1300};
    const std::size_t chunk = chunk_choices[r.below(3)];
    const std::size_t kSlack = 3 * 1400 + 14 + 34 + 34;   // the largest DU this stream can deliver
    std::uint32_t seq = 0;
    switch (r.below(4)) {
        case 0: seq = r.u32(); break;
        case 1: seq = 0xFFFFFFFFu - static_cast<std::uint32_t>(r.below(4)); break;   // wraps during the run
        case 2: seq = static_cast<std::uint32_t>(r.below(1000)); break;
        default: seq = 1676551952u; break;
    }
    std::uint32_t psn = r.u32();
    std::uint64_t tfdt = r.chance(50) ? 0 : static_cast<std::uint64_t>(r.u32());
    const bool tfdt_restarts = r.chance(40);
    std::vector<std::uint32_t> emitted_seqs;
    std::size_t checked = 0;
    auto checkEmitted = [&] {
        for (; checked < sink.frags.size(); ++checked) {
            const mmtp::MpuFragment& f = *sink.frags[checked];
            const std::string where = "seed " + std::to_string(seed) + " mpu " + std::to_string(f.mpu_sequence_number);
            if (!emitted_seqs.empty() && static_cast<std::int32_t>(f.mpu_sequence_number - emitted_seqs.back()) <= 0) {
                ++rs.order_violations;
                note(rs.first_order, where + " emitted after " + std::to_string(emitted_seqs.back()));
            }
            emitted_seqs.push_back(f.mpu_sequence_number);
            const auto plan = plans.find(f.mpu_sequence_number);
            if (plan == plans.end()) {
                ++rs.structure_failures;
                note(rs.first_structure, where + ": fragment for an MPU never sent");
                continue;
            }
            const FragmentCheck c = checkFragment(f, fx.media_track, plan->second.sizes);
            if (!c.structure) {
                ++rs.structure_failures;
                note(rs.first_structure, where + ": " + c.why);
            } else if (!c.mdat_matches) {
                if (plan->second.perturbed) {
                    ++rs.mdat_perturbed_failures;
                    note(rs.first_mdat_perturbed, where + ": " + c.why);
                } else {
                    ++rs.mdat_clean_failures;
                    note(rs.first_mdat_clean, where + ": " + c.why);
                }
            }
        }
    };
    auto send = [&](const Bytes& p) {
        feed.packet(p);
        ++rs.packets;
        if (a.stats().pending_bytes > cfg.max_pending_bytes + kSlack) {
            ++rs.pending_violations;
            note(rs.first_pending, "seed " + std::to_string(seed) + " packet " + std::to_string(rs.packets) + ": pending_bytes " + std::to_string(a.stats().pending_bytes) +
                                       " > " + std::to_string(cfg.max_pending_bytes) + " + " + std::to_string(kSlack));
        }
        checkEmitted();
    };
    const std::size_t n_mpus = 4 + r.below(7);
    std::vector<Bytes> previous;   // packets of the previous MPU, for late duplicates
    for (std::size_t k = 0; k < n_mpus; ++k) {
        if (k != 0) seq += 1 + (r.chance(15) ? static_cast<std::uint32_t>(r.below(4)) : 0u);
        MpuPlan plan;
        plan.seq = seq;
        ++rs.mpus;
        const bool perturb_prefix = r.chance(20);   // some MFUs of this MPU carry an MMTHSample with a wrong samplenumber
        const bool perturb_agg = r.chance(15);      // some aggregated packets of this MPU carry bad DU_lengths
        for (std::size_t i = 0; i < n; ++i) plan.sizes.push_back(1 + static_cast<std::uint32_t>(r.below(1400)));
        if (tfdt_restarts) tfdt = 0;
        else tfdt += fx.traf().total_duration;
        const Bytes moof_du = fx.moofWith(plan.sizes, tfdt);
        if (r.chance(40)) a.setPresentationTime(seq, anchor + (static_cast<std::uint64_t>(k) << 32));
        // DUs of this MPU in a random emission order
        std::vector<std::pair<std::uint8_t, Bytes>> dus;
        if (r.chance(85)) dus.push_back({0, r.chance(90) ? fx.initWithSeq(seq) : fx.init_du});
        std::vector<std::pair<std::uint8_t, Bytes>> mfus;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t number = static_cast<std::uint32_t>(i + 1);
            const Bytes sample = sampleBytes(number, plan.sizes[i]);
            const std::size_t variant = r.below(100);
            if (variant < 70) mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track))});
            else if (variant < 80) {
                if (perturb_prefix) {
                    mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track), true, number + 7)});   // wrong MMTHSample number
                    plan.perturbed = true;
                } else {
                    mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track))});
                }
            } else if (variant < 96) mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track), false)});
            else if (variant < 98) {   // the sample twice (duplicate MFU)
                mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track))});
                mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track))});
            } else {   // the sample plus a stray MFU with a sample number outside the trun (or 0)
                mfus.push_back({2, mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track))});
                mfus.push_back({2, mfuDu(r.chance(50) ? 0u : static_cast<std::uint32_t>(n + 1 + r.below(5)), 0, sampleBytes(99, 40), static_cast<std::uint8_t>(fx.media_track))});
            }
        }
        const std::size_t moof_at = r.below(mfus.size() + 1);   // before, in the middle of, or after the MFUs
        for (std::size_t i = 0; i < mfus.size(); ++i) {
            if (i == moof_at) dus.push_back({1, moof_du});
            dus.push_back(std::move(mfus[i]));
        }
        if (moof_at == mfus.size()) dus.push_back({1, moof_du});
        if (r.chance(3)) dus.push_back({2, cat({u32(7), ascii("item")})});   // non-timed MFU
        if (r.chance(5)) dus.push_back({static_cast<std::uint8_t>(3 + r.below(13)), ascii("private data")});   // unknown FT
        if (plan.perturbed) ++rs.perturbed_mpus;
        plans[seq] = plan;
        // packetize: aggregate runs of small MFUs, fragment big ones
        std::vector<Bytes> packets;
        for (std::size_t i = 0; i < dus.size(); ++i) {
            const std::uint8_t ft = dus[i].first;
            const bool timed = !(ft == 2 && dus[i].second.size() == 8 && std::memcmp(dus[i].second.data() + 4, "item", 4) == 0);
            if (ft == 2 && timed && dus[i].second.size() <= 400 && r.chance(30)) {
                Bytes data;
                std::size_t j = i;
                for (; j < dus.size() && j < i + 3 && dus[j].first == 2 && dus[j].second.size() <= 400; ++j) {
                    std::uint16_t len = static_cast<std::uint16_t>(dus[j].second.size());
                    if (perturb_agg && r.chance(25)) {   // bad DU_length
                        len = static_cast<std::uint16_t>(r.chance(50) ? 0 : (r.chance(50) ? len + 1 + r.below(40) : r.below(len)));
                        if (!plans[seq].perturbed) ++rs.perturbed_mpus;
                        plans[seq].perturbed = true;
                    }
                    const Bytes l = u16(len);
                    data.insert(data.end(), l.begin(), l.end());
                    data.insert(data.end(), dus[j].second.begin(), dus[j].second.end());
                }
                packets.push_back(mpuPacket(pid, psn++, 2, 0, 0, seq, data, true, true));
                i = j - 1;
                continue;
            }
            std::vector<Bytes> ps = duPackets(pid, psn, ft, seq, dus[i].second, ft == 2 && timed ? chunk : 1300, -1, timed);
            for (Bytes& p : ps) {
                if (r.chance(8)) p[17] = static_cast<std::uint8_t>(r.below(256));   // random fragment_counter (byte 17: 14-byte MMTP header + length)
                if (r.chance(2)) p[16] = static_cast<std::uint8_t>((p[16] & 0x0F) | (r.below(16) << 4));   // random FT on this packet
                packets.push_back(std::move(p));
            }
        }
        // loss, duplication (including late duplicates of the previous MPU), reordering
        std::vector<Bytes> wire;
        for (const Bytes& p : packets) {
            if (p_loss && r.chance(p_loss)) continue;
            wire.push_back(p);
            if (p_dup && r.chance(p_dup)) {
                if (!previous.empty() && r.chance(30)) wire.push_back(previous[r.below(previous.size())]);
                else wire.push_back(p);
            }
        }
        if (p_reorder) {
            for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
                if (!r.chance(p_reorder)) continue;
                const std::size_t j = std::min(wire.size() - 1, i + 1 + r.below(3));
                std::swap(wire[i], wire[j]);
            }
        }
        for (const Bytes& p : wire) send(p);
        previous = packets;
        if (r.chance(10)) {
            const std::uint64_t before = a.stats().fragments + a.stats().fragments_dropped;
            a.flush();
            if (a.stats().fragments + a.stats().fragments_dropped - before > cfg.max_pending_mpus) ++rs.flush_overflows;
            if (a.stats().pending_bytes != 0) ++rs.pending_violations;
            checkEmitted();
        }
    }
    const std::uint64_t before = a.stats().fragments + a.stats().fragments_dropped;
    a.flush();
    if (a.stats().fragments + a.stats().fragments_dropped - before > cfg.max_pending_mpus) ++rs.flush_overflows;
    checkEmitted();
    CHECK(a.stats().pending_bytes == 0);
    CHECK(a.stats().fragments == sink.frags.size());
    CHECK(a.stats().init_segments == sink.inits.size());
}

// ---- forked-child harness for the known crashers ---------------------------------------------------------------------

struct ChildResult {
    bool clean = false;
    std::string what;
};
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
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(3u) << 30;   // 3 GiB: a runaway allocation fails fast
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

/// Feeds one complete DU (fragmented at 1300 bytes) without Catch2 assertions, for use inside a forked child.
void rawDu(mmtp::MpuAssembler& a, std::uint16_t pid, std::uint32_t& psn, std::uint8_t ft, std::uint32_t seq, const Bytes& data) {
    for (const Bytes& p : duPackets(pid, psn, ft, seq, data, 1300)) {
        mmtp::PacketHeader h;
        mmtp::MpuPayloadHeader m;
        if (mmtp::parsePacket(view(p), h) != mmtp::ParseStatus::Ok || mmtp::parseMpuPayload(h.payload, m) != mmtp::ParseStatus::Ok) return;
        a.onPacket(h, m, 1000);
    }
}

}  // namespace

TEST_CASE("MPU assembler robustness: randomized packet streams keep the invariants", "[mmtp][robustness]") {
    std::vector<Fixture> fixtures;
    fixtures.emplace_back("unisoft-20-init.mp4", "unisoft-20-moof.m4s");
    fixtures.emplace_back("unisoft-21-init.mp4", "unisoft-21-moof.m4s");
    fixtures.emplace_back("bwi-100-init.mp4", "bwi-100-moof.m4s");
    fixtures.emplace_back("bwi-200-init.mp4", "bwi-200-moof.m4s");
    fixtures.emplace_back("bwi-300-init.mp4", "bwi-300-moof.m4s");
    RunStats rs;
    constexpr std::uint64_t kRuns = 28;
    for (std::uint64_t i = 0; i < kRuns; ++i) runStream(kSeed + i, fixtures, rs);
    INFO("packets " << rs.packets << ", MPUs " << rs.mpus << " (" << rs.perturbed_mpus << " with size perturbations)");
    CHECK(rs.packets > 3000);
    CHECK(rs.mpus > rs.perturbed_mpus * 2);
    {
        INFO("first: " << rs.first_structure);
        CHECK(rs.structure_failures == 0);
    }
    {
        INFO("first: " << rs.first_mdat_clean);
        CHECK(rs.mdat_clean_failures == 0);
    }
    CHECK(rs.flush_overflows == 0);
    {
        INFO("first: " << rs.first_order);
        CHECK(rs.order_violations == 0);
    }
    {
        INFO("first: " << rs.first_pending);
        // known defect: a single pending MPU is exempt from max_pending_bytes - evict() only resolves MPUs while more
        // than one is pending (src/mmtp/mpu.cpp evict()), so pending_bytes grows with the MPU
        CHECK(rs.pending_violations == 0);
    }
    {
        INFO("first: " << rs.first_mdat_perturbed);
        // known defect: an MFU whose MMTHSample samplenumber does not match the DU header keeps its 34-byte prefix (and
        // a mis-split aggregated DU keeps its wrong size) and is written as-is, so the mdat no longer matches the trun
        // sizes and nothing is counted or flagged (src/mmtp/mpu.cpp onMfu / emit)
        CHECK(rs.mdat_perturbed_failures == 0);
    }
}

TEST_CASE("MPU assembler robustness: known defects", "[mmtp][robustness]") {
    Fixture fx("bwi-300-init.mp4", "bwi-300-moof.m4s");   // one sample per MPU: the smallest on-air asset
    const std::vector<std::uint32_t> one{2000};
    const Bytes sample = sampleBytes(1, 2000);
    auto mfu = [&](std::uint32_t number) { return mfuDu(number, 0, sample, static_cast<std::uint8_t>(fx.media_track)); };

    SECTION("a backward MPU sequence jump (encoder restart) is never recovered") {
        Sink sink;
        mmtp::MpuAssembler a(300, sink.callbacks());
        Feeder f{a};
        auto mpu = [&](std::uint32_t seq) {
            f.packet(mpuPacket(300, 1, 0, 0, 0, seq, fx.initWithSeq(seq)));
            f.packet(mpuPacket(300, 2, 2, 0, 0, seq, mfu(1)));
            f.packet(mpuPacket(300, 3, 1, 0, 0, seq, fx.moofWith(one, 0)));
        };
        mpu(1676551952u);
        mpu(1676551953u);
        REQUIRE(sink.frags.size() == 2);
        for (std::uint32_t s = 1; s <= 10; ++s) mpu(s);   // the encoder restarted: sequence numbers start again at 1
        // known defect: src/mmtp/mpu.cpp onDu() treats every MPU with (seq - last_emitted) <= 0 as a late duplicate,
        // so after a backward jump the packet_id emits nothing until the sequence passes the old value (2^31 MPUs)
        CHECK(sink.frags.size() == 12);
        CHECK(a.stats().samples_late == 0);
    }

    SECTION("a single pending MPU is exempt from max_pending_bytes") {
        mmtp::MpuAssemblerConfig cfg;
        cfg.max_pending_bytes = 1u << 20;
        Sink sink;
        mmtp::MpuAssembler a(300, sink.callbacks(), cfg);
        Feeder f{a};
        std::uint32_t psn = 1;
        for (const Bytes& p : duPackets(300, psn, 0, 5, fx.initWithSeq(5), 1300)) f.packet(p);
        // 2000 samples of 2 KiB (4 MiB) of an MPU whose moof never arrives (stuck sequence number, sample numbers free)
        const Bytes one_du = mfu(1);
        for (std::uint32_t s = 1; s <= 2000; ++s) f.packet(mpuPacket(300, psn++, 2, 0, 0, 5, mfu(s)));
        CHECK(a.stats().dus == 2001);
        CHECK(sink.frags.empty());
        // known defect: evict() only resolves pending MPUs while more than one is pending, so the only MPU keeps growing
        CHECK(a.stats().pending_bytes <= cfg.max_pending_bytes + one_du.size());
    }

    SECTION("a late duplicate of an evicted MPU re-creates it and resolves every MPU still waiting for its moof") {
        mmtp::MpuAssemblerConfig cfg;
        cfg.max_pending_mpus = 3;
        Sink sink;
        mmtp::MpuAssembler a(300, sink.callbacks(), cfg);
        Feeder f{a};
        std::uint32_t psn = 1;
        auto du = [&](std::uint8_t ft, std::uint32_t seq, const Bytes& data) { f.packet(mpuPacket(300, psn++, ft, 0, 0, seq, data)); };
        du(0, 10, fx.initWithSeq(10));
        du(1, 10, fx.moofWith(one, 0));   // MPU 10: moof, its only sample lost
        for (std::uint32_t s = 11; s <= 12; ++s) {   // MPUs 11, 12: samples in, moofs still to come (Sinclair order)
            du(0, s, fx.initWithSeq(s));
            du(2, s, mfu(1));
        }
        du(0, 13, fx.initWithSeq(13));   // a fourth MPU evicts 10 (dropped: first sample missing)
        CHECK(a.stats().fragments_dropped == 1);
        CHECK(sink.frags.empty());
        du(2, 10, mfu(1));   // the lost sample of MPU 10 arrives late (retransmission / duplicate)
        du(1, 10, fx.moofWith(one, 0));
        du(2, 13, mfu(1));
        du(1, 11, fx.moofWith(one, 0));
        du(1, 12, fx.moofWith(one, 0));
        du(1, 13, fx.moofWith(one, 0));
        std::vector<std::uint32_t> emitted;
        for (const auto& fr : sink.frags) emitted.push_back(fr->mpu_sequence_number);
        INFO("emitted: " << [&] { std::string s; for (auto e : emitted) s += std::to_string(e) + " "; return s; }());
        // known defect: dropMpu() does not record the resolved sequence number, so onDu() accepts the late DUs of MPU
        // 10, mpuFor() appends it behind 11..13 and its completion makes drain() resolve (drop) 11 and 12 although their
        // moofs are still on the way; expected: 10 stays dropped, 11, 12, 13 are emitted in order
        CHECK(emitted == std::vector<std::uint32_t>{11, 12, 13});
        CHECK(a.stats().fragments_dropped == 1);
    }

    SECTION("max_pending_mpus = 0 is accepted and reads a popped deque element") {
        const Bytes init = fx.initWithSeq(5);
        const Bytes m1 = mfu(1);
        const ChildResult r = inChild(
            [&] {
                mmtp::MpuAssemblerConfig cfg;
                cfg.max_pending_mpus = 0;
                mmtp::MpuAssembler a(300, {}, cfg);
                std::uint32_t psn = 1;
                rawDu(a, 300, psn, 0, 5, init);
                rawDu(a, 300, psn, 2, 5, m1);
            },
            700);
        INFO("assembler with max_pending_mpus = 0: " << r.what);
        // known defect: mpuFor() pushes the new MPU, evict() pops everything (size > 0) and mpuFor() returns
        // mpus_.back() of the empty deque (heap-use-after-free under ASan, src/mmtp/mpu.cpp mpuFor / evict)
        CHECK(r.clean);
    }

    SECTION("corrupted trun sample sizes make emit() zero-fill gigabytes") {
        Fixture video("bwi-100-init.mp4", "bwi-100-moof.m4s");
        std::vector<std::uint32_t> sizes(video.sampleCount(), 0xFFFFFFFFu);
        sizes[0] = 100;
        const Bytes moof_du = video.moofWith(sizes, 0);
        const Bytes init = video.initWithSeq(7);
        const Bytes m1 = mfuDu(1, 0, sampleBytes(1, 100), static_cast<std::uint8_t>(video.media_track));
        const ChildResult r = inChild(
            [&] {
                mmtp::MpuAssembler a(100, {});
                std::uint32_t psn = 1;
                rawDu(a, 100, psn, 0, 7, init);
                rawDu(a, 100, psn, 2, 7, m1);
                rawDu(a, 100, psn, 1, 7, moof_du);
                a.flush();   // sample 1 present, 29 samples of 4 GiB each are "missing"
            },
            700);
        INFO("flush with 29 missing 4 GiB samples: " << r.what);
        // known defect: emit() allocates a zero buffer of the unvalidated trun size per missing sample
        // (src/mmtp/mpu.cpp emit(): zeros.emplace_back(traf->samples[i - 1].size, 0u)) and buildFragment() reserves the
        // sum: std::bad_alloc escapes flush()/onPacket() or the process is OOM-killed
        CHECK(r.clean);
    }
}
