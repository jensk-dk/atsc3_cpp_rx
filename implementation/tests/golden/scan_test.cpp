// Golden inventory of the corpus (spec Section 9 item 2 and the large-capture baselines): atsc3scan's report on every
// capture, asserted number by number. The values are what the capture holds, independent of the receiver's policies,
// so they are the regression baseline for the tuner, deframer, decapsulation, demux, LLS, LCT and MMTP parsers.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "atsc3/io/inet.hpp"
#include "atsc3/tools/scan.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {

tools::ScanReport scan(const std::string& file, std::uint64_t limit = 0) {
    tools::ScanOptions o;
    o.input = corpus(file);
    o.max_records = limit;
    o.keep_sls_objects = false;
    tools::ScanReport r;
    std::string err;
    REQUIRE(tools::scanCapture(o, r, &err) == 0);
    return r;
}

std::uint32_t addr4(const char* dotted) {
    std::uint32_t v = 0;
    REQUIRE(io::parseIpv4(dotted, v));
    return v;
}

const tools::LctFlowReport& lctFlow(const tools::ScanReport& r, const char* dst, std::uint16_t port) {
    const std::uint32_t d = addr4(dst);
    const auto it = std::find_if(r.lct_flows.begin(), r.lct_flows.end(), [&](const tools::LctFlowReport& f) { return f.dst == d && f.port == port; });
    REQUIRE(it != r.lct_flows.end());
    return *it;
}

const tools::MmtpFlowReport& mmtpFlow(const tools::ScanReport& r, const char* dst, std::uint16_t port) {
    const std::uint32_t d = addr4(dst);
    const auto it = std::find_if(r.mmtp_flows.begin(), r.mmtp_flows.end(), [&](const tools::MmtpFlowReport& f) { return f.dst == d && f.port == port; });
    REQUIRE(it != r.mmtp_flows.end());
    return *it;
}

const tools::UdpFlowCount& udpFlow(const tools::ScanReport& r, const char* src, const char* dst, std::uint16_t port) {
    const std::uint32_t s = addr4(src), d = addr4(dst);
    const auto it = std::find_if(r.udp_flows.begin(), r.udp_flows.end(),
                                 [&](const tools::UdpFlowCount& f) { return f.src == s && f.dst == d && f.port == port; });
    REQUIRE(it != r.udp_flows.end());
    return *it;
}

std::uint64_t at(const std::map<std::string, std::uint64_t>& m, const std::string& k) {
    const auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
}
template <class K>
std::uint64_t at(const std::map<K, std::uint64_t>& m, K k) {
    const auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
}

std::size_t lmtEntries(const tools::LmtReport& l) {
    std::size_t n = 0;
    for (const link::LmtPlp& p : l.lmt.plps) n += p.multicasts.size();
    return n;
}

}  // namespace

TEST_CASE("alp-sample1: AEAT, one LMT of three PLPs, an MMTP flow and one partial ROUTE object", "[golden][corpus][scan]") {
    if (!exists(corpus("alp-sample1.pcap"))) SKIP("corpus not available");
    const auto r = scan("alp-sample1.pcap");
    CHECK(r.mode == pcap::CaptureFormat::Alp);
    CHECK(r.output == service::TunerOutput::Alp);
    CHECK(r.tuner.records == 17);
    CHECK(r.alp.packets == 17);
    CHECK(at(r.alp.types, "IPv4") == 16);
    CHECK(at(r.alp.types, "signaling") == 1);
    CHECK(at(r.alp.layouts, "single") == 17);
    CHECK(at(r.alp.extensions, "0xf1 00") == 3);   // vendor-private zero-length packets (spec 3, finding 2)
    CHECK(at(r.alp.extensions, "0xf1 04") == 6);
    CHECK(r.alp.odd == 9);
    CHECK(r.alp.decap.zero_length == 9);
    REQUIRE(r.lmts.size() == 1);
    CHECK(r.lmts[0].lmt.version == 2);
    CHECK(r.lmts[0].lmt.plps.size() == 3);
    CHECK(lmtEntries(r.lmts[0]) == 56);
    CHECK(r.udp_flows.size() == 4);
    CHECK(udpFlow(r, "172.16.200.1", "239.255.54.1", 5001).packets == 3);
    CHECK(udpFlow(r, "172.16.200.1", "239.255.54.1", 5001).bytes == 3503);
    CHECK(udpFlow(r, "172.16.200.1", "224.0.23.60", 4937).bytes == 1355);
    REQUIRE(r.lls_tables.size() == 1);
    CHECK(r.lls_tables[0].table_id == lls::kAeat);
    CHECK(r.lls_tables[0].group == 1);
    CHECK(r.lls_tables[0].version == 38);
    CHECK(r.lls_tables[0].length == 5529);
    REQUIRE(r.lct_flows.size() == 1);
    CHECK(at(lctFlow(r, "239.255.45.1", 5002).tsi, std::uint64_t{100}) == 2);
    CHECK(at(lctFlow(r, "239.255.45.1", 5002).codepoints, std::uint8_t{8}) == 2);
    CHECK(lctFlow(r, "239.255.45.1", 5002).objects == 1);
    REQUIRE(r.mmtp_flows.size() == 1);
    CHECK(mmtpFlow(r, "239.255.54.1", 5001).packets == 3);
    CHECK(at(mmtpFlow(r, "239.255.54.1", 5001).packet_ids, std::uint16_t{100}) == 2);
    CHECK(at(mmtpFlow(r, "239.255.54.1", 5001).packet_ids, std::uint16_t{200}) == 1);
    REQUIRE(r.unclassified.size() == 1);
    CHECK(r.unclassified[0].port == 5000);
    CHECK(r.objects_complete == 0);
    CHECK(r.objects_incomplete == 1);
}

