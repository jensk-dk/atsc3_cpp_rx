#pragma once
// MPU reconstruction (spec 5.10.3; ISO/IEC 23008-1 §9.3.2 MPU mode, A/331 §8.1.2.2): per packet_id, reassembles the
// MPU-mode data units (fragmentation and aggregation), turns FT=0 MPU metadata into an MSE initialization segment,
// collects the FT=2 MFUs of an MPU by sample number and, once the FT=1 movie fragment metadata is known, emits one
// MSE media segment (styp + moof + mdat) per movie fragment. Timeline per asset: tfdt kept when it increases across
// MPUs, rewritten from the MPU_timestamp_descriptor when it restarts (both behaviours are on air; spec 5.10.3
// "Observed"). All work happens on the caller's thread; nothing is shared.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/media/isobmff.hpp"
#include "atsc3/mmtp/packet.hpp"

namespace atsc3::mmtp {

struct MpuInitSegment {
    std::uint16_t packet_id = 0;
    std::uint32_t mpu_sequence_number = 0;     // the MPU whose metadata produced it
    std::vector<std::uint8_t> bytes;           // MSE-clean: ftyp + moov with the MMT hint track removed
    std::vector<std::uint8_t> raw;             // the DU as received (ftyp mmpu moov ...) when MpuAssemblerConfig::keep_raw_init
    media::InitSegmentInfo info;               // of the received DU: all tracks, mmpu fields
    media::TrackInfo media;                    // the media track (codecs string from its sample entry)
    std::optional<std::uint32_t> hint_track_id;
    std::uint32_t hint_sample_size = 0;        // hint trex default_sample_size (34 on air: the MMTHSample prefix of every MFU)
    std::int64_t received_ns = 0;
};

struct MpuFragment {
    std::uint16_t packet_id = 0;
    std::uint32_t mpu_sequence_number = 0;
    std::uint32_t movie_fragment_sequence_number = 1;   // mfhd.sequence_number
    std::vector<std::uint8_t> bytes;           // styp + moof' + mdat' (media samples only)
    std::uint32_t timescale = 0;
    std::uint64_t tfdt_received = 0;           // baseMediaDecodeTime as sent
    std::uint64_t tfdt = 0;                    // baseMediaDecodeTime in `bytes`
    bool tfdt_rewritten = false;
    std::uint64_t duration = 0;                // sum of the trun sample durations (ticks)
    std::uint32_t sample_count = 0;
    std::uint32_t samples_missing = 0;         // zero-filled samples
    bool damaged = false;                      // samples_missing > 0
    bool discontinuity = false;                // an MPU of this asset was dropped (or skipped) right before this one
    bool restart = false;                      // the encoder restarted (MPU numbering jumped back): timeline starts over
    std::optional<std::uint64_t> presentation_ntp;   // MPU_timestamp_descriptor of this MPU sequence number
    std::int64_t received_ns = 0;              // completion time (stack clock)
};

struct MpuAssemblerConfig {
    std::size_t max_pending_mpus = 3;          // incomplete MPUs kept per packet_id; the oldest is flushed beyond this
    std::size_t max_pending_bytes = 48u << 20; // sample bytes held for incomplete MPUs (plus 64 B per stored sample); also bounds the zero-fill
    bool keep_raw_init = false;
    bool drop_without_first_sample = true;     // an MPU missing sample 1 (the IDR) is dropped instead of zero-filled
};

struct MpuAssemblerStats {
    std::uint64_t packets = 0, dus = 0, du_fragments = 0, du_drops = 0, aggregated_dus = 0, non_timed = 0, unknown_ft = 0;
    std::uint64_t mpus = 0, mpu_gaps = 0, init_segments = 0, init_repeats = 0, init_errors = 0;
    std::uint64_t fragments = 0, fragments_damaged = 0, fragments_dropped = 0, fragments_no_pt = 0 /* emitted by continuity, no descriptor */, samples_zero_filled = 0, samples_late = 0;
    std::uint64_t samples_rejected = 0, samples_size_mismatch = 0, hint_prefix_missing = 0, tfdt_rewrites = 0, build_errors = 0, mpu_restarts = 0;
    std::size_t pending_bytes = 0;
};

struct MpuAssemblerCallbacks {
    std::function<void(std::shared_ptr<const MpuInitSegment>)> onInit;        // a new or changed init segment
    std::function<void(std::shared_ptr<const MpuFragment>)> onFragment;       // one per movie fragment, in MPU order
};

/// Timeline mode of an asset (decided at the first movie fragment, may switch to Rewrite later).
enum class TimelineMode { Undecided, Keep, Rewrite };

class MpuAssembler {
public:
    MpuAssembler(std::uint16_t packet_id, MpuAssemblerCallbacks cb, MpuAssemblerConfig cfg = {});

