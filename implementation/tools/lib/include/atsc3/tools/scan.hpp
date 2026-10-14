#pragma once
// atsc3scan (spec 6.1): capture inventory over the stack's own parsers. The pcap virtual tuner delivers the capture
// (BBP, ALP or IP); the scanner runs deframer → ALP decapsulation → IP demux and classifies every UDP flow as LLS,
// ROUTE/LCT, MMTP or unclassified, counting what the inventory of a new capture needs: link-layer facts (BBP headers,
// ALP header shapes, LMT), LLS tables, LCT flows per TSI/codepoint/HET with object completeness, MMTP flows per
// packet_id with the MP tables and mmt_atsc3_messages, and the UDP flow table.
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "atsc3/alp/decap.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/link/signaling.hpp"
#include "atsc3/lls/processor.hpp"
#include "atsc3/mmtp/signaling.hpp"
#include "atsc3/phy/bbp.hpp"
#include "atsc3/stltp/demux.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"

namespace atsc3::tools {

struct ScanOptions {
    std::string input;
    std::uint64_t max_records = 0;       // first N pcap records (0 = whole file)
    std::int64_t start_offset_ns = 0;
    std::int64_t duration_ns = 0;        // 0 = until end of file
    std::string sidecar = "auto";        // "auto" | "off" | path
    std::uint32_t probe_packets = 0;     // format detector probe window (0 = default)
    bool keep_sls_objects = true;        // keep the bytes of TSI 0 objects for ScanPrint::sls
    std::size_t max_odd_packets = 10;    // ALP packets listed as odd (zero length / trailing bytes)
    std::size_t max_lmts = 4;            // distinct LMTs kept
    std::size_t max_mp_tables = 6;       // MP table instances kept per MMTP flow
};

struct ScanPrint {
    bool slt = false;        // print the SLT XML of every LLS SLT
    bool sls = false;        // print every contiguous TSI 0 (SLS) object as text
    bool objs = false;       // list every object
    bool mmt = false;        // print MP tables and mmt_atsc3_messages
    std::size_t max_sls_bytes = 6000;
};

struct UdpFlowCount {
    std::uint32_t src = 0, dst = 0;   // host order
    std::uint16_t port = 0;
    std::uint8_t plp = 0xFF;
    std::uint64_t packets = 0, bytes = 0;   // bytes = IPv4 total length (20 + 8 + payload, no options)
};

struct AlpOddPacket {
    std::uint64_t index = 0;          // ALP packet index in delivery order
    std::string layout;
    std::size_t length = 0;           // payload length from the header
    long trailing = 0;                // bytes beyond the header's total length (negative = truncated)
    std::string head_hex;             // first 16 bytes
};

struct AlpReport {
    std::uint64_t packets = 0;
    std::map<std::string, std::uint64_t> types;            // "ipv4", "signaling", ...
    std::map<std::string, std::uint64_t> layouts;          // single / segment / concatenation / ts
    std::map<std::uint8_t, std::uint64_t> sids;
    std::map<std::string, std::uint64_t> extensions;       // "0xf1 04" -> count
    std::map<std::uint8_t, std::uint64_t> signaling_types; // ALP signaling type -> packets
    std::uint64_t odd = 0;                                 // zero length or trailing bytes
    std::vector<AlpOddPacket> odd_packets;
    alp::Stats decap;
};

struct BbpReport {
    std::map<std::uint8_t, std::uint64_t> per_plp;
    std::map<std::size_t, std::uint64_t> lengths;
    std::map<std::uint8_t, std::uint64_t> ofi;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint64_t> extensions;  // (OFI, EXT_TYPE)
    std::uint64_t pointer_zero = 0, pointer_no_start = 0, pointer_other = 0, invalid = 0, errors = 0;
    phy::DeframerStats deframer;
};

struct LmtReport {
    std::uint8_t carrier_plp = 0;
    link::Lmt lmt;
};

struct LlsTableReport {
    std::uint8_t group = 0, table_id = 0, version = 0;
    std::uint32_t source_ip = 0;
    bool in_signed_multi_table = false;
    std::size_t length = 0;
    std::string preview;   // first 160 bytes, non-printables escaped
    std::string text;      // whole table as text (XML tables)
};

struct SignedMultiTableReport {
    std::uint8_t group = 0, version = 0;
    std::uint32_t source_ip = 0;
    std::uint16_t signature_length = 0;
    std::vector<lls::SignedMultiTableEntry> entries;
};

struct LctFlowReport {
    std::uint32_t dst = 0;
    std::uint16_t port = 0;
    std::uint64_t packets = 0, close_object = 0, close_session = 0;
    std::map<std::uint64_t, std::uint64_t> tsi;
    std::map<std::uint8_t, std::uint64_t> codepoints;
    std::map<std::uint8_t, std::uint64_t> hets;
    std::map<std::string, std::uint64_t> header_shapes;  // "C,S,O,H,HL"
    std::size_t objects = 0;                             // distinct (tsi, toi)
};

struct ObjectReport {
    std::uint32_t dst = 0;
    std::uint16_t port = 0;
    std::uint64_t tsi = 0, toi = 0;
    std::uint64_t received = 0;        // bytes covered by the merged ranges
    std::uint64_t extent = 0;          // highest end offset seen
    std::optional<std::uint64_t> transfer_length;   // EXT_TOL / EXT_FTI
    bool contiguous = false;           // one range from offset 0 to extent
    bool complete = false;             // contiguous and (no transfer length or extent == transfer length)
    std::string first_hex;             // first 12 bytes of the object
    std::vector<std::uint8_t> data;    // TSI 0 objects only (keep_sls_objects)
};

struct MpTableReport {
    std::uint16_t message_id = 0;
    std::uint16_t packet_id = 0;
    mmtp::MpTable table;
};

struct Atsc3MessageReport {
    std::uint16_t packet_id = 0;
    mmtp::Atsc3Message message;   // content truncated to 300 bytes
    std::size_t content_length = 0;
};

struct MmtpFlowReport {
    std::uint32_t dst = 0;
    std::uint16_t port = 0;
    std::uint64_t packets = 0, bad_header = 0, length_mismatch = 0, sequence_gaps = 0;
    std::map<std::uint16_t, std::uint64_t> packet_ids;
    std::map<std::string, std::uint64_t> payload_types;              // "mpu", "signaling"
    std::map<std::uint8_t, std::uint64_t> byte0, byte1;              // raw header bytes (layout check)
    std::map<std::uint8_t, std::uint64_t> mpu_fragment_types, mpu_fragmentation, mpu_aggregation;
    std::map<std::string, std::uint64_t> signaling_fragmentation;    // "f_i,A"
    std::map<std::string, std::uint64_t> messages;                   // "pid:0x8100"
    std::map<std::uint16_t, std::uint64_t> atsc3_content_types;
    std::vector<MpTableReport> mp_tables;                            // first max_mp_tables instances
    std::vector<Atsc3MessageReport> atsc3_messages;                  // first per (content_type, uri)
};

struct ScanReport {
    std::string detection;
    pcap::CaptureFormat mode = pcap::CaptureFormat::Unsupported;
    service::TunerOutput output = service::TunerOutput::Ip;
    pcap::VirtualTuner::Stats tuner;
    std::optional<stltp::Stats> stltp;
    std::optional<io::L1Info> l1;
    std::string sidecar;
    std::int64_t first_capture_ns = 0, last_capture_ns = 0;

    BbpReport bbp;
    AlpReport alp;
    link::LinkSignaling::Stats link;
    std::vector<LmtReport> lmts;
    ip::DemuxStats demux;
    std::vector<UdpFlowCount> udp_flows;           // by packets, descending
    lls::ProcessorStats lls;
    std::vector<LlsTableReport> lls_tables;
    std::vector<SignedMultiTableReport> signed_multi_tables;
    std::vector<LctFlowReport> lct_flows;          // first-seen order
    std::vector<ObjectReport> objects;             // by (dst, port, tsi, toi)
    std::uint64_t objects_complete = 0, objects_incomplete = 0;
    std::vector<MmtpFlowReport> mmtp_flows;        // first-seen order
    std::vector<UdpFlowCount> unclassified;        // UDP flows that are neither LLS, LCT nor MMTP

    double durationSeconds() const noexcept;
    /// The inventory as text (what atsc3scan prints).
    std::string text(const ScanPrint& print = {}) const;
};

/// Returns 0 on success, 2 on open errors, 3 when the capture carries no ATSC 3.0 content.
int scanCapture(const ScanOptions& opts, ScanReport& report, std::string* error);

}  // namespace atsc3::tools
