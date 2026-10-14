// MPU reconstruction (spec 5.10.3) on synthetic MMTP packets around the on-air MPU metadata and movie-fragment DUs
// in tests/vectors/mmt: both emission orders, MFU fragmentation with the repeated DU header, the MMTHSample prefix,
// aggregation, loss (zero-fill and drop), the tfdt keep/rewrite timeline, and the session hook-up.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <map>
#include <vector>

#include "../media/mp4_builders.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/media/isobmff.hpp"
#include "atsc3/mmtp/mpu.hpp"
#include "atsc3/mmtp/session.hpp"

using namespace atsc3;
using namespace atsc3::test;
using namespace atsc3::test::mp4;

namespace {

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
/// down and the 14-byte MFU DU header repeated in every fragment, as both encoders do. `skip_fragment` drops one
/// packet (its sequence number is still consumed) to simulate loss.
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

struct Feeder {
    mmtp::MpuAssembler& a;
    std::uint16_t pid;
    std::uint32_t psn = 1000;
    std::int64_t t_ns = 1'000'000;
    void packet(const Bytes& bytes) {
        mmtp::PacketHeader h;
        REQUIRE(mmtp::parsePacket(view(bytes), h) == mmtp::ParseStatus::Ok);
        mmtp::MpuPayloadHeader m;
        REQUIRE(mmtp::parseMpuPayload(h.payload, m) == mmtp::ParseStatus::Ok);
        a.onPacket(h, m, t_ns);
        t_ns += 1000;
    }
    /// A DU, fragmented into 1300-byte packets when larger (UDP-sized, as on air).
    void du(std::uint8_t ft, std::uint32_t mpu_seq, const Bytes& data, bool timed = true) {
        for (const Bytes& p : duPackets(pid, psn, ft, mpu_seq, data, 1300, -1, timed)) packet(p);
    }
    void fragmented(std::uint8_t ft, std::uint32_t mpu_seq, const Bytes& data, std::size_t chunk, int skip_fragment = -1) {
        const auto packets = duPackets(pid, psn, ft, mpu_seq, data, chunk, skip_fragment);
        REQUIRE(packets.size() >= (skip_fragment < 0 ? 2u : 1u));
        for (const Bytes& p : packets) packet(p);
    }
    void aggregated(std::uint8_t ft, std::uint32_t mpu_seq, const std::vector<Bytes>& dus) {
        Bytes data;
        for (const Bytes& d : dus) {
            const Bytes len = u16(static_cast<std::uint16_t>(d.size()));
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), d.begin(), d.end());
        }
        packet(mpuPacket(pid, psn++, ft, 0, 0, mpu_seq, data, true, true));
    }
};

