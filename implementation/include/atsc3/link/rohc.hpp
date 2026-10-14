#pragma once
// ROHC-U decompressor interface (A/330 §6; spec 5.5.2). The in-house implementation is Phase 2;
// this header fixes the contract so the ALP dispatcher and the RDT context store can be wired now.
#include <cstdint>
#include <optional>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/link/signaling.hpp"

namespace atsc3::link {

class IRohcDecompressor {
public:
    virtual ~IRohcDecompressor() = default;
    /// Installs the contexts (static/dynamic chains) signalled by an RDT for its PLP.
    virtual void applyRdt(const Rdt& rdt) = 0;
    /// Decompresses one ROHC packet (large CID, U-mode). nullopt when the context is unknown or the
    /// packet fails its CRC; the caller counts and drops it.
    virtual std::optional<std::vector<std::uint8_t>> decompress(std::uint8_t plp, ByteView rohc) = 0;
    virtual void reset() = 0;
};

/// Placeholder that counts packets and decompresses nothing (used until Phase 2 lands).
class NullRohcDecompressor final : public IRohcDecompressor {
public:
    void applyRdt(const Rdt&) override {}
    std::optional<std::vector<std::uint8_t>> decompress(std::uint8_t, ByteView) override {
        ++dropped_;
        return std::nullopt;
    }
    void reset() override {}
    std::uint64_t dropped() const noexcept { return dropped_; }

private:
    std::uint64_t dropped_ = 0;
};

}  // namespace atsc3::link
