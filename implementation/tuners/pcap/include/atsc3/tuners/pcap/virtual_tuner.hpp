#pragma once
// Pcap virtual tuner (spec 5.1.2): a service::ITuner over a capture file, replacing an RF tuner for tests and PCs
// without RF hardware. It delivers the one kind of unit the capture holds (BBP for STLTP and BBP containers, ALP for
// DLT 289, IP for raw-IP captures) with real-time or non-real-time pacing, pause, loop and a time window.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "atsc3/tuners/pcap/format_detector.hpp"
#include "atsc3/tuners/pcap/sidecar_xa3.hpp"
#include "atsc3/io/units.hpp"
#include "atsc3/service/tuner.hpp"

namespace atsc3::stltp {
class Demux;
struct Stats;
}

namespace atsc3::pcap {

enum class Pacing { RealTime, NonRealTime };

struct IpFilter {
    std::uint32_t dst = 0;     // host order
    std::uint16_t port = 0;    // 0 = any
};

struct VirtualTunerConfig {
    std::string path;
    Pacing pacing = Pacing::RealTime;
    double speed = 1.0;                                  // real-time only
    std::int64_t max_gap_ns = 2'000'000'000;             // clamp for gaps between records
    bool loop = false;
    std::int64_t start_offset_ns = 0;                    // skip this much capture time
    std::int64_t duration_ns = 0;                        // 0 = until end of file
    std::uint64_t max_records = 0;                       // stop after this many delivered records (0 = all)
    std::optional<CaptureFormat> force_mode;
    bool allow_mixed = false;
    DetectorConfig detector;
    std::vector<IpFilter> ip_filter;                     // empty = all UDP datagrams
    std::string sidecar = "auto";                        // "auto" | path | "off"
    bool descramble = false;                             // BBP mode: A/322 §5.2.3 (not implemented yet)
    bool use_rtp_timing = false;                         // STLTP: refine pacing with inner RTP timestamps (not yet)
};

class VirtualTuner final : public service::ITuner {
public:
    struct Stats {
        std::uint64_t records = 0, delivered_bbp = 0, delivered_alp = 0, delivered_ip = 0;
        std::uint64_t dropped_non_ip = 0, dropped_non_udp = 0, dropped_filtered = 0, dropped_mixed = 0;
        std::uint64_t dropped_other_interface = 0, time_jumps = 0, wraps = 0;
    };

    explicit VirtualTuner(VirtualTunerConfig cfg);
    ~VirtualTuner() override;

    /// Result of opening/probing the file (done in the constructor).
    service::TunerError openError() const noexcept { return open_error_; }
    const std::string& openMessage() const noexcept { return open_message_; }
    const Detection& detection() const noexcept { return detection_; }
    const std::optional<SidecarXa3>& sidecar() const noexcept { return sidecar_; }
    CaptureFormat mode() const noexcept { return mode_; }

    // service::ITuner
    std::string name() const override { return "pcap"; }
    std::string source() const override { return cfg_.path; }
    /// The one kind the capture holds; empty until a capture is open.
    std::set<service::TunerOutput> outputs() const override;
    service::TunerError start(service::ITunerSink& sink, service::TunerOutput output) override;
    void stop() override;
    std::optional<io::L1Info> l1() const override;
    service::TunerState state() const override;
    /// Freezes/unfreezes delivery and the real-time clock together (spec 5.19.3 pause semantics).
    bool pause(bool paused) override;

    /// The kind of unit this capture delivers (the single member of outputs()).
    service::TunerOutput output() const noexcept { return kind_; }
    const VirtualTunerConfig& config() const noexcept { return cfg_; }
    service::SignalStats signal() const;
    bool paused() const noexcept { return paused_.load(); }
    /// Blocks until EndOfStream was delivered or stop() was called; false on timeout.
    bool waitFinished(std::chrono::milliseconds timeout);
    bool finished() const noexcept { return finished_.load(); }

    Stats stats() const;
    /// Capture position of the last delivered record relative to the first record (ns).
    std::int64_t positionNs() const noexcept { return position_ns_.load(); }
    /// Counters of the embedded STLTP demultiplexer (STLTP captures only; empty before start()).
    std::optional<stltp::Stats> stltpStats() const;

private:
    void probe();
    void run(service::ITunerSink& sink);
    bool deliverRecord(service::ITunerSink& sink, const PcapRecord& rec, io::UnitTime time);
    bool waitForDeadline(std::chrono::steady_clock::time_point deadline, std::int64_t& paused_ns);
    bool ipAllowed(std::uint32_t dst, std::uint16_t port) const;
    void emit(service::ITunerSink& sink, service::TunerEventType type, std::string message, std::int64_t value = 0);

    VirtualTunerConfig cfg_;
    std::unique_ptr<PcapReader> reader_;
    Detection detection_;
    CaptureFormat mode_ = CaptureFormat::Unsupported;
    service::TunerOutput kind_ = service::TunerOutput::Ip;
    service::TunerError open_error_ = service::TunerError::NotOpen;
    std::string open_message_;
    std::optional<SidecarXa3> sidecar_;
    std::uint8_t ip_plp_ = io::kUnknownPlp;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> tunnel_flows_;

    std::unique_ptr<stltp::Demux> demux_;
    service::ITunerSink* sink_ = nullptr;
    io::UnitTime current_time_;  // of the record being delivered (used by demux callbacks)

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> finished_{false};
    std::atomic<std::int64_t> position_ns_{0};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    Stats stats_;
    std::vector<std::uint8_t> plps_seen_;
};

}  // namespace atsc3::pcap
