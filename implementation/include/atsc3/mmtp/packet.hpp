#pragma once
// MMTP packet header (ISO/IEC 23008-1 §9.3, A/331 §8.1.2; spec 5.10.2) and the MPU / signaling
// payload headers. Version-01 packets always carry the 16-bit QoS word (observed on both encoders).
#include <cstdint>
#include <optional>

#include "atsc3/byte_view.hpp"

namespace atsc3::mmtp {

enum class PayloadType : std::uint8_t { Mpu = 0, Gfd = 1, Signaling = 2, Repair = 3, Other = 0xFF };
const char* toString(PayloadType t) noexcept;

struct PacketHeader {
    std::uint8_t version = 0;
    bool packet_counter_flag = false;
    std::uint8_t fec_type = 0;
    bool extension_flag = false;  // f2
    bool flag_a = false, flag_b = false;  // f1, f0 (raw; not reliable RAP indicators)
    PayloadType type = PayloadType::Other;
    std::uint8_t raw_type = 0;
    std::uint16_t packet_id = 0;
    std::uint32_t timestamp = 0;  // NTP short
    std::uint32_t packet_sequence_number = 0;
    std::optional<std::uint32_t> packet_counter;
    std::optional<std::uint16_t> qos;  // version 01 only
    std::size_t header_bytes = 0;
    ByteView extension;  // header extension data when f2 is set
    ByteView payload;
};

struct MpuPayloadHeader {
    std::uint16_t length = 0;
    std::uint8_t fragment_type = 0;  // FT: 0 MPU metadata, 1 movie fragment metadata, 2 MFU
    bool timed = false;              // T
    std::uint8_t fragmentation_indicator = 0;  // f_i
    bool aggregation = false;        // A
    std::uint8_t fragment_counter = 0;
    std::uint32_t mpu_sequence_number = 0;
    ByteView data;                   // DU(s) after the 8-byte header
};

struct SignalingPayloadHeader {
    std::uint8_t fragmentation_indicator = 0;  // f_i
    bool long_length = false;        // H
    bool aggregation = false;        // A
    std::uint8_t fragment_counter = 0;
    ByteView data;                   // [MSG_length] message(s)
};

enum class ParseStatus { Ok, TooShort, BadVersion, LengthMismatch };

ParseStatus parsePacket(ByteView udp_payload, PacketHeader& out) noexcept;
ParseStatus parseMpuPayload(ByteView payload, MpuPayloadHeader& out) noexcept;
ParseStatus parseSignalingPayload(ByteView payload, SignalingPayloadHeader& out) noexcept;

}  // namespace atsc3::mmtp
