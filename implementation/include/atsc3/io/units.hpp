#pragma once
// Delivery units and physical-layer facts shared by the tuner contract (atsc3/service/tuner.hpp) and the
// layers that consume them (spec 5.1): a tuner delivers Baseband Packets, ALP packets or IP datagrams.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::io {

/// PLP id used when the source cannot know it (DLT 289 captures without sidecar, plain IP).
constexpr std::uint8_t kUnknownPlp = 0xFF;

/// Common timing of every delivered unit:
///  - t_ns: stack monotonic time at which the unit is (or should have been) delivered;
///  - capture_ns: original capture timestamp (ns since the Unix epoch) when known, else 0.
struct UnitTime {
    std::int64_t t_ns = 0;
    std::int64_t capture_ns = 0;
};

struct BasebandPacket {
    std::uint8_t plp_id = kUnknownPlp;
    bool error = false;
    UnitTime time;
    ByteView bytes;  // complete BBP: header + payload, descrambled
};

struct AlpPacket {
    std::uint8_t plp_id = kUnknownPlp;
    UnitTime time;
    ByteView bytes;  // complete ALP packet: header + payload
};

struct IpDatagram {
    std::uint8_t plp_id = kUnknownPlp;
    UnitTime time;
    ByteView bytes;  // starts at the IPv4 header
};

/// One LMT multicast entry (A/330 Table 7.2), also used for sidecar-synthesized LMTs.
struct LmtEntry {
    std::uint8_t plp_id = 0;
    std::uint32_t src_ip = 0;  // host order; 0 = wildcard
    std::uint32_t dst_ip = 0;
    std::uint16_t src_port = 0;
    std::uint16_t dst_port = 0;
};

enum class LmtOrigin { Broadcast, Sidecar };

/// What is known about the physical layer: from L1 signaling (STLTP preamble, RF adapter) or a sidecar.
struct L1Info {
    struct Plp {
        std::uint8_t plp_id = 0;
        bool lls_flag = false;
        std::string modulation;      // e.g. "qam256"
        std::string code_rate;       // e.g. "cod_9_15"
        std::string fec_code_length; // e.g. "ldpc_64k"
        std::string fec_outer_code;  // e.g. "bch"
        std::string layer;           // "core" / "enhanced"
        std::string plp_type;
    };
    std::vector<Plp> plps;
    bool lls_flag = false;
    int minor_version = -1;
    std::string bandwidth, fft_size, guard_interval, pilot_pattern;
    std::vector<LmtEntry> lmt;                 // synthesized (sidecar) or decoded entries
    LmtOrigin lmt_origin = LmtOrigin::Sidecar;
    std::string source;                        // e.g. sidecar file path or "stltp-preamble"
};

}  // namespace atsc3::io
