#pragma once
// Link-layer signaling tables (A/330 §7): LMT and RDT, plus the PLP map (spec 5.5).
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "atsc3/alp/header.hpp"
#include "atsc3/byte_view.hpp"
#include "atsc3/io/units.hpp"

namespace atsc3::link {

constexpr std::uint8_t kSignalingLmt = 0x01;
constexpr std::uint8_t kSignalingRdt = 0x02;

struct LmtMulticast {
    std::uint32_t src_ip = 0, dst_ip = 0;  // host order
    std::uint16_t src_port = 0, dst_port = 0;
    std::optional<std::uint8_t> sid;
    std::optional<std::uint8_t> context_id;  // compressed_flag == 1
};

struct LmtPlp {
    std::uint8_t plp_id = 0;
    std::vector<LmtMulticast> multicasts;
};

struct Lmt {
    std::uint8_t version = 0;  // signaling_version from the ALP header
    std::vector<LmtPlp> plps;
};

/// Parses link_mapping_table() (A/330 Table 7.2).
bool parseLmt(ByteView table, Lmt& out) noexcept;

struct RdtContext {
    std::uint8_t context_id = 0;
    std::uint8_t profile = 0;          // 8 LSBs of the ROHC profile id
    std::uint8_t context_config = 0;   // 0 none, 1 static, 2 dynamic, 3 both
    std::vector<std::uint8_t> chain;   // static_chain_byte()/dynamic_chain_byte() as transmitted
};

struct Rdt {
    std::uint8_t version = 0;
    std::uint8_t plp_id = 0;
    std::uint8_t max_cid = 0;
    std::uint8_t adaptation_mode = 0;  // 0 mode 1, 1 mode 2, 2 mode 3
    std::uint8_t context_config = 0;
    std::vector<RdtContext> contexts;
};

/// Parses ROHC-U_description_table() (A/330 Table 7.4).
bool parseRdt(ByteView table, Rdt& out) noexcept;

/// Multicast → PLP mapping (spec 5.5.1): from LMTs received on air or synthesized from a sidecar.
class PlpMap {
public:
    struct Entry {
        std::uint8_t plp_id = 0;
        std::uint32_t src_ip = 0;      // 0 = wildcard
        std::uint16_t src_port = 0;    // 0 = wildcard
        std::optional<std::uint8_t> sid;
        std::optional<std::uint8_t> context_id;
        io::LmtOrigin origin = io::LmtOrigin::Broadcast;
        std::uint32_t dst_ip = 0;      // filled by entries()
        std::uint16_t dst_port = 0;
    };
    /// Replaces the entries learned from the LMT carried in `carrier_plp` (an LMT may describe several PLPs).
    void applyLmt(std::uint8_t carrier_plp, const Lmt& lmt);
    void applySidecar(const std::vector<io::LmtEntry>& entries);
    /// Best match for a flow: exact source first, then wildcard source.
    std::optional<Entry> lookup(std::uint32_t dst_ip, std::uint16_t dst_port, std::uint32_t src_ip = 0,
                                std::uint16_t src_port = 0) const;
    std::vector<Entry> entries() const;
    std::size_t size() const noexcept { return entries_.size(); }
    void clear() { entries_.clear(); carriers_.clear(); }

private:
    struct Keyed {
        std::uint32_t dst_ip;
        std::uint16_t dst_port;
        Entry entry;
        std::uint8_t carrier;  // PLP whose LMT produced it (0xFF = sidecar)
    };
    std::vector<Keyed> entries_;
    std::map<std::uint8_t, std::uint8_t> carriers_;
};

/// Consumes ALP signaling packets: versions, dedup, parsing, callbacks and PLP map maintenance.
class LinkSignaling {
public:
    struct Callbacks {
        std::function<void(std::uint8_t plp, const Lmt&)> onLmt;
        std::function<void(std::uint8_t plp, const Rdt&)> onRdt;
        std::function<void(std::uint8_t plp, const alp::SignalingInfo&, ByteView)> onUserPrivate;
        std::function<void(std::uint8_t plp, const alp::SignalingInfo&, ByteView)> onUnparsed;
    };
    struct Stats {
        std::uint64_t tables = 0, lmt = 0, lmt_new = 0, rdt = 0, rdt_new = 0, user_private = 0;
        std::uint64_t invalid = 0, unsupported = 0;
    };
    explicit LinkSignaling(Callbacks cb);
    void onSignaling(std::uint8_t plp, const alp::SignalingInfo& info, ByteView table);
    const PlpMap& plpMap() const noexcept { return map_; }
    PlpMap& plpMap() noexcept { return map_; }
    std::optional<Lmt> lmt(std::uint8_t plp) const;
    std::optional<Rdt> rdt(std::uint8_t plp) const;
    const Stats& stats() const noexcept { return stats_; }
    void reset();

private:
    Callbacks cb_;
    Stats stats_;
    PlpMap map_;
    std::map<std::uint8_t, Lmt> lmts_;  // by carrier PLP
    std::map<std::uint8_t, Rdt> rdts_;
};

}  // namespace atsc3::link