/// Sample content: deterministic bytes so the mdat can be checked.
Bytes sampleBytes(std::uint32_t number, std::uint32_t size) {
    Bytes b(size);
    for (std::uint32_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>((number * 31u + i) & 0xFF);
    return b;
}

/// MFU DU for timed media: 14-byte DU header + 34-byte MMTHSample + the sample.
Bytes mfuDu(std::uint32_t sample_number, std::uint32_t offset, const Bytes& sample, std::uint8_t track_id, bool with_hint = true) {
    Bytes du = cat({u32(1), u32(sample_number), u32(0), u8(1), u8(0)});   // offset is 0 on air
    if (with_hint) {
        Bytes hint = cat({u32(sample_number - 1), u8(track_id), u32(1), u32(sample_number), u8(1), u8(static_cast<std::uint8_t>(sample_number)), u32(offset),
                          u32(static_cast<std::uint32_t>(sample.size())), u32(11), fourcc("muli"), u8(0), u8(0), u8(0)});
        REQUIRE(hint.size() == 34);
        du.insert(du.end(), hint.begin(), hint.end());
    }
    du.insert(du.end(), sample.begin(), sample.end());
    return du;
}

struct Fixture {
    Bytes init_du, moof_du;
    media::MoofInfo moof;
    std::uint32_t media_track = 0;
    std::vector<Bytes> samples;
    Fixture(const std::string& init_name, const std::string& moof_name) : init_du(loadVector(init_name)), moof_du(loadVector(moof_name)) {
        media::InitSegmentInfo info;
        REQUIRE(media::parseInitSegment(view(init_du), info));
        REQUIRE(info.mediaTrack());
        media_track = info.mediaTrack()->track_id;
        REQUIRE(media::parseMoof(view(moof_du), moof));
        const media::TrackFragmentInfo* t = moof.traf(media_track);
        REQUIRE(t);
        for (std::size_t i = 0; i < t->samples.size(); ++i) samples.push_back(sampleBytes(static_cast<std::uint32_t>(i + 1), t->samples[i].size));
    }
    const media::TrackFragmentInfo& traf() const { return *moof.traf(media_track); }
    std::vector<Bytes> mfus(bool with_hint = true) const {
        std::vector<Bytes> out;
        std::uint32_t off = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            out.push_back(mfuDu(static_cast<std::uint32_t>(i + 1), off, samples[i], static_cast<std::uint8_t>(media_track), with_hint));
            off += static_cast<std::uint32_t>(samples[i].size());
        }
        return out;
    }
    /// A copy of the moof DU with its baseMediaDecodeTime replaced (version-1 tfdt on every fixture).
    Bytes moofWithTfdt(std::uint64_t tfdt) const {
        Bytes b = moof_du;
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

/// The mdat payload of an emitted fragment (after styp and moof').
ByteView mdatPayload(const mmtp::MpuFragment& f) {
    const auto bx = media::boxes(view(f.bytes));
    for (const media::Box& b : bx)
        if (b.type == "mdat") return b.payload;
    return {};
}

}  // namespace

TEST_CASE("MPU assembler: UniSoft order (metadata, moof, fragmented MFUs) yields an MSE init and a clean fragment", "[mmtp][mpu]") {
    Fixture fx("unisoft-20-init.mp4", "unisoft-20-moof.m4s");
    REQUIRE(fx.samples.size() == 60);
    Sink sink;
    mmtp::MpuAssembler a(20, sink.callbacks());
    Feeder f{a, 20};
    f.du(0, 996, fx.init_du);
    REQUIRE(sink.inits.size() == 1);
    const mmtp::MpuInitSegment& init = *sink.inits[0];
    CHECK(init.packet_id == 20);
    CHECK(init.mpu_sequence_number == 996);
    CHECK(init.media.track_id == 1);
    CHECK(init.media.codecs == "hev1.2.4.L120.B0");
    CHECK(init.media.kind() == "video");
    CHECK(init.media.timescale == 1000000u);
    CHECK(init.hint_track_id == 2u);
    CHECK(init.hint_sample_size == 34u);
    CHECK(init.info.has_mmpu);
    CHECK(init.info.mmpu_sequence_number == 996u);
    const std::string head(reinterpret_cast<const char*>(init.bytes.data()), init.bytes.size());
    CHECK(head.compare(4, 4, "ftyp") == 0);
    CHECK(head.find("mmpu") == std::string::npos);
    CHECK(head.find("mmth") == std::string::npos);
    CHECK(init.raw.empty());

    f.du(1, 996, fx.moof_du);
    CHECK(sink.frags.empty());
    const auto mfus = fx.mfus();
    for (std::size_t i = 0; i < mfus.size(); ++i) {
        if (mfus[i].size() > 1200) f.fragmented(2, 996, mfus[i], 1200);
        else f.du(2, 996, mfus[i]);
    }
    REQUIRE(sink.frags.size() == 1);
    const mmtp::MpuFragment& frag = *sink.frags[0];
    CHECK(frag.mpu_sequence_number == 996);
    CHECK(frag.sample_count == 60);
    CHECK(frag.samples_missing == 0);
    CHECK_FALSE(frag.damaged);
    CHECK_FALSE(frag.discontinuity);
    CHECK(frag.timescale == 1000000u);
    CHECK(frag.tfdt_received == 1991990000u);
    CHECK(frag.tfdt == 1991990000u);
    CHECK_FALSE(frag.tfdt_rewritten);
    CHECK(frag.duration == 2002020u);
    CHECK(a.timelineMode() == mmtp::TimelineMode::Keep);
    CHECK(std::string(reinterpret_cast<const char*>(frag.bytes.data()) + 4, 4) == "styp");
    // the output is styp + moof' + mdat' with the hint traf gone and data_offset repaired
    const auto top = media::boxes(view(frag.bytes));
    REQUIRE(top.size() == 3);
    CHECK(top[0].type == "styp");
    CHECK(top[1].type == "moof");
    CHECK(top[2].type == "mdat");
    media::MoofInfo out;
    REQUIRE(media::parseMoof(ByteView(frag.bytes.data() + top[1].offset, frag.bytes.size() - top[1].offset), out));
    REQUIRE(out.trafs.size() == 1);
    CHECK(out.trafs[0].track_id == 1);
    CHECK(out.trafs[0].data_offset == static_cast<std::int32_t>(top[1].size + 8));
    const ByteView mdat = mdatPayload(frag);
    std::size_t total = 0;
    for (const Bytes& s : fx.samples) total += s.size();
    REQUIRE(mdat.size() == total);
    CHECK(std::memcmp(mdat.data(), fx.samples[0].data(), fx.samples[0].size()) == 0);
    CHECK(std::memcmp(mdat.data() + total - fx.samples.back().size(), fx.samples.back().data(), fx.samples.back().size()) == 0);
    const mmtp::MpuAssemblerStats& st = a.stats();
    CHECK(st.init_segments == 1);
    CHECK(st.fragments == 1);
    CHECK(st.du_drops == 0);
    CHECK(st.hint_prefix_missing == 0);
    CHECK(st.pending_bytes == 0);

    // the next MPU: repeated metadata (only the mmpu sequence differs) is not re-emitted; a later tfdt keeps the timeline
    f.du(0, 997, fx.initWithSeq(997));
    CHECK(sink.inits.size() == 1);
    CHECK(a.stats().init_repeats == 1);
    f.du(1, 997, fx.moofWithTfdt(1991990000u + 2002000u));
    for (const Bytes& m : fx.mfus()) f.du(2, 997, m);
    REQUIRE(sink.frags.size() == 2);
    CHECK(sink.frags[1]->tfdt == 1991990000u + 2002000u);
    CHECK_FALSE(sink.frags[1]->tfdt_rewritten);
    CHECK_FALSE(sink.frags[1]->discontinuity);
    CHECK(a.stats().mpus == 2);
    CHECK(a.stats().mpu_gaps == 0);
}

TEST_CASE("MPU assembler: Sinclair order (MFUs before the moof) with restarting tfdt is rewritten from the presentation time", "[mmtp][mpu]") {
    Fixture fx("bwi-100-init.mp4", "bwi-100-moof.m4s");
    REQUIRE(fx.samples.size() == 30);
    REQUIRE(fx.traf().tfdt == 0u);
    Sink sink;
    mmtp::MpuAssembler a(100, sink.callbacks());
    const std::uint64_t anchor = (2208988800ull + 1678228503ull) << 32;   // 2023-03-07 22:35:03 UTC
    a.setAnchor(anchor);
    auto pt = [&](double seconds) { return anchor + static_cast<std::uint64_t>(seconds * 4294967296.0); };
    Feeder f{a, 100};
    const std::uint32_t seq0 = 1676551952u;
    a.setPresentationTime(seq0, pt(1.001));
    f.du(0, seq0, fx.init_du);
    REQUIRE(sink.inits.size() == 1);
    CHECK(sink.inits[0]->media.codecs == "hev1.2.4.L93.B0");
    CHECK(sink.inits[0]->media.timescale == 240000u);
    for (const Bytes& m : fx.mfus()) {
        if (m.size() > 1300) f.fragmented(2, seq0, m, 1300);
        else f.du(2, seq0, m);
    }
    CHECK(sink.frags.empty());   // the moof has not arrived yet
    CHECK(a.stats().pending_bytes > 0);
    f.du(1, seq0, fx.moof_du);
    REQUIRE(sink.frags.size() == 1);
    CHECK(a.timelineMode() == mmtp::TimelineMode::Rewrite);
    CHECK(sink.frags[0]->tfdt_received == 0u);
    CHECK(sink.frags[0]->tfdt == 240240u);   // 1.001 s * 240000
    CHECK(sink.frags[0]->tfdt_rewritten);
    CHECK(sink.frags[0]->duration == 240240u);
    CHECK(sink.frags[0]->presentation_ntp == pt(1.001));
    CHECK(a.timestampOffsetSeconds() == 0.0);
    // the rewritten tfdt is really in the bytes
    const auto top = media::boxes(view(sink.frags[0]->bytes));
    media::MoofInfo out;
    REQUIRE(media::parseMoof(ByteView(sink.frags[0]->bytes.data() + top[1].offset, sink.frags[0]->bytes.size() - top[1].offset), out));
    REQUIRE(out.trafs.size() == 1);
    CHECK(out.trafs[0].tfdt == 240240u);
    CHECK(a.stats().pending_bytes == 0);

    // second MPU with its descriptor, third without one (continues by duration)
    a.setPresentationTime(seq0 + 1, pt(2.002));
    f.du(0, seq0 + 1, fx.initWithSeq(seq0 + 1));
    for (const Bytes& m : fx.mfus()) f.du(2, seq0 + 1, m);
    f.du(1, seq0 + 1, fx.moof_du);
    f.du(0, seq0 + 2, fx.initWithSeq(seq0 + 2));
    for (const Bytes& m : fx.mfus()) f.du(2, seq0 + 2, m);
    f.du(1, seq0 + 2, fx.moof_du);
    REQUIRE(sink.frags.size() == 3);
    CHECK(sink.frags[1]->tfdt == 480480u);
    CHECK(sink.frags[2]->tfdt == 720720u);
    CHECK(sink.frags[2]->tfdt_rewritten);
    CHECK_FALSE(sink.frags[2]->presentation_ntp);
    CHECK(a.stats().tfdt_rewrites == 3);
}

TEST_CASE("MPU assembler: loss handling - zero-filled sample, dropped MPU without its first sample, discontinuity", "[mmtp][mpu]") {
    Fixture fx("unisoft-21-init.mp4", "unisoft-21-moof.m4s");
    const std::size_t n = fx.samples.size();
    REQUIRE(n >= 90);
    Sink sink;
    mmtp::MpuAssembler a(21, sink.callbacks());
    Feeder f{a, 21};
    f.du(0, 996, fx.init_du);
    f.du(1, 996, fx.moof_du);
    auto mfus = fx.mfus();
    // sample 5 fragmented, its middle fragment lost -> the DU is dropped
    Bytes big = mfus[4];
    big.resize(big.size() + 3000, 0x5A);   // make it large enough to fragment (sizes differ from the trun: still accepted as given)
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 4) f.fragmented(2, 996, big, 1000, 1);
        else f.du(2, 996, mfus[i]);
    }
    CHECK(sink.frags.empty());
    CHECK(a.stats().du_drops == 1);
    a.flush();
    REQUIRE(sink.frags.size() == 1);
    CHECK(sink.frags[0]->damaged);
    CHECK(sink.frags[0]->samples_missing == 1);
    CHECK(a.stats().samples_zero_filled == 1);
    CHECK(a.stats().fragments_damaged == 1);
    const ByteView mdat = mdatPayload(*sink.frags[0]);
    std::size_t off = 0;
    for (std::size_t i = 0; i < 4; ++i) off += fx.samples[i].size();
    bool zero = true;
    for (std::size_t i = 0; i < fx.samples[4].size(); ++i) zero = zero && mdat[off + i] == 0;
    CHECK(zero);
    CHECK(std::memcmp(mdat.data() + off + fx.samples[4].size(), fx.samples[5].data(), fx.samples[5].size()) == 0);

    // next MPU: sample 1 missing -> dropped; the MPU after it is flagged as a discontinuity
    f.du(0, 997, fx.initWithSeq(997));
    f.du(1, 997, fx.moofWithTfdt(1991992000u + 2008667u));
    for (std::size_t i = 1; i < n; ++i) f.du(2, 997, mfus[i]);
    f.du(0, 998, fx.initWithSeq(998));
    f.du(1, 998, fx.moofWithTfdt(1991992000u + 2 * 2008667u));
    for (std::size_t i = 0; i < n; ++i) f.du(2, 998, mfus[i]);
    // 998 completing resolves the older 997 at once: first sample missing -> dropped, 998 emitted with a discontinuity
    REQUIRE(sink.frags.size() == 2);
    CHECK(sink.frags[1]->mpu_sequence_number == 998);
    CHECK(sink.frags[1]->discontinuity);
    CHECK(a.stats().fragments_dropped == 1);
    a.flush();
    CHECK(sink.frags.size() == 2);
    // an MPU sequence gap marks the next fragment
    f.du(0, 1000, fx.initWithSeq(1000));
    f.du(1, 1000, fx.moofWithTfdt(1991992000u + 4 * 2008667u));
    for (std::size_t i = 0; i < n; ++i) f.du(2, 1000, mfus[i]);
    REQUIRE(sink.frags.size() == 3);
    CHECK(sink.frags[2]->discontinuity);
    CHECK(a.stats().mpu_gaps >= 1);
}

