#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "atsc3/alp/decap.hpp"
#include "atsc3/io/inet.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/lls/processor.hpp"
#include "atsc3/util/gzip.hpp"
#include "corpus.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;
using namespace std::chrono_literals;

namespace {
Bytes table(std::uint8_t id, std::uint8_t group, std::uint8_t count_m1, std::uint8_t version, const std::string& xml, bool gz = true) {
    Bytes b{id, group, count_m1, version};
    const Bytes body(xml.begin(), xml.end());
    if (gz) {
        const Bytes z = util::gzipCompress(ByteView(body.data(), body.size()));
        b.insert(b.end(), z.begin(), z.end());
    } else {
        b.insert(b.end(), body.begin(), body.end());
    }
    return b;
}
ip::UdpDatagram datagram(const Bytes& payload, std::uint32_t src = addr(10, 1, 1, 1), std::int64_t t = 1) {
    ip::UdpDatagram d;
    d.src_ip = src;
    d.dst_ip = lls::kLlsAddress;
    d.dst_port = lls::kLlsPort;
    d.plp = 0;
    d.time.t_ns = t;
    d.payload = ByteView(payload.data(), payload.size());
    return d;
}
struct Count {
    int slt = 0, stt = 0, aeat = 0, rrt = 0, omn = 0, any = 0, smt = 0, err = 0;
    std::vector<std::uint8_t> ids;
    lls::Callbacks cb() {
        lls::Callbacks c;
        c.onSlt = [this](const lls::TableInfo&, const lls::Slt&) { ++slt; };
        c.onSystemTime = [this](const lls::TableInfo&, const lls::SystemTime&) { ++stt; };
        c.onAeat = [this](const lls::TableInfo&, const lls::Aeat&) { ++aeat; };
        c.onRrt = [this](const lls::TableInfo&, const lls::Rrt&) { ++rrt; };
        c.onOmn = [this](const lls::TableInfo&, const lls::Omn&) { ++omn; };
        c.onTable = [this](const lls::TableInfo& i, ByteView) { ++any; ids.push_back(i.header.table_id); };
        c.onSignedMultiTable = [this](const lls::TableInfo&, const lls::SignedMultiTable&) { ++smt; };
        c.onError = [this](const std::string&) { ++err; };
        return c;
    }
};
const char* kSlt = "<SLT xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SLT/1.0/\" bsid=\"564\"><Service serviceId=\"4\" sltSvcSeqNum=\"1\" serviceCategory=\"1\" shortServiceName=\"WESH\">"
                   "<BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.2.1\" slsDestinationUdpPort=\"5021\" slsSourceIpAddress=\"10.133.16.20\"/></Service></SLT>";
const char* kStt = "<SystemTime xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SYSTIME/1.0/\" currentUtcOffset=\"37\" utcLocalOffset=\"-PT4H\" dsStatus=\"true\"/>";
}  // namespace

TEST_CASE("tables are registered by group and dedup by version and content", "[lls][processor]") {
    Count c;
    lls::Processor p(c.cb());
    REQUIRE(p.onDatagram(datagram(table(lls::kSlt, 1, 0, 7, kSlt))));
    REQUIRE(p.onDatagram(datagram(table(lls::kSlt, 1, 0, 7, kSlt))));  // repetition
    CHECK(c.slt == 1);
    CHECK(p.stats().duplicates == 1);
    REQUIRE(p.onDatagram(datagram(table(lls::kSlt, 1, 0, 8, kSlt))));  // new version, same content
    CHECK(c.slt == 2);
    REQUIRE(p.onDatagram(datagram(table(lls::kSlt, 2, 1, 8, kSlt))));  // another group
    CHECK(c.slt == 3);
    CHECK(p.groups() == std::vector<std::uint8_t>{1, 2});
    CHECK(p.announcedGroups() == 2);
    REQUIRE(p.slt(1));
    CHECK(p.slt(1)->bsid == std::vector<std::uint16_t>{564});
    CHECK(p.slt(1)->services[0].bss->sls_destination_port == 5021);
    CHECK_FALSE(p.slt(3));
    REQUIRE(p.onDatagram(datagram(table(lls::kSystemTime, 1, 0, 2, kStt, false))));  // uncompressed XML
    CHECK(c.stt == 1);
    REQUIRE(p.systemTime(1));
    CHECK(p.systemTime(1)->current_utc_offset == 37);
    // same version, changed content -> still delivered (hash differs)
    std::string stt2 = kStt;
    stt2.replace(stt2.find("37"), 2, "38");
    REQUIRE(p.onDatagram(datagram(table(lls::kSystemTime, 1, 0, 2, stt2))));
    CHECK(c.stt == 2);
    CHECK(p.systemTime(1)->current_utc_offset == 38);
    // raw tables: CDT and UserDefined and an unknown id
    REQUIRE(p.onDatagram(datagram(table(lls::kCdt, 1, 0, 1, "<CertificationData/>"))));
    REQUIRE(p.onDatagram(datagram(table(lls::kUserDefined, 1, 0, 1, "<UDST xmlns=\"tag:digicap.com,2018:XMLSchemas/ATSC3/UDS/0.1/\"/>"))));
    REQUIRE(p.onDatagram(datagram(table(0x42, 1, 0, 1, "<X/>"))));
    CHECK(c.any == 8);
    REQUIRE(p.table(1, lls::kUserDefined));
    CHECK(p.table(1, lls::kUserDefined)->info.root_namespace == "tag:digicap.com,2018:XMLSchemas/ATSC3/UDS/0.1/");
    CHECK(p.table(1, lls::kUserDefined)->info.raw_xml.find("UDST") != std::string::npos);
    // not LLS address -> ignored
    ip::UdpDatagram other = datagram(table(lls::kSlt, 1, 0, 9, kSlt));
    other.dst_port = 5000;
    CHECK_FALSE(p.onDatagram(other));
    // malformed: short, bad gzip, bad xml
    REQUIRE(p.onDatagram(datagram(Bytes{1, 2})));
    CHECK(p.stats().bad_header == 1);
    REQUIRE(p.onDatagram(datagram(Bytes{lls::kSlt, 1, 0, 10, 0x1F, 0x8B, 0x08, 0, 0, 0})));
    CHECK(p.stats().gunzip_failed == 1);
    REQUIRE(p.onDatagram(datagram(table(lls::kSlt, 1, 0, 11, "<SLT bsid=\"1\"><Service></SLT>"))));
    CHECK(p.stats().parse_failed == 1);
    CHECK(c.err >= 2);
    CHECK(c.slt == 3);
}