TEST_CASE("alp-sample2: SignedMultiTable, CDT, NRT carousel objects and partial media", "[golden][corpus][scan]") {
    if (!exists(corpus("alp-sample2.pcap"))) SKIP("corpus not available");
    const auto r = scan("alp-sample2.pcap");
    CHECK(r.tuner.records == 85);
    CHECK(at(r.alp.types, "IPv4") == 84);
    CHECK(at(r.alp.types, "signaling") == 1);
    CHECK(at(r.alp.extensions, "0xf0 d8ea8cf8c266f1bc") == 1);
    REQUIRE(r.lmts.size() == 1);
    CHECK(r.lmts[0].lmt.version == 56);
    CHECK(lmtEntries(r.lmts[0]) == 4);
    CHECK(r.udp_flows.size() == 3);
    CHECK(udpFlow(r, "10.133.16.20", "239.255.18.1", 5181).packets == 75);
    CHECK(udpFlow(r, "10.133.16.20", "239.255.18.1", 5181).bytes == 112500);
    REQUIRE(r.signed_multi_tables.size() == 1);
    CHECK(r.signed_multi_tables[0].group == 0);
    CHECK(r.signed_multi_tables[0].version == 1);
    CHECK(r.signed_multi_tables[0].entries.size() == 2);
    CHECK(r.signed_multi_tables[0].signature_length == 664);
    REQUIRE(r.lls_tables.size() == 3);
    CHECK(r.lls_tables[0].table_id == lls::kSlt);
    CHECK(r.lls_tables[0].in_signed_multi_table);
    CHECK(r.lls_tables[0].length == 1386);   // inflated (426 bytes gzip on air)
    CHECK(r.lls_tables[0].text.find("bsid=\"564\"") != std::string::npos);
    CHECK(r.lls_tables[1].table_id == lls::kSystemTime);
    CHECK(r.lls_tables[2].table_id == lls::kCdt);
    CHECK(r.lls_tables[2].version == 74);
    CHECK(r.lls_tables[2].length == 11233);
    REQUIRE(r.lct_flows.size() == 2);
    const auto& media = lctFlow(r, "239.255.18.1", 5181);
    CHECK(at(media.tsi, std::uint64_t{10}) == 69);
    CHECK(at(media.tsi, std::uint64_t{20}) == 2);
    CHECK(at(media.tsi, std::uint64_t{30}) == 4);
    CHECK(at(media.hets, std::uint8_t{194}) == 75);
    CHECK(media.objects == 3);
    const auto& nrt = lctFlow(r, "239.255.2.255", 8000);
    CHECK(at(nrt.codepoints, std::uint8_t{1}) == 6);
    CHECK(nrt.objects == 6);
    CHECK(r.objects_complete == 6);
    CHECK(r.objects_incomplete == 3);
}