TEST_CASE("MPU assembler: aggregated MFUs, MFUs without the hint prefix, non-timed and unknown fragment types", "[mmtp][mpu]") {
    Fixture fx("unisoft-21-init.mp4", "unisoft-21-moof.m4s");
    const std::size_t n = fx.samples.size();
    Sink sink;
    mmtp::MpuAssembler a(21, sink.callbacks());
    Feeder f{a, 21};
    f.du(0, 996, fx.init_du);
    f.du(1, 996, fx.moof_du);
    const auto mfus = fx.mfus(false);   // no MMTHSample prefix: the whole payload is the sample
    for (std::size_t i = 0; i + 3 <= n; i += 3) f.aggregated(2, 996, {mfus[i], mfus[i + 1], mfus[i + 2]});
    for (std::size_t i = n - n % 3; i < n; ++i) f.du(2, 996, mfus[i]);
    REQUIRE(sink.frags.size() == 1);
    CHECK_FALSE(sink.frags[0]->damaged);
    CHECK(a.stats().aggregated_dus == n / 3);
    CHECK(a.stats().hint_prefix_missing == n);
    const ByteView mdat = mdatPayload(*sink.frags[0]);
    CHECK(std::memcmp(mdat.data(), fx.samples[0].data(), fx.samples[0].size()) == 0);
    // non-timed MFU and an unknown FT are counted and ignored
    f.du(2, 997, cat({u32(7), ascii("item")}), false);
    f.du(5, 997, ascii("private"));
    CHECK(a.stats().non_timed == 1);
    CHECK(a.stats().unknown_ft == 1);
    CHECK(sink.frags.size() == 1);
}

