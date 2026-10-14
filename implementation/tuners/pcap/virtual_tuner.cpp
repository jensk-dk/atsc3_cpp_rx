#include "atsc3/tuners/pcap/virtual_tuner.hpp"

#include <algorithm>
#include <sys/stat.h>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/stltp/demux.hpp"
#include "link_strip.hpp"

namespace atsc3::pcap {

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

VirtualTuner::VirtualTuner(VirtualTunerConfig cfg) : cfg_(std::move(cfg)) { probe(); }

VirtualTuner::~VirtualTuner() { stop(); }

void VirtualTuner::probe() {
    struct stat st {};
    if (::stat(cfg_.path.c_str(), &st) != 0) {
        open_error_ = service::TunerError::FileNotFound;
        open_message_ = "cannot open " + cfg_.path;
        return;
    }
    std::string err;
    reader_ = PcapReader::open(cfg_.path, &err);
    if (!reader_) {
        open_error_ = service::TunerError::BadFile;
        open_message_ = err;
        return;
    }
    detection_ = detectCaptureFormat(*reader_, cfg_.detector);
    mode_ = cfg_.force_mode ? *cfg_.force_mode : detection_.format;

    // Sidecar (DekTec .xa3): explicit path, auto lookup, or off.
    if (cfg_.sidecar != "off") {
        std::optional<std::string> path;
        if (cfg_.sidecar == "auto") path = findXa3Sidecar(cfg_.path);
        else path = cfg_.sidecar;
        if (path) {
            std::string serr;
            sidecar_ = loadXa3(*path, &serr);
            if (!sidecar_) open_message_ += "sidecar ignored: " + serr + "; ";
        }
    }
    if (sidecar_) {
        if (const auto plp = sidecar_->plpForCapture(cfg_.path)) ip_plp_ = *plp;
    }

    switch (mode_) {
        case CaptureFormat::Alp: kind_ = service::TunerOutput::Alp; break;
        case CaptureFormat::Bbp: kind_ = service::TunerOutput::Bbp; break;
        case CaptureFormat::IpBypass: kind_ = service::TunerOutput::Ip; break;
        case CaptureFormat::Stltp: {
            const std::uint8_t pt = detection_.tunnel_pt.value_or(stltp::kPtStltp);
            if (pt == stltp::kPtStltp) kind_ = service::TunerOutput::Bbp;
            else if (pt == stltp::kPtAlptp) kind_ = service::TunerOutput::Alp;
            else {
                mode_ = CaptureFormat::Unsupported;
                open_message_ += "DSTP (PT 81) tunnels are not supported; ";
            }
            for (const FlowEvidence& f : detection_.flows) {
                if (f.cls == FlowEvidence::Class::Tunnel) tunnel_flows_.emplace_back(f.dst, f.port);
            }
            break;
        }
        default: break;
    }
    if (mode_ == CaptureFormat::Unsupported) {
        open_error_ = service::TunerError::NoAtscContent;
        open_message_ += detection_.summary();
        return;
    }
    open_error_ = service::TunerError::None;
    open_message_ += detection_.summary();
    if (sidecar_) open_message_ += " sidecar=" + sidecar_->path;
}

std::optional<io::L1Info> VirtualTuner::l1() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (sidecar_) return sidecar_->l1;
    if (mode_ == CaptureFormat::Stltp || mode_ == CaptureFormat::Bbp) {
        io::L1Info info;
        info.source = mode_ == CaptureFormat::Stltp ? "stltp" : "bbp-container";
        for (std::uint8_t p : plps_seen_) {
            io::L1Info::Plp plp;
            plp.plp_id = p;
            info.plps.push_back(plp);
        }
        return info;
    }
    return std::nullopt;
}

std::optional<stltp::Stats> VirtualTuner::stltpStats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!demux_) return std::nullopt;
    return demux_->stats();
}

service::SignalStats VirtualTuner::signal() const {
    service::SignalStats s;
    s.locked = running_.load() && !finished_.load();
    s.snr_db = 99.0;
    return s;
}

VirtualTuner::Stats VirtualTuner::stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

std::set<service::TunerOutput> VirtualTuner::outputs() const {
    if (open_error_ != service::TunerError::None) return {};
    return {kind_};
}

service::TunerState VirtualTuner::state() const {
    service::TunerState s;
    s.ready = open_error_ == service::TunerError::None;
    if (!s.ready) s.error = open_message_;
    s.paused = paused_.load();
    s.finished = finished_.load();
    s.position_ns = position_ns_.load();
    s.signal = signal();
    return s;
}