TEST_CASE("ch11-25-slt: two broadcasters' tables, a truncated ALP packet, twelve LCT flows", "[golden][corpus][scan]") {
    if (!exists(corpus("ch11-25-slt.pcap"))) SKIP("corpus not available");
    const auto r = scan("ch11-25-slt.pcap");
    CHECK(r.tuner.records == 68);
    CHECK(at(r.alp.types, "IPv4") == 64);
    CHECK(at(r.alp.types, "signaling") == 4);
    CHECK(r.alp.odd == 2);
    REQUIRE(r.alp.odd_packets.size() == 2);
    CHECK(r.alp.odd_packets[0].index == 6);
    CHECK(r.alp.odd_packets[0].length == 1500);
    CHECK(r.alp.odd_packets[0].trailing == -2);
    CHECK(r.alp.decap.truncated == 1);
    CHECK(r.link.lmt == 4);
    CHECK(r.lmts.size() == 2);
    CHECK(r.udp_flows.size() == 26);
    CHECK(r.lls.new_tables == 5);
    CHECK(r.lls.duplicates == 3);
    CHECK(r.lls_tables.size() == 5);
    CHECK(r.lct_flows.size() == 12);
    const auto& f = lctFlow(r, "239.255.45.1", 5002);
    CHECK(f.packets == 7);
    CHECK(at(f.tsi, std::uint64_t{100}) == 4);
    CHECK(at(f.codepoints, std::uint8_t{3}) == 3);
    CHECK(f.objects == 4);
    REQUIRE(r.mmtp_flows.size() == 1);
    CHECK(mmtpFlow(r, "239.255.54.1", 5001).packets == 3);
    CHECK(r.unclassified.size() == 11);
    CHECK(r.objects_complete == 6);
    CHECK(r.objects_incomplete == 16);
}

TEST_CASE("lls-tables-alp: three sources, two SignedMultiTables in group 0 from different broadcasters", "[golden][corpus][scan]") {
    if (!exists(corpus("lls-tables-alp.pcap"))) SKIP("corpus not available");
    const auto r = scan("lls-tables-alp.pcap");
    CHECK(r.tuner.records == 8);
    CHECK(r.udp_flows.size() == 3);
    CHECK(r.lls.datagrams == 8);
    CHECK(r.lls.new_tables == 10);
    CHECK(r.lls_tables.size() == 10);
    REQUIRE(r.signed_multi_tables.size() == 2);
    CHECK(r.signed_multi_tables[0].version == 0);
    CHECK(r.signed_multi_tables[0].source_ip == addr4("10.4.49.5"));
    CHECK(r.signed_multi_tables[1].version == 1);
    CHECK(r.signed_multi_tables[1].source_ip == addr4("10.133.16.20"));
    std::size_t slts = 0;
    for (const tools::LlsTableReport& t : r.lls_tables)
        if (t.table_id == lls::kSlt) ++slts;
    CHECK(slts == 3);   // bsid 668, bsid 564 (group 0) and the group 1 SLT
    CHECK(r.lct_flows.empty());
    CHECK(r.objects_complete == 0);
    CHECK(r.objects_incomplete == 0);
}

TEST_CASE("mp4-ftyp-styp-sidx: EXT_FTI flows, a close-object flag and two short objects", "[golden][corpus][scan]") {
    if (!exists(corpus("mp4-ftyp-styp-sidx.pcap"))) SKIP("corpus not available");
    const auto r = scan("mp4-ftyp-styp-sidx.pcap");
    CHECK(r.tuner.records == 5);
    REQUIRE(r.lct_flows.size() == 3);
    CHECK(lctFlow(r, "239.255.24.1", 5241).close_object == 1);
    CHECK(at(lctFlow(r, "239.255.24.1", 5241).hets, std::uint8_t{194}) == 2);
    CHECK(at(lctFlow(r, "239.255.45.1", 5002).hets, std::uint8_t{64}) == 2);
    CHECK(at(lctFlow(r, "239.255.45.1", 5002).header_shapes, "0,1,1,0,8") == 2);
    CHECK(r.objects_complete == 3);
    CHECK(r.objects_incomplete == 2);
    std::size_t contiguous_short = 0;
    for (const tools::ObjectReport& o : r.objects)
        if (o.contiguous && !o.complete) ++contiguous_short;
    CHECK(contiguous_short == 2);
}