    /// One MPU-mode packet of this packet_id (headers parsed by the session). `t_ns` is the stack clock.
    void onPacket(const PacketHeader& h, const MpuPayloadHeader& m, std::int64_t t_ns);
    /// MPU_timestamp_descriptor entry for this asset (from the MP table or its subset tables).
    void setPresentationTime(std::uint32_t mpu_sequence_number, std::uint64_t ntp);
    /// Service-wide timeline origin (NTP64): rewritten tfdt = (pt - anchor) * timescale. Set once per service.
    void setAnchor(std::uint64_t ntp);
    std::optional<std::uint64_t> anchor() const noexcept { return anchor_; }
    /// Emits every pending MPU that has a moof (zero-filling missing samples) and drops the rest.
    void flush();

    std::uint16_t packetId() const noexcept { return packet_id_; }
    const MpuAssemblerStats& stats() const noexcept { return stats_; }
    std::shared_ptr<const MpuInitSegment> init() const noexcept { return init_; }
    TimelineMode timelineMode() const noexcept { return mode_; }
    /// Constant SourceBuffer.timestampOffset (seconds) that places this asset on the service timeline, frozen at the
    /// first emitted fragment: Keep mode -> (pt_first - anchor) - tfdt_first / timescale (0 without a presentation
    /// time), Rewrite mode -> 0. It never changes afterwards (a Keep->Rewrite switch rewrites tfdt relative to it).
    /// nullopt before the first fragment.
    std::optional<double> timestampOffsetSeconds() const noexcept;
    std::optional<std::uint64_t> presentationTime(std::uint32_t mpu_sequence_number) const;

private:
    struct PendingDu {                         // one DU being reassembled (fragments are consecutive on a packet_id)
        bool active = false;
        std::uint32_t mpu_seq = 0;
        std::uint8_t ft = 0;
        bool timed = true;
        std::uint32_t last_pseq = 0;
        std::size_t fragments = 0;
        std::vector<std::uint8_t> head;        // the 14-byte MFU DU header of the first fragment (FT=2)
        std::vector<std::uint8_t> bytes;       // DU header + payload so far
    };
    struct Mpu {                               // always built in place (emplace): moving a fresh Mpu, whose `moof`
        Mpu(std::uint32_t s, std::int64_t t) noexcept : seq(s), first_ns(t) {}   // is disengaged, trips GCC 11's
        std::uint32_t seq = 0;                 // -Wmaybe-uninitialized at -O3 (the release preset builds with -Werror)
        std::int64_t first_ns = 0;
        std::vector<std::uint8_t> moof_du;     // moof + mdat header as received
        std::optional<media::MoofInfo> moof;
        std::map<std::uint32_t, std::vector<std::uint8_t>> samples;   // by sample number (1-based)
        std::size_t sample_bytes = 0;          // payload bytes plus a fixed per-sample overhead
    };

    void onDu(std::uint32_t mpu_seq, std::uint8_t ft, bool timed, ByteView du, std::int64_t t_ns);
    void onInitDu(std::uint32_t mpu_seq, ByteView du, std::int64_t t_ns);
    void onMoofDu(Mpu& mpu, ByteView du);
    void onMfu(Mpu& mpu, ByteView du);
    Mpu& mpuFor(std::uint32_t seq, std::int64_t t_ns);
    bool complete(const Mpu& m) const;
    void emit(Mpu& m, bool forced);
    void resolve(Mpu& m);
    void drain();
    void dropMpu(Mpu& m, const char* why);
    void release(Mpu& m);
    void markResolved(std::uint32_t seq);
    std::int32_t deltaFromResolved(std::uint32_t seq) const noexcept;
    void restart(std::uint32_t seq);
    void dropPendingDu();
    void evictCount();
    void evictBytes();

    std::uint16_t packet_id_;
    MpuAssemblerCallbacks cb_;
    MpuAssemblerConfig cfg_;
    MpuAssemblerStats stats_;
    PendingDu pending_;
    std::deque<Mpu> mpus_;                     // in order of first appearance
    std::shared_ptr<const MpuInitSegment> init_;
    std::vector<std::uint8_t> init_key_;       // the received DU minus its mmpu box, to detect real changes
    std::map<std::uint32_t, std::uint64_t> pts_;   // mpu_sequence_number -> NTP64 (bounded)
    std::optional<std::uint64_t> anchor_;
    std::optional<std::uint32_t> last_resolved_seq_;   // highest MPU emitted or dropped; older DUs are late
    bool pending_discontinuity_ = false, pending_restart_ = false;
    TimelineMode mode_ = TimelineMode::Undecided;
    std::optional<std::uint64_t> last_tfdt_received_, last_tfdt_out_, last_duration_;
    std::optional<std::uint64_t> first_pt_, first_tfdt_;
    std::optional<double> offset_s_;           // frozen SourceBuffer offset (see timestampOffsetSeconds)
    double rewrite_shift_s_ = 0.0;             // Rewrite mode: presentation clock minus the local timeline of fragments emitted before the first descriptor
    bool shift_known_ = false;
    std::uint32_t timescale_ = 0;
};

/// NTP64 -> seconds since the Unix epoch (fraction included).
double ntpToUnixSeconds(std::uint64_t ntp) noexcept;
/// Difference b - a of two NTP64 timestamps in seconds.
double ntpDeltaSeconds(std::uint64_t a, std::uint64_t b) noexcept;

}  // namespace atsc3::mmtp