service::TunerError VirtualTuner::start(service::ITunerSink& sink, service::TunerOutput output) {
    if (open_error_ != service::TunerError::None) return open_error_;
    if (output != kind_) return service::TunerError::UnsupportedOutput;
    if (running_.load()) return service::TunerError::AlreadyStarted;
    if (thread_.joinable()) thread_.join();
    finished_.store(false);
    running_.store(true);
    sink_ = &sink;
    thread_ = std::thread([this, &sink] { run(sink); });
    return service::TunerError::None;
}

void VirtualTuner::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        running_.store(false);
        paused_.store(false);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool VirtualTuner::pause(bool p) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        paused_.store(p);
    }
    cv_.notify_all();
    return true;
}

bool VirtualTuner::waitFinished(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mtx_);
    return cv_.wait_for(lock, timeout, [this] { return finished_.load() || !running_.load(); });
}

void VirtualTuner::emit(service::ITunerSink& sink, service::TunerEventType type, std::string message, std::int64_t value) {
    service::TunerEvent ev;
    ev.type = type;
    ev.message = std::move(message);
    ev.value_ns = value;
    sink.onTunerEvent(ev);
}

bool VirtualTuner::ipAllowed(std::uint32_t dst, std::uint16_t port) const {
    if (cfg_.ip_filter.empty()) return true;
    for (const IpFilter& f : cfg_.ip_filter) {
        if (f.dst == dst && (f.port == 0 || f.port == port)) return true;
    }
    return false;
}

// Waits until the deadline (real-time mode); honours pause (freezing the timeline) and stop.
// Time spent paused is added to paused_ns so the caller can shift the host anchor.
// Returns false when stopped.
bool VirtualTuner::waitForDeadline(Clock::time_point deadline, std::int64_t& paused_ns) {
    std::unique_lock<std::mutex> lock(mtx_);
    for (;;) {
        if (!running_.load()) return false;
        if (paused_.load()) {
            const Clock::time_point t0 = Clock::now();
            cv_.wait(lock, [this] { return !paused_.load() || !running_.load(); });
            const auto spent = Clock::now() - t0;
            paused_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count();
            deadline += spent;
            continue;
        }
        if (Clock::now() >= deadline) return true;
        cv_.wait_until(lock, deadline, [this] { return !running_.load() || paused_.load(); });
    }
}

void VirtualTuner::run(service::ITunerSink& sink) {
    if (mode_ == CaptureFormat::Stltp) {
        stltp::Callbacks cb;
        cb.onBbp = [this](std::uint8_t plp, ByteView bbp) {
            io::BasebandPacket p;
            p.plp_id = plp;
            p.time = current_time_;
            p.bytes = bbp;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.delivered_bbp;
                if (std::find(plps_seen_.begin(), plps_seen_.end(), plp) == plps_seen_.end()) plps_seen_.push_back(plp);
            }
            sink_->onBbp(p);
        };
        cb.onAlp = [this](std::uint8_t plp, std::uint8_t, ByteView alp) {
            io::AlpPacket p;
            p.plp_id = plp;
            p.time = current_time_;
            p.bytes = alp;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.delivered_alp;
            }
            sink_->onAlp(p);
        };
        demux_ = std::make_unique<stltp::Demux>(std::move(cb));
    }
    emit(sink, service::TunerEventType::FormatDetected,
         std::string("mode=") + toString(mode_) + (cfg_.force_mode ? " (forced) " : " ") + detection_.summary() +
             (sidecar_ ? " sidecar=" + sidecar_->path : std::string()));
    emit(sink, service::TunerEventType::Lock, "virtual tuner locked");

    const bool realtime = cfg_.pacing == Pacing::RealTime;
    const double speed = cfg_.speed > 0.0 ? cfg_.speed : 1.0;
    PcapRecord rec;
    bool have_first = false;
    std::int64_t first_ts = 0;        // first record timestamp of the file
    std::int64_t prev_ts = 0;         // previous delivered record timestamp
    std::int64_t data_time = 0;       // clamped, monotonic capture time since first record
    Clock::time_point host_start = Clock::now();

    for (;;) {
        if (!running_.load()) break;
        if (!reader_->next(rec)) {
            if (reader_->truncated()) emit(sink, service::TunerEventType::Error, "truncated record at end of file");
            if (cfg_.loop && running_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    ++stats_.wraps;
                }
                emit(sink, service::TunerEventType::Wrap, "capture wrapped", data_time);
                reader_->rewind();
                if (demux_) demux_->reset();
                have_first = false;  // timeline continues without a gap (see below)
                continue;
            }
            break;
        }
        if (!have_first) {
            first_ts = rec.ts_ns;
            prev_ts = rec.ts_ns;
            have_first = true;
            // After a wrap: keep data_time, re-anchor the host clock so the next record is "now".
            host_start = Clock::now() - std::chrono::nanoseconds(static_cast<std::int64_t>(
                                            static_cast<double>(data_time) / speed));
        }
        if (cfg_.start_offset_ns > 0 && rec.ts_ns - first_ts < cfg_.start_offset_ns) continue;
        if (cfg_.duration_ns > 0 && rec.ts_ns - first_ts > cfg_.start_offset_ns + cfg_.duration_ns) break;

        std::int64_t gap = rec.ts_ns - prev_ts;
        prev_ts = rec.ts_ns;
        if (gap < 0) gap = 0;
        if (gap > cfg_.max_gap_ns) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.time_jumps;
            }
            emit(sink, service::TunerEventType::TimeJump, "clamped inter-record gap", gap);
            gap = cfg_.max_gap_ns;
        }
        data_time += gap;

        io::UnitTime time;
        time.capture_ns = rec.ts_ns;
        if (realtime) {
            const auto deadline = host_start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                                   static_cast<double>(data_time) / speed));
            std::int64_t paused_ns = 0;
            if (!waitForDeadline(deadline, paused_ns)) break;
            // Time spent paused shifts the host anchor so the timeline stays frozen (no catch-up burst).
            if (paused_ns) host_start += std::chrono::nanoseconds(paused_ns);
            const auto delivered_at = host_start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                                       static_cast<double>(data_time) / speed));
            time.t_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delivered_at.time_since_epoch()).count();
        } else {
            // Non-real-time: data time on an artificial monotonic base (first record timestamp).
            time.t_ns = first_ts + data_time;
            if (!running_.load()) break;
            if (paused_.load()) {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] { return !paused_.load() || !running_.load(); });
                if (!running_.load()) break;
            }
        }
        position_ns_.store(data_time);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (cfg_.max_records && stats_.records >= cfg_.max_records) break;
            ++stats_.records;
        }
        if (!deliverRecord(sink, rec, time)) break;
    }
    const bool stopped = !running_.load();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        finished_.store(true);
        running_.store(false);
    }
    if (!stopped) emit(sink, service::TunerEventType::EndOfStream, "end of capture", data_time);
    cv_.notify_all();
}

