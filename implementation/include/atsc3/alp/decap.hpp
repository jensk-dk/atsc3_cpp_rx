#pragma once
// ALP decapsulation (spec 5.4): header parsing, segmentation reassembly, concatenation splitting,
// SID filtering, dispatch by packet type, MPEG-2 TS reconstruction.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atsc3/alp/header.hpp"
#include "atsc3/byte_view.hpp"

namespace atsc3::alp {

enum class EventType { ZeroLength, PrivateExtension, ReservedType, InvalidHeader, Truncated, ReassemblyDrop, SidFiltered };

struct Event {
    EventType type;
    std::uint8_t plp;
    std::string detail;
};

struct Stats {
    std::uint64_t packets = 0, ipv4 = 0, compressed_ip = 0, signaling = 0, type_extension = 0, ts = 0;
    std::uint64_t ts_packets = 0, segments = 0, reassembled = 0, concatenated = 0, components = 0;
    std::uint64_t zero_length = 0, private_extension = 0, reserved_type = 0, invalid = 0, truncated = 0;
    std::uint64_t reassembly_drops = 0, sid_filtered = 0, unsupported = 0;
};

struct Callbacks {
    /// Whole IPv4 packet (after segmentation reassembly / concatenation splitting).
    std::function<void(std::uint8_t plp, ByteView ip, std::optional<std::uint8_t> sid)> onIpv4;
    /// ROHC-compressed IP packet (A/330 §6), to be decompressed by the link layer.
    std::function<void(std::uint8_t plp, ByteView rohc, std::optional<std::uint8_t> sid)> onCompressedIp;
    /// Link-layer signaling table (LMT, RDT, user private), raw payload.
    std::function<void(std::uint8_t plp, const SignalingInfo& info, ByteView table)> onSignaling;
    std::function<void(std::uint8_t plp, std::uint16_t extended_type, ByteView payload)> onTypeExtension;
    /// Reconstructed 188-byte TS packets (concatenated, n*188 bytes, sync byte restored, nulls re-inserted).
    std::function<void(std::uint8_t plp, ByteView ts_packets)> onTs;
    std::function<void(const Event&)> onEvent;
};

class Decap {
public:
    explicit Decap(Callbacks cb);
    /// Feeds one complete ALP packet (header + payload) as delivered by a tuner or the BBP deframer.
    void feed(std::uint8_t plp, ByteView alp);
    /// Drop packets whose SID is not in the set (packets without SID pass). Empty set = no filter.
    void setSidFilter(std::uint8_t plp, std::set<std::uint8_t> sids);
    void reset();
    const Stats& stats() const noexcept { return stats_; }
    std::size_t maxReassemblyBytes() const noexcept { return max_reassembly_; }
    void setMaxReassemblyBytes(std::size_t n) noexcept { max_reassembly_ = n; }

private:
    struct Reassembly {
        std::vector<std::uint8_t> data;
        std::uint8_t next_sn = 0;
        Header first;  // header of the first segment (signaling info, extension)
    };
    using Key = std::uint32_t;  // plp << 16 | type << 8 | sid(+1, 0 = none)

    void dispatch(std::uint8_t plp, const Header& h, ByteView payload);
    void deliverTs(std::uint8_t plp, const Header& h, ByteView payload);
    void event(EventType t, std::uint8_t plp, std::string detail);

    Callbacks cb_;
    Stats stats_;
    std::map<Key, Reassembly> reassembly_;
    std::map<std::uint8_t, std::set<std::uint8_t>> sid_filter_;
    std::size_t max_reassembly_ = 65535;
    std::vector<std::uint8_t> ts_buf_;
};

}  // namespace atsc3::alp
