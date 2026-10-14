#pragma once
// MMTP session (spec 5.10): per-packet_id statistics, signaling reassembly (fragmentation and
// aggregation), latest MP tables and mmt_atsc3_messages. MPU reconstruction is a later phase.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/ip/demux.hpp"
#include "atsc3/mmtp/mpu.hpp"
#include "atsc3/mmtp/packet.hpp"
#include "atsc3/mmtp/signaling.hpp"

namespace atsc3::mmtp {

struct PacketIdStats {
    std::uint16_t packet_id = 0;
    std::uint64_t packets = 0, bytes = 0, mpu_packets = 0, signaling_packets = 0, other_packets = 0;
    std::uint64_t sequence_gaps = 0, length_mismatch = 0;
    std::optional<std::uint32_t> last_sequence;
    std::optional<std::uint32_t> last_mpu_sequence;
    std::uint64_t mpus = 0;        // distinct MPU sequence numbers seen
    std::int64_t last_ns = 0;
};

struct SessionCallbacks {
    std::function<void(const MpTable&)> onMpTable;             // every (new version of a) table
    std::function<void(std::uint16_t packet_id, const Atsc3Message&)> onAtsc3Message;
    std::function<void(std::uint16_t packet_id, const Message&)> onMessage;  // all messages, raw
    std::function<void(std::shared_ptr<const MpuInitSegment>)> onMpuInit;    // MPU assembly (setMpuAssembly(true))
    std::function<void(std::shared_ptr<const MpuFragment>)> onMpuFragment;
};

struct SessionStats {
    std::uint64_t packets = 0, bad_header = 0, messages = 0, fragments = 0, fragment_drops = 0, mp_tables = 0, atsc3_messages = 0;
};

class Session {
public:
    explicit Session(SessionCallbacks cb = {});
    void onDatagram(const ip::UdpDatagram& d);
    const SessionStats& stats() const noexcept { return stats_; }
    std::vector<PacketIdStats> packetIds() const;
    /// Latest complete MP table (0x11/0x20), with subset-table MPU timestamps merged in.
    const MpTable* mpTable() const noexcept { return complete_ ? &*complete_ : nullptr; }
    std::vector<MpTable> subsetTables() const;
    /// Latest mmt_atsc3_message per (content_type, uri).
    std::vector<Atsc3Message> atsc3Messages() const;
    std::vector<std::uint16_t> messageIdsSeen() const;
    void reset();

    /// Enables MPU reconstruction (one MpuAssembler per media packet_id, fed with the presentation times of the MP
    /// tables). Disabling drops the assemblers and their pending data; the session keeps the recent timestamps.
    void setMpuAssembly(bool on);
    bool mpuAssembly() const noexcept { return mpu_enabled_; }
    MpuAssemblerConfig& mpuConfig() noexcept { return mpu_cfg_; }
    const MpuAssembler* assembler(std::uint16_t packet_id) const;
    std::vector<std::uint16_t> assemblerIds() const;
    /// Service timeline origin: the first MPU_timestamp_descriptor seen on any asset (NTP64).
    std::optional<std::uint64_t> anchorNtp() const noexcept { return anchor_; }
    /// Emits whatever the assemblers hold (end of stream).
    void flushMpus();
    /// Starts the MPU timeline over (pcap loop wrap): drops the assemblers, the cached descriptors and the anchor, so
    /// the next MPU_timestamp_descriptor re-anchors the service. Assembly stays enabled if it was.
    void resetMpuTimeline();

private:
    void handleSignaling(const PacketHeader& h, ByteView payload);
    void handleMessage(std::uint16_t packet_id, ByteView msg);
    void noteTimestamps(const MpTable& t);
    MpuAssembler& assemblerFor(std::uint16_t packet_id);

    SessionCallbacks cb_;
    SessionStats stats_;
    std::map<std::uint16_t, PacketIdStats> pids_;
    std::map<std::uint16_t, std::vector<std::uint8_t>> pending_;  // fragmented signaling per packet_id
    std::optional<MpTable> complete_;
    std::map<std::uint8_t, MpTable> subsets_;
    std::map<std::uint64_t, Atsc3Message> atsc3_;  // key content_type<<32 | hash(uri)
    std::map<std::uint16_t, std::uint64_t> message_ids_;
    // MPU assembly
    bool mpu_enabled_ = false;
    MpuAssemblerConfig mpu_cfg_;
    std::map<std::uint16_t, std::unique_ptr<MpuAssembler>> assemblers_;
    std::map<std::uint16_t, std::vector<MpuTimestamp>> recent_pts_;   // per packet_id, bounded; replayed into new assemblers
    std::optional<std::uint64_t> anchor_;
};

}  // namespace atsc3::mmtp
