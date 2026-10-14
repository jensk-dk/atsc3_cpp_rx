#pragma once
// LLS processor (spec 5.7.1): table header, gzip, SignedMultiTable, registry by (group, table id),
// version/content dedup, typed parsers and raw pass-through for every table.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/lls/tables.hpp"

namespace atsc3::lls {

constexpr std::uint32_t kLlsAddress = 0xE000173Cu;  // 224.0.23.60
constexpr std::uint16_t kLlsPort = 4937;

struct Callbacks {
    std::function<void(const TableInfo&, const Slt&)> onSlt;
    std::function<void(const TableInfo&, const SystemTime&)> onSystemTime;
    std::function<void(const TableInfo&, const Aeat&)> onAeat;
    std::function<void(const TableInfo&, const Rrt&)> onRrt;
    std::function<void(const TableInfo&, const Omn&)> onOmn;
    /// Every new table instance, typed or not (CDT, DRCT, UserDefined, unknown ids): raw bytes after the header,
    /// decompressed when gzip. Called before the typed callback.
    std::function<void(const TableInfo&, ByteView payload)> onTable;
    std::function<void(const TableInfo&, const SignedMultiTable&)> onSignedMultiTable;
    std::function<void(const std::string& message)> onError;
};

struct ProcessorStats {
    std::uint64_t datagrams = 0, tables = 0, duplicates = 0, new_tables = 0, bad_header = 0;
    std::uint64_t gunzip_failed = 0, parse_failed = 0, signed_multi_tables = 0;
};

struct TableRecord {
    TableInfo info;
    std::vector<std::uint8_t> payload;  // decompressed
};

class Processor {
public:
    explicit Processor(Callbacks cb);
    /// Feed any UDP datagram; non-LLS destinations are ignored (returns false).
    bool onDatagram(const ip::UdpDatagram& d);
    /// Feed a raw LLS_table() (bytes starting at LLS_table_id) with its origin.
    void onLlsTable(ByteView table, std::uint32_t source_ip, std::uint8_t plp, std::int64_t t_ns);

    // registry
    std::vector<std::uint8_t> groups() const;
    std::optional<Slt> slt(std::uint8_t group) const;
    std::optional<SystemTime> systemTime(std::uint8_t group) const;
    const TableRecord* table(std::uint8_t group, std::uint8_t table_id) const;
    std::vector<const TableRecord*> tables() const;
    /// Number of groups announced by group_count_minus1 of the latest table of any group (0 = unknown).
    std::uint8_t announcedGroups() const noexcept { return announced_groups_; }
    const ProcessorStats& stats() const noexcept { return stats_; }
    void reset();

private:
    void handleTable(const TableHeader& h, ByteView payload, std::uint32_t source_ip, std::uint8_t plp, std::int64_t t_ns,
                     bool in_smt, std::uint8_t smt_group);
    void dispatch(const TableInfo& info, ByteView payload);

    Callbacks cb_;
    ProcessorStats stats_;
    std::map<std::uint16_t, TableRecord> registry_;   // key: group << 8 | table_id
    std::map<std::uint16_t, std::uint64_t> hashes_;   // content hash per key
    std::map<std::uint8_t, Slt> slts_;
    std::map<std::uint8_t, SystemTime> stts_;
    std::uint8_t announced_groups_ = 0;
};

}  // namespace atsc3::lls
