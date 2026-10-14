#pragma once
// BBP capture container (spec 6.2): pcap link type USER0 (147), each record =
// plp_id(8) flags(8) reserved(16) followed by one complete Baseband Packet.
#include <cstdint>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::pcap {

constexpr std::uint16_t kBbpContainerLinkType = 147;
constexpr std::uint8_t kBbpFlagScrambled = 0x01;
constexpr std::uint8_t kBbpFlagError = 0x02;
constexpr std::size_t kBbpContainerPrefix = 4;

struct BbpContainerRecord {
    std::uint8_t plp_id = 0;
    std::uint8_t flags = 0;
    ByteView bbp;
};

bool parseBbpContainerRecord(ByteView record, BbpContainerRecord& out) noexcept;
std::vector<std::uint8_t> makeBbpContainerRecord(std::uint8_t plp_id, std::uint8_t flags, ByteView bbp);

}  // namespace atsc3::pcap
