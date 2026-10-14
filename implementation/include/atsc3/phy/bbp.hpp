#pragma once
// Baseband Packet header (A/322 §5.2.2) and the BBP → ALP deframer (spec 5.3).
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::phy {

constexpr std::uint16_t kPointerNoAlpStart = 8191;

struct BbpHeader {
    bool mode = false;                 // MODE bit (0: 1-byte base field, 1: 2 bytes with OFI)
    std::uint16_t pointer = 0;         // 7 or 13 bits
    std::uint8_t ofi = 0;              // 00 none, 01 short, 10 long, 11 mixed
    std::uint8_t ext_type = 0;         // short/long: EXT_TYPE; mixed: first extension type
    std::uint8_t num_ext = 0;          // mixed: NUM_EXT
    std::uint16_t ext_len = 0;         // total extension field length in bytes
    std::optional<std::uint16_t> counter;  // EXT_TYPE 000 counter when present
    std::size_t header_length = 0;     // base + optional + extension
};

enum class BbpStatus { Ok, Invalid };
BbpStatus parseBbpHeader(ByteView bbp, BbpHeader& h) noexcept;

struct DeframerStats {
    std::uint64_t bbp = 0, alp = 0, bbp_invalid = 0, bbp_error = 0, pointer_no_start = 0, pointer_zero = 0;
    std::uint64_t orphan_prefix = 0, pending_mismatch = 0, alp_invalid = 0, counter_jumps = 0, flushed = 0;
};

class BbpDeframer {
public:
    using AlpSink = std::function<void(std::uint8_t plp, ByteView alp)>;
    explicit BbpDeframer(AlpSink sink);
    /// Feeds one complete, descrambled Baseband Packet of a PLP. `error` = uncorrectable FEC.
    void feed(std::uint8_t plp, ByteView bbp, bool error = false);
    void reset();
    void resetPlp(std::uint8_t plp);
    const DeframerStats& stats() const noexcept { return stats_; }

private:
    struct PlpState {
        std::vector<std::uint8_t> pending;  // partial ALP packet continued in the next BBP
        bool have_pending = false;
        std::optional<std::uint16_t> counter;
        bool need_resync = false;
    };
    void flush(PlpState& s);
    void emitFromBuffer(std::uint8_t plp, PlpState& s, const std::uint8_t* data, std::size_t len);

    AlpSink sink_;
    DeframerStats stats_;
    std::map<std::uint8_t, PlpState> plps_;
};

}  // namespace atsc3::phy