bool VirtualTuner::deliverRecord(service::ITunerSink& sink, const PcapRecord& rec, io::UnitTime time) {
    current_time_ = time;
    const ByteView frame(rec.data.data(), rec.data.size());
    switch (mode_) {
        case CaptureFormat::Alp: {
            if (rec.link_type != linktype::kAtscAlp) {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.dropped_other_interface;
                return true;
            }
            io::AlpPacket p;
            p.plp_id = ip_plp_;
            p.time = time;
            p.bytes = frame;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.delivered_alp;
            }
            sink.onAlp(p);
            return true;
        }
        case CaptureFormat::Bbp: {
            BbpContainerRecord b;
            if (!linktype::isUser(rec.link_type) || !parseBbpContainerRecord(frame, b)) {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.dropped_other_interface;
                return true;
            }
            io::BasebandPacket p;
            p.plp_id = b.plp_id;
            p.error = (b.flags & kBbpFlagError) != 0;
            p.time = time;
            p.bytes = b.bbp;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.delivered_bbp;
                if (std::find(plps_seen_.begin(), plps_seen_.end(), b.plp_id) == plps_seen_.end())
                    plps_seen_.push_back(b.plp_id);
            }
            sink.onBbp(p);
            return true;
        }
        case CaptureFormat::Stltp:
        case CaptureFormat::IpBypass: {
            ByteView ip;
            if (!linktype::isIpBearing(rec.link_type) || !stripToIpv4(rec.link_type, frame, ip)) {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.dropped_non_ip;
                return true;
            }
            io::Ipv4Udp h;
            if (!io::parseIpv4Udp(ip, h) || !h.udp) {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.dropped_non_udp;
                return true;
            }
            if (mode_ == CaptureFormat::Stltp) {
                const bool tunnel = std::find(tunnel_flows_.begin(), tunnel_flows_.end(),
                                              std::make_pair(h.dst, h.dport)) != tunnel_flows_.end();
                if (tunnel) {
                    demux_->feedTunnelPacket(h.payload);
                    return true;
                }
                if (!cfg_.allow_mixed) {
                    std::lock_guard<std::mutex> lock(mtx_);
                    ++stats_.dropped_mixed;
                    return true;
                }
            }
            if (!ipAllowed(h.dst, h.dport)) {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.dropped_filtered;
                return true;
            }
            io::IpDatagram d;
            d.plp_id = ip_plp_;
            d.time = time;
            d.bytes = ip;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                ++stats_.delivered_ip;
            }
            sink.onIp(d);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace atsc3::pcap
