#pragma once
// Test-only packet builders and a recording ITunerSink. Generic builders live in atsc3::tools.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/io/units.hpp"
#include "atsc3/service/tuner.hpp"
#include "atsc3/tools/bbp_packer.hpp"
#include "atsc3/tools/packets.hpp"
#include "atsc3/tools/stltp_packer.hpp"

namespace atsc3::test {

using tools::Bytes;
using tools::ethernet;
using tools::ipv4Udp;
using tools::linuxSll;
using tools::rtp;

constexpr std::uint32_t addr(unsigned a, unsigned b, unsigned c, unsigned d) { return tools::ip4(a, b, c, d); }

inline std::vector<Bytes> bppsPackets(std::uint8_t plp, const Bytes& bbp, std::uint16_t& seq, std::uint32_t rtp_ts,
                                      std::size_t fragment = 1316) {
    tools::StltpPackerConfig cfg;
    cfg.inner_fragment = fragment;
    return tools::bppsPackets(plp, bbp, seq, rtp_ts, cfg);
}
inline std::vector<Bytes> stltpTunnel(const std::vector<Bytes>& inner, std::uint16_t& seq, std::size_t payload_size = 1360,
                                      std::uint8_t protocol_version = 0) {
    tools::StltpPackerConfig cfg;
    cfg.tunnel_payload = payload_size;
    cfg.protocol_version = protocol_version;
    return tools::stltpTunnel(inner, seq, cfg);
}

/// Minimal LLS datagram payload: table header + XML body (detector only looks at the address).
Bytes llsPayload(std::uint8_t table_id = 1);
/// Minimal LCT header (V=1, S=1, O=1, HDR_LEN=4, CP) + FEC payload id + payload.
Bytes lctPacket(std::uint32_t tsi, std::uint32_t toi, std::uint8_t cp, const Bytes& payload);
/// Minimal MMTP v01 packet (type 0 MPU).
Bytes mmtpPacket(std::uint16_t packet_id, std::uint32_t seq, const Bytes& payload);
/// A Baseband Packet with MODE=1, OFI=10 long extension of all-padding so that `payload` + padding = size.
Bytes basebandPacket(std::uint16_t pointer, const Bytes& payload, std::size_t total_size);
/// ALP single packet (IPv4 type unless given) wrapping `payload`; HM is set when the length needs it or when forced.
Bytes alpSingle(const Bytes& payload, std::uint8_t packet_type = 0, bool force_hm = false,
                std::optional<std::uint8_t> sid = std::nullopt);
/// ALP IPv4 packet whose payload is a UDP datagram of `payload_len` pseudo-random bytes.
Bytes alpIpv4Packet(std::size_t payload_len, std::uint32_t seed);

/// Writes records to a temporary pcap and returns its path.
std::string writePcap(const std::vector<std::pair<std::int64_t, Bytes>>& records, std::uint16_t link_type = 1,
                      const std::string& name = "test.pcap");
std::string tempPath(const std::string& name);

/// Records every callback with copies of the bytes.
class RecordingSink : public service::ITunerSink {
public:
    struct Unit {
        enum class Kind { Bbp, Alp, Ip } kind;
        std::uint8_t plp;
        io::UnitTime time;
        Bytes bytes;
        std::int64_t host_ns;
    };
    void onBbp(const io::BasebandPacket& p) override;
    void onAlp(const io::AlpPacket& p) override;
    void onIp(const io::IpDatagram& p) override;
    void onTunerEvent(const service::TunerEvent& e) override;

    std::vector<Unit> units() const;
    std::vector<service::TunerEvent> events() const;
    std::size_t count(Unit::Kind k) const;
    bool hasEvent(service::TunerEventType t) const;
    std::size_t eventCount(service::TunerEventType t) const;
    bool waitUnits(std::size_t n, std::chrono::milliseconds timeout) const;

private:
    void push(Unit u);
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    std::vector<Unit> units_;
    std::vector<service::TunerEvent> events_;
};

}  // namespace atsc3::test