TEST_CASE("MMTP session: MPU assembly is opt-in, per packet_id, and forwards init segments and fragments", "[mmtp][mpu]") {
    Fixture fx("unisoft-20-init.mp4", "unisoft-20-moof.m4s");
    std::vector<std::shared_ptr<const mmtp::MpuInitSegment>> inits;
    std::vector<std::shared_ptr<const mmtp::MpuFragment>> frags;
    mmtp::SessionCallbacks cb;
    cb.onMpuInit = [&](std::shared_ptr<const mmtp::MpuInitSegment> i) { inits.push_back(std::move(i)); };
    cb.onMpuFragment = [&](std::shared_ptr<const mmtp::MpuFragment> f) { frags.push_back(std::move(f)); };
    mmtp::Session s(std::move(cb));
    std::uint32_t psn = 1;
    auto send = [&](const Bytes& pkt) {
        ip::UdpDatagram d;
        d.payload = view(pkt);
        d.time.t_ns = 1'000'000;
        s.onDatagram(d);
    };
    send(mpuPacket(20, psn++, 0, 0, 0, 996, fx.init_du));
    CHECK(inits.empty());            // assembly off by default
    CHECK(s.assemblerIds().empty());
    s.setMpuAssembly(true);
    send(mpuPacket(20, psn++, 0, 0, 0, 996, fx.init_du));
    REQUIRE(inits.size() == 1);
    REQUIRE(s.assembler(20));
    send(mpuPacket(20, psn++, 1, 0, 0, 996, fx.moof_du));
    for (const Bytes& m : fx.mfus())
        for (const Bytes& p : duPackets(20, psn, 2, 996, m, 1300)) send(p);
    REQUIRE(frags.size() == 1);
    CHECK(frags[0]->packet_id == 20);
    CHECK(s.assembler(20)->stats().fragments == 1);
    s.setMpuAssembly(false);
    CHECK(s.assemblerIds().empty());
    send(mpuPacket(20, psn++, 0, 0, 0, 997, fx.init_du));
    CHECK(inits.size() == 1);
}
