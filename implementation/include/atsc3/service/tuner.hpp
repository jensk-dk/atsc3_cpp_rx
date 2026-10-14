#pragma once
// Tuner contract (spec 5.1): the engine's view of any source, real or virtual. A tuner delivers one kind of unit —
// Baseband Packets, ALP packets or IP datagrams — and the engine couples it to the receive chain at that layer
// (BBP → deframer → ALP decap → link signaling / IP demux; ALP → decap → …; IP → demux only). An implementation
// states every output it can deliver; the engine chooses one when it opens the tuner (OpenOptions::output, else the
// highest layer offered) and the tuner delivers exactly that kind on its own thread until stop().
#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "atsc3/io/units.hpp"

namespace atsc3::service {

/// The layer at which a tuner couples to the stack.
enum class TunerOutput { Bbp, Alp, Ip };
const char* toString(TunerOutput o) noexcept;

enum class TunerEventType { FormatDetected, Lock, Unlock, TimeJump, Wrap, EndOfStream, Error };

struct TunerEvent {
    TunerEventType type = TunerEventType::Error;
    std::string message;
    std::int64_t value_ns = 0;  // TimeJump: real gap; EndOfStream/Wrap: capture position
};

enum class TunerError { None, NoAtscContent, FileNotFound, BadFile, AlreadyStarted, NotOpen, UnsupportedOutput, Driver };
const char* toString(TunerError e) noexcept;

struct SignalStats {
    bool locked = false;
    double snr_db = 0.0;
    double ber = 0.0;
};

/// Generic state every tuner reports. Replay tuners fill position/duration/finished, RF tuners lock and signal.
struct TunerState {
    bool ready = false;              // capture open / demodulator usable; `error` explains when false
    std::string error;
    bool paused = false;
    bool finished = false;           // a replay reached its end (never set by an RF tuner)
    std::int64_t position_ns = 0;    // replay position since the first unit; 0 for RF
    std::int64_t duration_ns = 0;    // replay length when known, else 0
    SignalStats signal;
};

/// Implemented by the engine (or a test recorder). Called on the tuner's delivery thread; the tuner calls only the
/// method of the output it was started with.
class ITunerSink {
public:
    virtual ~ITunerSink() = default;
    virtual void onBbp(const io::BasebandPacket&) = 0;
    virtual void onAlp(const io::AlpPacket&) = 0;
    virtual void onIp(const io::IpDatagram&) = 0;
    virtual void onTunerEvent(const TunerEvent&) = 0;
};

/// Implemented per demodulator driver (tuners/<vendor>) and by the pcap virtual tuner (tuners/pcap).
class ITuner {
public:
    virtual ~ITuner() = default;
    /// Implementation name, e.g. "pcap", "sony".
    virtual std::string name() const = 0;
    /// What it is tuned to: a capture path, an RF frequency, a multicast address.
    virtual std::string source() const = 0;
    /// Every output the implementation can deliver (known once the source is open).
    virtual std::set<TunerOutput> outputs() const = 0;
    /// Starts delivering `output` to `sink` on the tuner's thread. UnsupportedOutput when it is not in outputs().
    virtual TunerError start(ITunerSink& sink, TunerOutput output) = 0;
    /// Stops delivery and joins the thread; safe to call when not started.
    virtual void stop() = 0;
    /// Physical-layer facts (L1 signaling, a sidecar) when known.
    virtual std::optional<io::L1Info> l1() const = 0;
    virtual TunerState state() const = 0;
    /// Freezes or resumes delivery; false when the implementation cannot pause (RF tuners).
    virtual bool pause(bool) { return false; }
};

}  // namespace atsc3::service
