#pragma once
// Reader for classic pcap (both magics, both byte orders, µs and ns) and pcapng (SHB/IDB/EPB/SPB).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace atsc3::pcap {

namespace linktype {
constexpr std::uint16_t kEthernet = 1;
constexpr std::uint16_t kRawBsd = 12;
constexpr std::uint16_t kRawIp = 101;
constexpr std::uint16_t kLinuxSll = 113;
constexpr std::uint16_t kUser0 = 147;
constexpr std::uint16_t kUser15 = 162;
constexpr std::uint16_t kIpv4 = 228;
constexpr std::uint16_t kLinuxSll2 = 276;
constexpr std::uint16_t kAtscAlp = 289;
inline bool isUser(std::uint16_t lt) noexcept { return lt >= kUser0 && lt <= kUser15; }
inline bool isIpBearing(std::uint16_t lt) noexcept {
    return lt == kEthernet || lt == kRawBsd || lt == kRawIp || lt == kLinuxSll || lt == kIpv4 || lt == kLinuxSll2;
}
}  // namespace linktype

struct PcapRecord {
    std::uint32_t interface_id = 0;
    std::uint16_t link_type = 0;
    std::int64_t ts_ns = 0;      // since Unix epoch
    std::uint32_t orig_len = 0;  // length on the wire
    std::vector<std::uint8_t> data;
};

class PcapReader {
public:
    static std::unique_ptr<PcapReader> open(const std::string& path, std::string* error = nullptr);
    ~PcapReader();
    PcapReader(const PcapReader&) = delete;
    PcapReader& operator=(const PcapReader&) = delete;

    /// Next record; false at end of file or on a truncated tail (truncation is reported via truncated()).
    bool next(PcapRecord& rec);
    void rewind();
    bool isPcapng() const noexcept { return pcapng_; }
    bool truncated() const noexcept { return truncated_; }
    std::uint64_t recordIndex() const noexcept { return index_; }
    /// Link type per interface id (classic pcap: exactly one).
    const std::vector<std::uint16_t>& interfaces() const noexcept { return if_link_; }
    const std::string& path() const noexcept { return path_; }

private:
    PcapReader() = default;
    bool readHeader(std::string* error);
    bool nextClassic(PcapRecord& rec);
    bool nextNg(PcapRecord& rec);
    bool readExact(void* buf, std::size_t n);

    std::string path_;
    std::FILE* f_ = nullptr;
    bool pcapng_ = false;
    bool swap_ = false;        // file byte order differs from host
    bool nanos_ = false;       // classic pcap with ns magic
    bool truncated_ = false;
    std::uint64_t index_ = 0;
    std::uint32_t snaplen_ = 0;
    std::vector<std::uint16_t> if_link_;
    std::vector<std::int64_t> if_tsres_ns_;  // ns per tick for each interface (pcapng)
    std::uint16_t classic_link_ = 0;
};

/// Minimal classic-pcap writer (nanosecond magic) used by converters and tests.
class PcapWriter {
public:
    static std::unique_ptr<PcapWriter> create(const std::string& path, std::uint16_t link_type,
                                              std::string* error = nullptr);
    ~PcapWriter();
    PcapWriter(const PcapWriter&) = delete;
    PcapWriter& operator=(const PcapWriter&) = delete;
    bool write(std::int64_t ts_ns, const std::uint8_t* data, std::size_t len);
    bool write(std::int64_t ts_ns, const std::vector<std::uint8_t>& data) { return write(ts_ns, data.data(), data.size()); }

private:
    PcapWriter() = default;
    std::FILE* f_ = nullptr;
};

}  // namespace atsc3::pcap
