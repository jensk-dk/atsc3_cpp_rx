#pragma once
// ALP → Baseband Packet packer (A/322 §5.2; the `alp2bbp` converter core and the deframer's test oracle).
#include <cstdint>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/tools/packets.hpp"

namespace atsc3::tools {

struct BbpPackerConfig {
    std::size_t kpayload_bytes = 5916;  // LDPC 64800, rate 11/15, BCH (as on the Baltimore STL)
    bool counter = false;               // add the EXT_TYPE 000 counter extension
};

/// Packs ALP packets, in order, into fixed-size BBPs: MODE=1 base field, OFI=10 long extension used for
/// the counter and/or all-padding, pointer 8191 when no ALP packet starts in the BBP, byte-granular
/// splitting across BBPs. `counter_state` carries the per-PLP counter across calls.
std::vector<Bytes> packBbps(const std::vector<Bytes>& alps, const BbpPackerConfig& cfg, std::uint16_t& counter_state);

/// Convenience: the A/322 Table 6.1 payload size in bytes for a 64800-bit LDPC code with BCH outer code.
std::size_t kpayloadBytes64k(unsigned rate_numerator);

}  // namespace atsc3::tools