TEST_CASE("UniSoft 3xROUTE, first 120 000 packets: 296 complete / 15 incomplete objects (spec Section 9)", "[golden][corpus][scan]") {
    if (!exists(corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap"))) SKIP("corpus not available");
    const auto r = scan("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap", 120000);
    CHECK(r.mode == pcap::CaptureFormat::IpBypass);
    CHECK(r.output == service::TunerOutput::Ip);
    CHECK(r.tuner.records == 120000);
    CHECK(r.tuner.delivered_ip == 120000);
    REQUIRE(r.l1);
    REQUIRE(r.l1->plps.size() == 1);
    CHECK(r.l1->plps[0].modulation == "qam256");
    CHECK(r.udp_flows.size() == 5);
    CHECK(udpFlow(r, "10.5.120.103", "239.255.24.1", 8000).packets == 55539);
    CHECK(udpFlow(r, "10.5.120.103", "239.255.24.1", 8000).bytes == 81376839);
    CHECK(udpFlow(r, "10.5.120.103", "224.0.23.60", 4937).packets == 190);
    CHECK(r.lls.new_tables == 3);
    CHECK(r.lls.duplicates == 187);
    REQUIRE(r.lct_flows.size() == 4);
    const auto& usc03 = lctFlow(r, "239.255.24.1", 8000);
    CHECK(usc03.packets == 55539);
    CHECK(at(usc03.tsi, std::uint64_t{0}) == 415);
    CHECK(at(usc03.tsi, std::uint64_t{1}) == 50474);
    CHECK(at(usc03.tsi, std::uint64_t{2}) == 2874);
    CHECK(at(usc03.tsi, std::uint64_t{4}) == 1776);
    CHECK(at(usc03.codepoints, std::uint8_t{130}) == 1776);
    CHECK(usc03.close_object == 2106);
    CHECK(usc03.objects == 89);
    CHECK(lctFlow(r, "239.255.24.99", 8000).objects == 5);
    CHECK(lctFlow(r, "239.255.24.2", 8000).objects == 87);
    CHECK(lctFlow(r, "239.255.24.3", 8000).objects == 130);
    CHECK(r.mmtp_flows.empty());
    CHECK(r.objects_complete == 296);
    CHECK(r.objects_incomplete == 15);
}

TEST_CASE("UniSoft 2xROUTE+1MMT, first 120 000 packets: MMTP flow, MP tables and USBD", "[golden][corpus][scan]") {
    if (!exists(corpus("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap"))) SKIP("corpus not available");
    const auto r = scan("dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap", 120000);
    CHECK(r.lct_flows.size() == 3);
    CHECK(r.objects_complete == 142);
    CHECK(r.objects_incomplete == 5);
    REQUIRE(r.mmtp_flows.size() == 1);
    const auto& m = mmtpFlow(r, "239.255.24.3", 8000);
    CHECK(m.packets == 49108);
    CHECK(at(m.packet_ids, std::uint16_t{0}) == 132);
    CHECK(at(m.packet_ids, std::uint16_t{20}) == 45743);
    CHECK(at(m.packet_ids, std::uint16_t{21}) == 3233);
    CHECK(at(m.payload_types, "MPU") == 48778);
    CHECK(at(m.payload_types, "signaling") == 330);
    CHECK(at(m.byte0, std::uint8_t{0x40}) == 49108);
    CHECK(at(m.mpu_fragment_types, std::uint8_t{0}) == 66);
    CHECK(at(m.mpu_fragment_types, std::uint8_t{1}) == 66);
    CHECK(at(m.mpu_fragment_types, std::uint8_t{2}) == 48646);
    CHECK(at(m.mpu_fragmentation, std::uint8_t{0}) == 3213);
    CHECK(at(m.mpu_fragmentation, std::uint8_t{1}) == 1937);
    CHECK(at(m.mpu_fragmentation, std::uint8_t{2}) == 41691);
    CHECK(at(m.mpu_fragmentation, std::uint8_t{3}) == 1937);
    CHECK(at(m.signaling_fragmentation, "0,0") == 330);
    CHECK(m.length_mismatch == 0);
    CHECK(m.sequence_gaps == 0);
    CHECK(at(m.messages, "pid 0: 0x0011") == 66);
    CHECK(at(m.messages, "pid 0: 0x8100") == 66);
    CHECK(at(m.messages, "pid 20: 0x0012") == 33);
    CHECK(at(m.messages, "pid 20: 0x0204") == 33);
    CHECK(at(m.messages, "pid 21: 0x020a") == 33);
    CHECK(at(m.atsc3_content_types, std::uint16_t{1}) == 66);
    REQUIRE(m.mp_tables.size() == 6);
    CHECK(m.mp_tables[0].table.table_id == 0x11);
    CHECK(m.mp_tables[0].table.package_id == "usc03m-1");
    REQUIRE(m.mp_tables[0].table.assets.size() == 2);
    CHECK(m.mp_tables[0].table.assets[0].assetIdText() == "audioasset02");
    CHECK(m.mp_tables[0].table.assets[0].asset_type == "ac-4");
    CHECK(m.mp_tables[0].table.assets[0].packetId() == 21);
    CHECK(m.mp_tables[0].table.assets[1].asset_type == "hev1");
    CHECK(m.mp_tables[0].table.assets[1].packetId() == 20);
    CHECK(m.mp_tables[1].table.table_id == 0x13);
    REQUIRE(m.mp_tables[1].table.assets.size() == 1);
    REQUIRE(m.mp_tables[1].table.assets[0].mpu_timestamps.size() == 1);
    CHECK(m.mp_tables[1].table.assets[0].mpu_timestamps[0].mpu_sequence_number == 996);
    REQUIRE(m.atsc3_messages.size() == 1);
    CHECK(m.atsc3_messages[0].message.service_id == 3);
    CHECK(m.atsc3_messages[0].message.uri == "usbd.xml");
    CHECK(m.atsc3_messages[0].message.compression == 2);
    CHECK(m.atsc3_messages[0].content_length == 783);
    const std::string head(m.atsc3_messages[0].message.content.begin(), m.atsc3_messages[0].message.content.begin() + 5);
    CHECK(head == "<?xml");
}

TEST_CASE("BWI STLTP, first 60 000 tunnel packets: BBPs per PLP, pointer facts, LMTs, six ROUTE flows, WNUV MMTP", "[golden][corpus][scan]") {
    if (!exists(corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"))) SKIP("corpus not available");
    const auto r = scan("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap", 60000);
    CHECK(r.mode == pcap::CaptureFormat::Stltp);
    CHECK(r.output == service::TunerOutput::Bbp);
    CHECK(r.tuner.records == 60000);
    REQUIRE(r.stltp);
    CHECK(r.stltp->outer_packets == 59999);
    CHECK(r.stltp->outer_seq_gaps == 0);
    CHECK(r.stltp->protocol_version == 0);
    CHECK(r.stltp->bbp == 13695);
    CHECK(r.stltp->preamble == 107);
    CHECK(r.stltp->tmp == 107);
    CHECK(r.stltp->bpps_seq_gap == 0);
    CHECK(at(r.bbp.per_plp, std::uint8_t{0}) == 642);
    CHECK(at(r.bbp.per_plp, std::uint8_t{1}) == 3209);
    CHECK(at(r.bbp.per_plp, std::uint8_t{2}) == 9844);
    CHECK(at(r.bbp.lengths, std::size_t{5916}) == 13695);
    CHECK(at(r.bbp.ofi, std::uint8_t{2}) == 13695);
    CHECK(r.bbp.pointer_zero == 323);
    CHECK(r.bbp.pointer_no_start == 1441);
    CHECK(r.bbp.pointer_other == 11931);
    CHECK(r.bbp.deframer.alp == 52947);
    CHECK(r.bbp.deframer.orphan_prefix == 2);
    CHECK(r.bbp.deframer.pending_mismatch == 0);
    CHECK(r.bbp.deframer.counter_jumps == 0);
    CHECK(r.alp.packets == 52947);
    CHECK(at(r.alp.types, "IPv4") == 52626);
    CHECK(at(r.alp.types, "signaling") == 321);
    CHECK(r.alp.odd == 0);
    CHECK(r.link.lmt == 321);
    CHECK(r.lmts.size() == 3);   // one LMT per carrier PLP
    CHECK(r.udp_flows.size() == 23);
    CHECK(udpFlow(r, "172.16.200.1", "239.255.11.1", 5003).packets == 12037);
    CHECK(udpFlow(r, "172.16.200.1", "239.255.11.1", 5003).bytes == 17182836);
    CHECK(r.lls.new_tables == 4);
    REQUIRE(r.lct_flows.size() == 6);
    CHECK(lctFlow(r, "239.255.2.1", 5004).objects == 114);
    CHECK(lctFlow(r, "239.255.11.1", 5003).objects == 119);
    CHECK(lctFlow(r, "239.255.22.1", 5006).objects == 117);
    CHECK(lctFlow(r, "239.255.54.1", 5100).objects == 10);
    CHECK(lctFlow(r, "239.255.54.1", 15007).objects == 7);
    CHECK(lctFlow(r, "239.255.45.1", 5002).objects == 120);
    CHECK(at(lctFlow(r, "239.255.2.1", 5004).hets, std::uint8_t{64}) == 536);
    CHECK(at(lctFlow(r, "239.255.2.1", 5004).header_shapes, "0,1,1,0,4") == 11176);
    REQUIRE(r.mmtp_flows.size() == 1);
    const auto& m = mmtpFlow(r, "239.255.54.1", 5001);
    CHECK(m.packets == 6147);
    CHECK(at(m.packet_ids, std::uint16_t{100}) == 3769);
    CHECK(at(m.packet_ids, std::uint16_t{300}) == 183);
    CHECK(at(m.payload_types, "MPU") == 5514);
    CHECK(at(m.payload_types, "signaling") == 633);
    CHECK(at(m.byte0, std::uint8_t{0x61}) == 318);
    CHECK(at(m.byte0, std::uint8_t{0x62}) == 5829);
    CHECK(at(m.messages, "pid 0: 0x8100") == 265);
    CHECK(at(m.messages, "pid 0: 0x0020") == 53);
    CHECK(at(m.messages, "pid 100: 0x0012") == 54);
    for (const int ct : {1, 3, 5, 8, 9}) CHECK(at(m.atsc3_content_types, static_cast<std::uint16_t>(ct)) == 53);
    CHECK(m.atsc3_messages.size() == 5);
    CHECK(r.unclassified.size() == 15);
    CHECK(r.objects_complete == 461);
    CHECK(r.objects_incomplete == 26);
}

TEST_CASE("BWI STLTP, first 320 000 tunnel packets: the spec Section 9 baseline", "[golden][corpus][scan]") {
    if (!exists(corpus("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"))) SKIP("corpus not available");
    const auto r = scan("dektec/ATSC3-STLTP/BWI_20230307_1742.pcap", 320000);
    REQUIRE(r.stltp);
    CHECK(r.stltp->outer_seq_gaps == 0);
    CHECK(r.stltp->bpps_seq_gap == 0);
    CHECK(r.tuner.delivered_bbp == 73046);
    CHECK(at(r.bbp.per_plp, std::uint8_t{0}) == 3424);
    CHECK(at(r.bbp.per_plp, std::uint8_t{1}) == 17120);
    CHECK(at(r.bbp.per_plp, std::uint8_t{2}) == 52502);
    CHECK(r.bbp.invalid == 0);
    CHECK(r.bbp.deframer.pending_mismatch == 0);
    CHECK(r.bbp.deframer.alp_invalid == 0);
    CHECK(r.alp.packets == 282642);
    CHECK(at(r.alp.types, "IPv4") == 280930);
    CHECK(at(r.alp.types, "signaling") == 1712);
    CHECK(mmtpFlow(r, "239.255.54.1", 5001).packets == 33018);
    CHECK(r.objects_complete == 2333);
    CHECK(r.objects_incomplete == 26);
}

TEST_CASE("captures without ATSC 3.0 content are refused with exit code 3", "[golden][scan]") {
    // LAN chatter only (SSDP and mDNS multicast), the same synthetic capture the format-detector test uses:
    // no LLS, LCT or MMTP, so the verdict is Unsupported and the scanner exits 3.
    std::vector<std::pair<std::int64_t, Bytes>> recs;
    const std::uint32_t src = addr(10, 5, 120, 103);
    recs.push_back({0, ethernet(ipv4Udp(src, addr(239, 255, 255, 250), 1900, 1900, Bytes(40, 'N')))});
    recs.push_back({1, ethernet(ipv4Udp(src, addr(224, 0, 0, 251), 5353, 5353, Bytes(40, 'M')))});
    const std::string lan = writePcap(recs, 1, "scan-lan.pcap");
    tools::ScanOptions o;
    o.input = lan;
    tools::ScanReport r;
    std::string err;
    CHECK(tools::scanCapture(o, r, &err) == 3);
    CHECK(r.mode == pcap::CaptureFormat::Unsupported);
    CHECK(!err.empty());
    std::remove(lan.c_str());
}
