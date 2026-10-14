#pragma once
// IPv4/UDP demultiplexer and flow router (spec 5.6).
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/io/units.hpp"

namespace atsc3::ip {

struct FlowKey {
    std::uint32_t dst_ip = 0;   // host order
    std::uint16_t dst_port = 0;
    std::uint32_t src_ip = 0;   // 0 = any source
};

struct UdpDatagram {
    std::uint8_t plp = io::kUnknownPlp;
    std::optional<std::uint8_t> sid;
    io::UnitTime time;
    std::uint32_t src_ip = 0, dst_ip = 0;
    std::uint16_t src_port = 0, dst_port = 0;
    ByteView payload;
};

using FlowHandler = std::function<void(const UdpDatagram&)>;
using SubscriptionId = std::uint32_t;

struct DemuxConfig {
    bool verify_ip_checksum = true;
    bool strict_udp_checksum = false;   // drop datagrams with a bad UDP checksum
    std::int64_t fragment_timeout_ns = 2'000'000'000;
    std::size_t fragment_max_bytes = 65535;
    std::size_t max_unrouted_flows = 256;
};

struct DemuxStats {
    std::uint64_t ipv4 = 0, bad_ipv4 = 0, ip_checksum_bad = 0, non_udp = 0, ipv6_or_other = 0;
    std::uint64_t fragments = 0, reassembled = 0, fragments_dropped = 0;
    std::uint64_t udp = 0, udp_bad_length = 0, udp_checksum_bad = 0;
    std::uint64_t routed = 0, unrouted = 0;
};

struct UnroutedFlow {
    std::uint32_t dst_ip = 0, src_ip = 0;
    std::uint16_t dst_port = 0;
    std::uint64_t packets = 0, bytes = 0;
    std::uint8_t plp = io::kUnknownPlp;
};

class Demux {
public:
    explicit Demux(DemuxConfig cfg = {});
    /// Entry point for IPv4 packets from ALP (with PLP/SID) or from the IP bypass path.
    void onIpv4(std::uint8_t plp, ByteView ip, io::UnitTime time, std::optional<std::uint8_t> sid = std::nullopt);

    /// Subscribes to a flow; several subscribers per flow are allowed. Lookup order: exact source,
    /// wildcard source, then taps. Returns an id for unsubscribe().
    SubscriptionId subscribe(FlowKey key, FlowHandler handler);
    /// Receives every UDP datagram (diagnostics / raw flow taps).
    SubscriptionId tapAll(FlowHandler handler);
    void unsubscribe(SubscriptionId id);

    const DemuxStats& stats() const noexcept { return stats_; }
    /// Flows seen without any subscriber (capped), most recent packets counted.
    std::vector<UnroutedFlow> unroutedFlows() const;
    void clearUnrouted() { unrouted_.clear(); }
    /// Expires stale fragment buffers (also done opportunistically on each packet).
    void expireFragments(std::int64_t now_ns);
    void reset();

private:
    struct Subscriber {
        SubscriptionId id;
        FlowKey key;
        bool tap;
        bool dead = false;                      // unsubscribed while a delivery was in progress
        std::shared_ptr<FlowHandler> handler;   // shared so a handler stays alive while it runs
    };
    struct FragmentBuffer {
        std::vector<std::uint8_t> data;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // [begin, end)
        std::optional<std::uint32_t> total;
        std::int64_t first_ns = 0;
        std::uint8_t plp = io::kUnknownPlp;
        std::optional<std::uint8_t> sid;
    };
    void deliver(const UdpDatagram& d);
    bool reassemble(std::uint8_t plp, ByteView ip, io::UnitTime time, std::optional<std::uint8_t> sid,
                    std::vector<std::uint8_t>& out);

    DemuxConfig cfg_;
    DemuxStats stats_;
    std::vector<Subscriber> subs_;
    SubscriptionId next_id_ = 1;
    int delivering_ = 0;        // reentrancy depth: handlers may (un)subscribe during delivery
    bool pending_erase_ = false;
    std::map<std::uint64_t, UnroutedFlow> unrouted_;
    std::map<std::uint64_t, FragmentBuffer> fragments_;  // key: src ^ dst ^ id/proto
};

}  // namespace atsc3::ip
