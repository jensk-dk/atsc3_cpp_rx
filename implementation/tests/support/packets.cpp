#include "packets.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

#include "atsc3/tuners/pcap/pcap_reader.hpp"

namespace atsc3::test {

namespace {
void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32(Bytes& b, std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v >> 16));
    put16(b, static_cast<std::uint16_t>(v & 0xFFFF));
}
}  // namespace

Bytes llsPayload(std::uint8_t table_id) {
    Bytes b{table_id, 0x01, 0x00, 0x01};
    const std::string xml = "<SLT bsid=\"1\"/>";
    b.insert(b.end(), xml.begin(), xml.end());
    return b;
}

Bytes lctPacket(std::uint32_t tsi, std::uint32_t toi, std::uint8_t cp, const Bytes& payload) {
    Bytes b{0x12, 0xA0, 0x04, cp};
    put32(b, 0);
    put32(b, tsi);
    put32(b, toi);
    put32(b, 0);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes mmtpPacket(std::uint16_t packet_id, std::uint32_t seq, const Bytes& payload) {
    Bytes b{0x40, 0x00};
    put16(b, packet_id);
    put32(b, 0x12345678);
    put32(b, seq);
    put16(b, 0x8000);
    put16(b, static_cast<std::uint16_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes basebandPacket(std::uint16_t pointer, const Bytes& payload, std::size_t total_size) {
    if (payload.size() + 4 > total_size) throw std::runtime_error("bbp too small");
    const std::size_t ext_len = total_size - 4 - payload.size();
    Bytes b;
    b.push_back(static_cast<std::uint8_t>(0x80 | (pointer & 0x7F)));
    b.push_back(static_cast<std::uint8_t>(((pointer >> 7) & 0x3F) << 2 | 0x02));
    b.push_back(static_cast<std::uint8_t>(0xE0 | (ext_len & 0x1F)));
    b.push_back(static_cast<std::uint8_t>((ext_len >> 5) & 0xFF));
    b.insert(b.end(), ext_len, 0);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes alpSingle(const Bytes& payload, std::uint8_t packet_type, bool force_hm, std::optional<std::uint8_t> sid) {
    const bool hm = force_hm || sid.has_value() || payload.size() > 2047;
    Bytes b;
    const std::size_t len = payload.size();
    b.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(packet_type) << 5) | (hm ? 0x08u : 0u) |
                                          static_cast<unsigned>((len >> 8) & 0x07)));
    b.push_back(static_cast<std::uint8_t>(len & 0xFF));
    if (hm) b.push_back(static_cast<std::uint8_t>(((len >> 11) << 3) | 0x04u | (sid ? 0x02u : 0u)));
    if (sid) b.push_back(*sid);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes alpIpv4Packet(std::size_t payload_len, std::uint32_t seed) {
    Bytes body(payload_len);
    std::uint32_t x = seed * 2654435761u + 1;
    for (auto& v : body) {
        x = x * 1103515245u + 12345u;
        v = static_cast<std::uint8_t>(x >> 16);
    }
    return alpSingle(ipv4Udp(addr(10, 0, 0, 1), addr(239, 1, 1, 1), 1000, 2000, body));
}

std::string tempPath(const std::string& name) {
    const char* dir = std::getenv("ATSC3_TEST_TMP");
    std::string base = dir ? dir : "/tmp";
    return base + "/atsc3-test-" + std::to_string(::getpid()) + "-" + name;
}

std::string writePcap(const std::vector<std::pair<std::int64_t, Bytes>>& records, std::uint16_t link_type,
                      const std::string& name) {
    const std::string path = tempPath(name);
    auto w = pcap::PcapWriter::create(path, link_type);
    if (!w) throw std::runtime_error("cannot create " + path);
    for (const auto& [ts, data] : records) w->write(ts, data);
    return path;
}

void RecordingSink::push(Unit u) {
    u.host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        units_.push_back(std::move(u));
    }
    cv_.notify_all();
}
void RecordingSink::onBbp(const io::BasebandPacket& p) {
    push(Unit{Unit::Kind::Bbp, p.plp_id, p.time, Bytes(p.bytes.begin(), p.bytes.end()), 0});
}
void RecordingSink::onAlp(const io::AlpPacket& p) {
    push(Unit{Unit::Kind::Alp, p.plp_id, p.time, Bytes(p.bytes.begin(), p.bytes.end()), 0});
}
void RecordingSink::onIp(const io::IpDatagram& p) {
    push(Unit{Unit::Kind::Ip, p.plp_id, p.time, Bytes(p.bytes.begin(), p.bytes.end()), 0});
}
void RecordingSink::onTunerEvent(const service::TunerEvent& e) {
    std::lock_guard<std::mutex> lock(mtx_);
    events_.push_back(e);
}
std::vector<RecordingSink::Unit> RecordingSink::units() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return units_;
}
std::vector<service::TunerEvent> RecordingSink::events() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return events_;
}
std::size_t RecordingSink::count(Unit::Kind k) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<std::size_t>(std::count_if(units_.begin(), units_.end(), [k](const Unit& u) { return u.kind == k; }));
}
bool RecordingSink::hasEvent(service::TunerEventType t) const { return eventCount(t) > 0; }
std::size_t RecordingSink::eventCount(service::TunerEventType t) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [t](const service::TunerEvent& e) { return e.type == t; }));
}
bool RecordingSink::waitUnits(std::size_t n, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mtx_);
    return cv_.wait_for(lock, timeout, [&] { return units_.size() >= n; });
}

}  // namespace atsc3::test