TEST_CASE("SignedMultiTable unpacks inner tables and exposes the signature", "[lls][processor][smt]") {
    Count c;
    lls::Processor p(c.cb());
    const Bytes z1 = util::gzipCompress(ByteView(reinterpret_cast<const std::uint8_t*>(kSlt), std::char_traits<char>::length(kSlt)));
    const Bytes z2 = util::gzipCompress(ByteView(reinterpret_cast<const std::uint8_t*>(kStt), std::char_traits<char>::length(kStt)));
    Bytes smt{lls::kSignedMultiTable, 0, 0, 1, 2};
    auto entry = [&](std::uint8_t id, std::uint8_t ver, const Bytes& body) {
        smt.push_back(id);
        smt.push_back(ver);
        smt.push_back(static_cast<std::uint8_t>(body.size() >> 8));
        smt.push_back(static_cast<std::uint8_t>(body.size() & 0xFF));
        smt.insert(smt.end(), body.begin(), body.end());
    };
    entry(lls::kSlt, 5, z1);
    entry(lls::kSystemTime, 2, z2);
    smt.push_back(0);
    smt.push_back(4);
    smt.insert(smt.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    lls::SignedMultiTable seen;
    lls::TableInfo smt_info;
    lls::Callbacks cb = c.cb();
    cb.onSignedMultiTable = [&](const lls::TableInfo& i, const lls::SignedMultiTable& s) { seen = s; smt_info = i; };
    lls::Processor p2(cb);
    REQUIRE(p2.onDatagram(datagram(smt)));
    REQUIRE(seen.entries.size() == 2);
    CHECK(seen.entries[0].table_id == lls::kSlt);
    CHECK(seen.entries[0].version == 5);
    CHECK(seen.signature_length == 4);
    CHECK(seen.signature == Bytes{0xDE, 0xAD, 0xBE, 0xEF});
    CHECK(c.slt == 1);
    CHECK(c.stt == 1);
    REQUIRE(p2.table(0, lls::kSlt));
    CHECK(p2.table(0, lls::kSlt)->info.in_signed_multi_table);
    CHECK(p2.table(0, lls::kSlt)->info.header.version == 5);
    CHECK(p2.slt(0)->services.size() == 1);
    // truncated entry
    Bytes bad(smt.begin(), smt.begin() + 12);
    REQUIRE(p2.onDatagram(datagram(bad)));
    CHECK(p2.stats().bad_header == 1);
}

TEST_CASE("corpus: lls-tables-alp.pcap through ALP decap, IP demux and the LLS processor", "[lls][corpus]") {
    const std::string path = corpus("lls-tables-alp.pcap");
    if (!exists(path)) SKIP("corpus not available");
    Count c;
    std::vector<std::pair<lls::TableInfo, lls::Slt>> slts;
    lls::Callbacks cb = c.cb();
    cb.onSlt = [&](const lls::TableInfo& i, const lls::Slt& s) { slts.emplace_back(i, s); };
    lls::Processor p(cb);
    ip::Demux demux;
    demux.subscribe(ip::FlowKey{lls::kLlsAddress, lls::kLlsPort}, [&](const ip::UdpDatagram& d) { p.onDatagram(d); });
    alp::Callbacks acb;
    acb.onIpv4 = [&](std::uint8_t plp, ByteView ipv, std::optional<std::uint8_t> sid) { demux.onIpv4(plp, ipv, io::UnitTime{}, sid); };
    alp::Decap decap(acb);
    auto r = pcap::PcapReader::open(path);
    REQUIRE(r);
    pcap::PcapRecord rec;
    while (r->next(rec)) decap.feed(0, ByteView(rec.data.data(), rec.data.size()));
    CHECK(demux.stats().udp == 8);
    CHECK(p.stats().datagrams == 8);
    CHECK(c.smt == 2);
    // SLT bsid 668 (SMT v0, group 0), SLT 564 (SMT v1, group 0), SLT 1409 v60 (group 1) -> three SLT deliveries
    REQUIRE(slts.size() == 3);
    CHECK(slts[0].second.bsid == std::vector<std::uint16_t>{668});
    CHECK(slts[0].first.in_signed_multi_table);
    CHECK(slts[1].second.bsid == std::vector<std::uint16_t>{564});
    CHECK(slts[1].second.services.size() == 4);
    CHECK(slts[2].second.bsid == std::vector<std::uint16_t>{1409});
    CHECK(slts[2].first.header.version == 60);
    CHECK(slts[2].first.header.group_id == 1);
    REQUIRE(slts[2].second.services.size() == 6);
    const lls::SltService* wnuv = nullptr;
    for (const auto& s : slts[2].second.services) if (s.short_service_name == std::optional<std::string>("WNUV")) wnuv = &s;
    REQUIRE(wnuv);
    CHECK(wnuv->bss->sls_protocol == 2);
    CHECK(io::ipv4ToString(wnuv->bss->sls_destination_ip) == "239.255.54.1");
    CHECK(wnuv->bss->sls_destination_port == 5001);
    CHECK(wnuv->service_id == 5007);
    CHECK(c.stt == 3);  // two in SMTs (group 0, v0 and v2) + group 1 http-namespace variant
    REQUIRE(p.systemTime(1));
    CHECK(p.systemTime(1)->current_utc_offset == 37);
    CHECK(p.table(1, lls::kSystemTime)->info.root_namespace == "http://www.atsc.org/XMLSchemas/ATSC3/Delivery/SYSTIME/1.0/");
    CHECK(c.aeat == 1);
    REQUIRE(p.table(0, lls::kCdt));
    CHECK(p.table(0, lls::kCdt)->info.raw_xml.find("CertificationData") != std::string::npos);
    REQUIRE(p.table(1, lls::kUserDefined));
    CHECK(p.groups() == std::vector<std::uint8_t>{0, 1});
    CHECK(p.stats().parse_failed == 0);
    CHECK(p.stats().gunzip_failed == 0);
    CHECK(c.err == 0);
}

TEST_CASE("corpus: UniSoft raw-IP capture through the virtual tuner, demux and LLS", "[lls][corpus]") {
    const std::string path = corpus("dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap");
    if (!exists(path)) SKIP("corpus not available");
    pcap::VirtualTunerConfig cfg;
    cfg.path = path;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = 4'000'000'000;
    pcap::VirtualTuner tuner(cfg);
    REQUIRE(tuner.openError() == service::TunerError::None);
    Count c;
    lls::Processor p(c.cb());
    ip::Demux demux;
    demux.subscribe(ip::FlowKey{lls::kLlsAddress, lls::kLlsPort}, [&](const ip::UdpDatagram& d) { p.onDatagram(d); });
    struct Sink final : service::ITunerSink {
        ip::Demux& d;
        explicit Sink(ip::Demux& dd) : d(dd) {}
        void onBbp(const io::BasebandPacket&) override {}
        void onAlp(const io::AlpPacket&) override {}
        void onIp(const io::IpDatagram& p) override { d.onIpv4(p.plp_id, p.bytes, p.time); }
        void onTunerEvent(const service::TunerEvent&) override {}
    } sink(demux);
    REQUIRE(tuner.start(sink, tuner.output()) == service::TunerError::None);
    REQUIRE(tuner.waitFinished(60s));
    CHECK(demux.stats().udp > 1000);
    CHECK(demux.stats().unrouted > 1000);  // ROUTE flows have no subscriber yet
    CHECK(demux.stats().ip_checksum_bad == 0);
    CHECK(p.stats().datagrams >= 3);
    CHECK(c.slt == 1);
    CHECK(c.stt == 1);
    CHECK(c.aeat == 1);
    CHECK(p.stats().parse_failed == 0);
    CHECK(c.err == 0);
    REQUIRE(p.slt(1));
    CHECK(p.slt(1)->bsid == std::vector<std::uint16_t>{24});
    CHECK(p.slt(1)->services.size() == 4);
    CHECK(p.table(1, lls::kSlt)->info.root_namespace.empty());  // SLT without namespace
    const auto un = demux.unroutedFlows();
    REQUIRE_FALSE(un.empty());
    CHECK(un[0].dst_port == 8000);
}
