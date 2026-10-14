// MPU reconstruction (spec 5.10.3): DU reassembly, MFU collection, MSE segment emission, per-asset timeline.
// On-air facts this follows (spec 3.2 item 20): the 14-byte MFU DU header is repeated in every fragment of a
// fragmented MFU; the DU-header offset is 0 and the usable position/length sit in the 34-byte MMTHSample that prefixes
// every MFU payload; the moof may arrive after all MFUs of the MPU (Sinclair) or before them (UniSoft); tfdt either
// increases across MPUs (UniSoft) or restarts at 0 every MPU (Sinclair).
// Robustness rules (spec 1.3): every buffer is bounded by configuration or a hard cap, every trun-declared size is
// checked before it drives an allocation, and hostile sequence numbers cannot grow the state.
#include "atsc3/mmtp/mpu.hpp"

#include <algorithm>
#include <cstring>

#include "atsc3/io/inet.hpp"

namespace atsc3::mmtp {

double ntpToUnixSeconds(std::uint64_t ntp) noexcept {
    const double secs = static_cast<double>(ntp >> 32) - 2208988800.0;
    return secs + static_cast<double>(ntp & 0xFFFFFFFFu) / 4294967296.0;
}

double ntpDeltaSeconds(std::uint64_t a, std::uint64_t b) noexcept {
    return static_cast<double>(static_cast<std::int64_t>(b - a)) / 4294967296.0;
}

namespace {
constexpr std::size_t kMfuHeader = 14;        // movie_fragment_sequence_number(32) sample_number(32) offset(32) priority(8) dep_counter(8)
constexpr std::size_t kHintSample = 34;       // ISO 23008-1 MMTHSample + its 11-byte 'muli' box, as sent by both encoders
constexpr std::size_t kMaxPts = 64;
constexpr std::size_t kMaxPendingDuBytes = 16u << 20;    // one DU under reassembly (an IDR sample is ~270 kB on air)
constexpr std::size_t kMaxPendingDuFragments = 1024;
constexpr std::uint32_t kMaxSamplesPerMpu = 1u << 16;    // sample numbers beyond this are hostile (94 on air)
constexpr std::uint64_t kMaxFragmentBytes = 256ull << 20;   // trun-declared bytes of one movie fragment
constexpr std::size_t kSampleEntryOverhead = 64;         // charged per stored sample so header-only MFUs hit the cap
constexpr std::int32_t kRestartWindow = 1 << 16;         // a backward MPU jump beyond this is an encoder restart

/// True when `payload` starts with an MMTHSample whose samplenumber matches the DU header.
bool hasHintPrefix(ByteView payload, std::uint32_t sample_number) noexcept {
    if (payload.size() < kHintSample) return false;
    const std::uint8_t* p = payload.data();
    // sequence_number(4) trackrefindex(1) movie_fragment_sequence_number(4) samplenumber(4) priority(1) dependency_counter(1)
    // offset(4) length(4) then box size(4)=11 type 'muli'
    return io::be32(p + 23) == 11 && std::memcmp(p + 27, "muli", 4) == 0 && io::be32(p + 9) == sample_number;
}
/// A hint prefix whose samplenumber does not match (the lenient check used at emission time).
bool looksLikeHintPrefix(ByteView payload) noexcept {
    return payload.size() >= kHintSample && io::be32(payload.data() + 23) == 11 && std::memcmp(payload.data() + 27, "muli", 4) == 0;
}
}  // namespace

MpuAssembler::MpuAssembler(std::uint16_t packet_id, MpuAssemblerCallbacks cb, MpuAssemblerConfig cfg)
    : packet_id_(packet_id), cb_(std::move(cb)), cfg_(cfg) {
    cfg_.max_pending_mpus = std::max<std::size_t>(1, cfg_.max_pending_mpus);
}

void MpuAssembler::setPresentationTime(std::uint32_t mpu_sequence_number, std::uint64_t ntp) {
    pts_[mpu_sequence_number] = ntp;
    while (pts_.size() > kMaxPts) pts_.erase(pts_.begin());
}

void MpuAssembler::setAnchor(std::uint64_t ntp) {
    if (!anchor_) anchor_ = ntp;
}

std::optional<std::uint64_t> MpuAssembler::presentationTime(std::uint32_t mpu_sequence_number) const {
    const auto it = pts_.find(mpu_sequence_number);
    if (it == pts_.end()) return std::nullopt;
    return it->second;
}

std::optional<double> MpuAssembler::timestampOffsetSeconds() const noexcept { return offset_s_; }

// ---- packet level: DU reassembly ----------------------------------------------------------------------------------

void MpuAssembler::dropPendingDu() {
    if (pending_.active) ++stats_.du_drops;
    pending_.active = false;
    pending_.bytes.clear();
    pending_.head.clear();
    pending_.fragments = 0;
}

void MpuAssembler::onPacket(const PacketHeader& h, const MpuPayloadHeader& m, std::int64_t t_ns) {
    ++stats_.packets;
    if (m.aggregation) {   // ISO 23008-1 §9.3.2: A=1 -> repeated DU_length(16) + DU, each complete
        if (pending_.active) dropPendingDu();
        ++stats_.aggregated_dus;
        std::size_t off = 0;
        while (off + 2 <= m.data.size()) {
            const std::size_t len = io::be16(m.data.data() + off);
            off += 2;
            if (len == 0 || off + len > m.data.size()) break;
            onDu(m.mpu_sequence_number, m.fragment_type, m.timed, m.data.subview(off, len), t_ns);
            off += len;
        }
        return;
    }
    switch (m.fragmentation_indicator) {
        case 0:   // complete DU
            if (pending_.active) dropPendingDu();
            onDu(m.mpu_sequence_number, m.fragment_type, m.timed, m.data, t_ns);
            return;
        case 1: {  // first fragment
            if (pending_.active) dropPendingDu();
            ++stats_.du_fragments;
            pending_.active = true;
            pending_.mpu_seq = m.mpu_sequence_number;
            pending_.ft = m.fragment_type;
            pending_.timed = m.timed;
            pending_.last_pseq = h.packet_sequence_number;
            pending_.fragments = 1;
            pending_.bytes.assign(m.data.begin(), m.data.end());
            pending_.head.clear();
            if (m.fragment_type == 2 && m.timed && m.data.size() >= kMfuHeader) pending_.head.assign(m.data.begin(), m.data.begin() + kMfuHeader);
            return;
        }
        default: {  // 2 middle, 3 last
            ++stats_.du_fragments;
            const bool continuous = pending_.active && pending_.mpu_seq == m.mpu_sequence_number && pending_.ft == m.fragment_type &&
                                    h.packet_sequence_number == pending_.last_pseq + 1u;
            if (!continuous) {
                dropPendingDu();
                return;
            }
            pending_.last_pseq = h.packet_sequence_number;
            ByteView part = m.data;
            // The MFU DU header is repeated in every fragment (both encoders): strip it when it matches the first one.
            if (!pending_.head.empty() && part.size() >= kMfuHeader && std::memcmp(part.data(), pending_.head.data(), 8) == 0) part = part.subview(kMfuHeader);
            if (++pending_.fragments > kMaxPendingDuFragments || pending_.bytes.size() + part.size() > kMaxPendingDuBytes) {
                dropPendingDu();   // a DU that never ends (or an absurd one) must not buffer the stream
                return;
            }
            pending_.bytes.insert(pending_.bytes.end(), part.begin(), part.end());
            if (m.fragmentation_indicator == 3) {
                std::vector<std::uint8_t> du = std::move(pending_.bytes);
                pending_.active = false;
                pending_.bytes.clear();
                pending_.head.clear();
                pending_.fragments = 0;
                onDu(m.mpu_sequence_number, m.fragment_type, m.timed, ByteView(du.data(), du.size()), t_ns);
            }
            return;
        }
    }
}

// ---- DU level -----------------------------------------------------------------------------------------------------

std::int32_t MpuAssembler::deltaFromResolved(std::uint32_t seq) const noexcept {
    return last_resolved_seq_ ? static_cast<std::int32_t>(seq - *last_resolved_seq_) : 1;
}

void MpuAssembler::restart(std::uint32_t seq) {
    // Encoder restart: the MPU numbering and (in Keep mode) the tfdt timeline start over. Pending data is useless.
    (void)seq;
    flush();
    last_resolved_seq_.reset();
    pending_discontinuity_ = true;
    pending_restart_ = true;
    mode_ = TimelineMode::Undecided;
    last_tfdt_received_.reset();
    last_tfdt_out_.reset();
    last_duration_.reset();
    first_pt_.reset();
    first_tfdt_.reset();
    offset_s_.reset();
    rewrite_shift_s_ = 0.0;
    shift_known_ = false;
    ++stats_.mpu_restarts;
}

MpuAssembler::Mpu& MpuAssembler::mpuFor(std::uint32_t seq, std::int64_t t_ns) {
    for (Mpu& m : mpus_)
        if (m.seq == seq) return m;
    const std::uint32_t last = mpus_.empty() ? last_resolved_seq_.value_or(seq - 1u) : mpus_.back().seq;
    if (seq != last + 1u) {
        ++stats_.mpu_gaps;
        pending_discontinuity_ = true;
    }
    ++stats_.mpus;
    Mpu m;
    m.seq = seq;
    m.first_ns = t_ns;
    // keep the deque in sequence order (a lower number must never sit behind higher ones)
    auto pos = mpus_.end();
    while (pos != mpus_.begin() && static_cast<std::int32_t>(seq - std::prev(pos)->seq) < 0) --pos;
    mpus_.insert(pos, std::move(m));
    evictCount();
    for (Mpu& x : mpus_)
        if (x.seq == seq) return x;
    // the new MPU was itself evicted (the deque was full of newer ones): give the caller a fresh, newest slot
    Mpu scratch;
    scratch.seq = seq;
    scratch.first_ns = t_ns;
    mpus_.push_back(std::move(scratch));
    return mpus_.back();
}

void MpuAssembler::onDu(std::uint32_t mpu_seq, std::uint8_t ft, bool timed, ByteView du, std::int64_t t_ns) {
    ++stats_.dus;
    const std::int32_t delta = deltaFromResolved(mpu_seq);
    if (delta < -kRestartWindow) {
        restart(mpu_seq);
    } else if (delta <= 0) {   // a DU of an MPU already emitted or resolved (late duplicate); MPUs are sequential per packet_id
        if (ft == 2) ++stats_.samples_late;
        return;
    }
    switch (ft) {
        case 0:
            onInitDu(mpu_seq, du, t_ns);
            mpuFor(mpu_seq, t_ns);
            drain();
            break;
        case 1:
            onMoofDu(mpuFor(mpu_seq, t_ns), du);
            drain();
            break;
        case 2:
            if (!timed) {
                ++stats_.non_timed;
                break;
            }
            onMfu(mpuFor(mpu_seq, t_ns), du);
            drain();
            break;
        default:
            ++stats_.unknown_ft;
            break;
    }
}

void MpuAssembler::onInitDu(std::uint32_t mpu_seq, ByteView du, std::int64_t t_ns) {
    // The MPU metadata repeats for every MPU; only the mmpu box (sequence number) changes. Compare without it.
    std::vector<std::uint8_t> key;
    key.reserve(du.size());
    for (const media::Box& b : media::boxes(du)) {
        if (b.type == "mmpu") continue;
        key.insert(key.end(), du.data() + b.offset, du.data() + b.offset + b.size);
    }
    if (init_ && key == init_key_) {
        ++stats_.init_repeats;
        return;
    }
    media::CleanInitOptions opts;
    media::CleanInitResult res;
    std::string err;
    if (!media::cleanInitSegment(du, opts, res, &err) || !res.info.mediaTrack()) {
        ++stats_.init_errors;
        return;
    }
    auto init = std::make_shared<MpuInitSegment>();
    init->packet_id = packet_id_;
    init->mpu_sequence_number = mpu_seq;
    init->bytes = std::move(res.bytes);
    if (cfg_.keep_raw_init) init->raw.assign(du.begin(), du.end());
    init->info = res.info;
    init->media = *res.info.mediaTrack();
    init->hint_track_id = res.hint_track_id;
    init->hint_sample_size = res.hint_sample_size.value_or(0);
    init->received_ns = t_ns;
    timescale_ = init->media.timescale;
    init_ = init;
    init_key_ = std::move(key);
    ++stats_.init_segments;
    if (cb_.onInit) cb_.onInit(init_);
}

void MpuAssembler::onMoofDu(Mpu& mpu, ByteView du) {
    media::MoofInfo info;
    std::string err;
    if (!media::parseMoof(du, info, &err)) {
        ++stats_.build_errors;
        return;
    }
    // trun-declared sizes drive allocations later: refuse absurd fragments up front
    for (const media::TrackFragmentInfo& t : info.trafs) {
        if (t.samples.size() > kMaxSamplesPerMpu || t.total_size > kMaxFragmentBytes) {
            ++stats_.build_errors;
            return;
        }
    }
    mpu.moof_du.assign(du.begin(), du.end());
    mpu.moof = std::move(info);
}

void MpuAssembler::onMfu(Mpu& mpu, ByteView du) {
    if (du.size() < kMfuHeader) return;
    const std::uint32_t sample_number = io::be32(du.data() + 4);
    ByteView payload = du.subview(kMfuHeader);
    if (hasHintPrefix(payload, sample_number)) {
        payload = payload.subview(kHintSample);
    } else if (init_ && init_->hint_sample_size != 0) {
        ++stats_.hint_prefix_missing;
    }
    if (sample_number == 0 || sample_number > kMaxSamplesPerMpu) {
        ++stats_.samples_rejected;
        return;
    }
    if (mpu.moof && init_) {   // the moof bounds the sample numbers once it is known
        const media::TrackFragmentInfo* traf = mpu.moof->traf(init_->media.track_id);
        if (traf && sample_number > traf->samples.size()) {
            ++stats_.samples_rejected;
            return;
        }
    }
    auto it = mpu.samples.find(sample_number);
    if (it != mpu.samples.end()) {   // repeated MFU (retransmission or duplicate): keep the first
        ++stats_.samples_late;
        return;
    }
    const std::size_t charge = payload.size() + kSampleEntryOverhead;
    mpu.samples.emplace(sample_number, std::vector<std::uint8_t>(payload.begin(), payload.end()));
    mpu.sample_bytes += charge;
    stats_.pending_bytes += charge;
    if (stats_.pending_bytes > cfg_.max_pending_bytes) evictBytes();   // may resolve `mpu` itself: no use of it after this
}

bool MpuAssembler::complete(const Mpu& m) const {
    if (!m.moof || !init_) return false;
    const media::TrackFragmentInfo* traf = m.moof->traf(init_->media.track_id);
    if (!traf) return false;
    const std::size_t n = traf->samples.size();
    if (m.samples.size() < n) return false;
    for (std::uint32_t i = 1; i <= n; ++i)
        if (!m.samples.count(i)) return false;
    return true;
}

void MpuAssembler::release(Mpu& m) {
    stats_.pending_bytes -= std::min(stats_.pending_bytes, m.sample_bytes);
    m.samples.clear();
    m.sample_bytes = 0;
    m.moof.reset();
    m.moof_du.clear();
}

void MpuAssembler::markResolved(std::uint32_t seq) {
    if (!last_resolved_seq_ || static_cast<std::int32_t>(seq - *last_resolved_seq_) > 0) last_resolved_seq_ = seq;
}

void MpuAssembler::dropMpu(Mpu& m, const char* why) {
    (void)why;
    ++stats_.fragments_dropped;
    pending_discontinuity_ = true;
    markResolved(m.seq);
    release(m);
}

void MpuAssembler::emit(Mpu& m, bool forced) {
    (void)forced;
    if (!m.moof || !init_) {
        if (!m.samples.empty() || !m.moof_du.empty()) {
            dropMpu(m, "no moof or init");
        } else {
            markResolved(m.seq);
            release(m);
        }
        return;
    }
    const media::TrackFragmentInfo* traf = m.moof->traf(init_->media.track_id);
    if (!traf) {
        dropMpu(m, "no media traf");
        return;
    }
    const std::size_t n = traf->samples.size();
    // Present samples must match the trun sizes (a hint prefix with a wrong sample number is stripped here; anything
    // else is zero-filled and flagged), missing ones are zero-filled, and the total zero-fill is bounded.
    std::uint32_t missing = 0;
    std::uint64_t zero_bytes = 0;
    std::vector<ByteView> views;
    views.reserve(n);
    for (std::uint32_t i = 1; i <= n; ++i) {
        const std::uint32_t want = traf->samples[i - 1].size;
        const auto it = m.samples.find(i);
        ByteView v;
        if (it != m.samples.end()) {
            v = ByteView(it->second.data(), it->second.size());
            if (v.size() == static_cast<std::size_t>(want) + kHintSample && looksLikeHintPrefix(v)) v = v.subview(kHintSample);
            if (v.size() != want) {
                ++stats_.samples_size_mismatch;
                v = ByteView();
            }
        }
        if (v.empty() && want != 0) {
            ++missing;
            zero_bytes += want;
        }
        views.push_back(v);
    }
    if (missing != 0 && cfg_.drop_without_first_sample && (n == 0 || views[0].empty())) {
        dropMpu(m, "first sample missing");
        return;
    }
    if (zero_bytes > cfg_.max_pending_bytes) {
        dropMpu(m, "implausible zero-fill");
        return;
    }
    std::vector<std::uint8_t> zero;   // one shared zero buffer for every missing sample
    if (zero_bytes != 0) {
        std::size_t largest = 0;
        for (std::uint32_t i = 0; i < n; ++i)
            if (views[i].empty()) largest = std::max<std::size_t>(largest, traf->samples[i].size);
        zero.assign(largest, 0u);
        for (std::uint32_t i = 0; i < n; ++i)
            if (views[i].empty() && traf->samples[i].size != 0) views[i] = ByteView(zero.data(), traf->samples[i].size);
        stats_.samples_zero_filled += missing;
    }

    // timeline
    const std::uint64_t tfdt_received = traf->tfdt.value_or(0);
    const std::uint32_t ts = init_->media.timescale != 0 ? init_->media.timescale : 1;
    const auto pt = presentationTime(m.seq);
    if (mode_ == TimelineMode::Undecided) {
        mode_ = tfdt_received == 0 ? TimelineMode::Rewrite : TimelineMode::Keep;
        if (mode_ == TimelineMode::Rewrite) {
            offset_s_ = 0.0;
        } else {
            // the component's constant SourceBuffer offset, frozen at the first emission: with a presentation time
            // the asset lands on the service timeline, without one it starts at its own tfdt origin
            offset_s_ = (pt && anchor_) ? ntpDeltaSeconds(*anchor_, *pt) - static_cast<double>(tfdt_received) / static_cast<double>(ts) : 0.0;
            if (pt) {
                first_pt_ = pt;
                first_tfdt_ = tfdt_received;
            }
        }
    } else if (mode_ == TimelineMode::Keep && last_tfdt_received_ && tfdt_received <= *last_tfdt_received_) {
        mode_ = TimelineMode::Rewrite;   // the encoder restarted its timeline: continue on the presentation clock, the offset stays frozen
    }
    std::uint64_t tfdt_out = tfdt_received;
    bool rewritten = false;
    if (mode_ == TimelineMode::Rewrite) {
        if (pt && !anchor_) anchor_ = *pt;   // no service anchor (tests, or a lone asset): this MPU defines the origin
        if (pt && anchor_) {
            const double rel = ntpDeltaSeconds(*anchor_, *pt) - offset_s_.value_or(0.0);
            if (!shift_known_) {
                // Fragments emitted before any descriptor (join time, or an encoder without descriptors) sit on a local
                // timeline from 0; the first descriptor must not move the next fragment back onto them: shift the
                // presentation clock so this fragment continues where the local timeline ended.
                const double cont_s = (last_tfdt_out_ && last_duration_) ? static_cast<double>(*last_tfdt_out_ + *last_duration_) / static_cast<double>(ts) : 0.0;
                rewrite_shift_s_ = last_tfdt_out_ ? rel - cont_s : 0.0;
                shift_known_ = true;
            }
            const double placed = rel - rewrite_shift_s_;
            tfdt_out = placed > 0 ? static_cast<std::uint64_t>(placed * static_cast<double>(ts) + 0.5) : 0;
        } else if (last_tfdt_out_ && last_duration_) {
            tfdt_out = *last_tfdt_out_ + *last_duration_;
            ++stats_.fragments_no_pt;
        } else {
            tfdt_out = 0;   // first fragment without any descriptor: local origin, reconciled above when one arrives
            ++stats_.fragments_no_pt;
        }
        rewritten = tfdt_out != tfdt_received;
        if (rewritten) ++stats_.tfdt_rewrites;
    }

    media::BuildFragmentOptions opts;
    if (init_->hint_track_id) opts.drop_track_ids.insert(*init_->hint_track_id);
    opts.media_track_id = init_->media.track_id;
    if (rewritten) opts.tfdt = tfdt_out;
    opts.prepend_styp = true;
    media::BuildFragmentResult res;
    std::string err;
    if (!media::buildFragment(ByteView(m.moof_du.data(), m.moof_du.size()), views, opts, res, &err)) {
        ++stats_.build_errors;
        dropMpu(m, "build failed");
        return;
    }
    auto f = std::make_shared<MpuFragment>();
    f->packet_id = packet_id_;
    f->mpu_sequence_number = m.seq;
    f->movie_fragment_sequence_number = m.moof->sequence_number;
    f->bytes = std::move(res.bytes);
    f->timescale = ts;
    f->tfdt_received = tfdt_received;
    f->tfdt = tfdt_out;
    f->tfdt_rewritten = rewritten;
    f->duration = traf->total_duration;
    f->sample_count = static_cast<std::uint32_t>(n);
    f->samples_missing = missing;
    f->damaged = missing != 0;
    f->discontinuity = pending_discontinuity_ || (last_resolved_seq_ && m.seq != *last_resolved_seq_ + 1u);
    f->restart = pending_restart_;
    f->presentation_ntp = pt;
    f->received_ns = m.first_ns;
    pending_discontinuity_ = false;
    pending_restart_ = false;
    markResolved(m.seq);
    last_tfdt_received_ = tfdt_received;
    last_tfdt_out_ = tfdt_out;
    last_duration_ = traf->total_duration;
    ++stats_.fragments;
    if (missing) ++stats_.fragments_damaged;
    release(m);
    if (cb_.onFragment) cb_.onFragment(f);
}

void MpuAssembler::resolve(Mpu& m) {
    // an MPU that can no longer complete: emit it with zero-filled samples when it has its moof (and sample 1), else drop
    if (m.moof && init_) {
        emit(m, true);
    } else if (!m.samples.empty() || !m.moof_du.empty()) {
        dropMpu(m, "incomplete");
    } else {
        markResolved(m.seq);
        release(m);
    }
}

void MpuAssembler::drain() {
    // Emission stays in MPU order: the oldest pending MPU goes first. On air MPUs of a packet_id are sequential, so
    // once a newer MPU is complete an older incomplete one will not receive anything more and is resolved at once.
    while (!mpus_.empty()) {
        Mpu& front = mpus_.front();
        if (complete(front)) {
            emit(front, false);
            mpus_.pop_front();
            continue;
        }
        bool newer_complete = false;
        for (std::size_t i = 1; i < mpus_.size() && !newer_complete; ++i) newer_complete = complete(mpus_[i]);
        if (!newer_complete) break;
        resolve(front);
        mpus_.pop_front();
    }
}

void MpuAssembler::evictCount() {
    while (mpus_.size() > cfg_.max_pending_mpus) {
        resolve(mpus_.front());
        mpus_.pop_front();
    }
}

void MpuAssembler::evictBytes() {
    // the byte cap holds regardless of how many MPUs are pending: resolve from the oldest until it does
    while (stats_.pending_bytes > cfg_.max_pending_bytes && !mpus_.empty()) {
        resolve(mpus_.front());
        mpus_.pop_front();
    }
}

void MpuAssembler::flush() {
    dropPendingDu();
    while (!mpus_.empty()) {
        if (complete(mpus_.front())) emit(mpus_.front(), false);
        else resolve(mpus_.front());
        mpus_.pop_front();
    }
    stats_.pending_bytes = 0;
}

}  // namespace atsc3::mmtp
