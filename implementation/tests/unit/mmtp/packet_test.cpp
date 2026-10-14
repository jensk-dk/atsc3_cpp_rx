#include <catch2/catch_test_macros.hpp>

#include "atsc3/mmtp/packet.hpp"
#include "atsc3/mmtp/signaling.hpp"
#include "atsc3/util/gzip.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

TEST_CASE("MMTP: version-01 header with the always-present QoS word (both encoders)", "[mmtp]") {
    // UniSoft: 40 00 | 00 14 | 24 32 28 f6 | 00 15 4d 22 | 80 00 | MPU payload header 05 14 2c 9b 00 00 03 e3 ...
    Bytes uni{0x40, 0x00, 0x00, 0x14, 0x24, 0x32, 0x28, 0xf6, 0x00, 0x15, 0x4d, 0x22, 0x80, 0x00};
    Bytes mpu{0x00, 0x0a, 0x2c, 0x9b, 0x00, 0x00, 0x03, 0xe3, 0xAA, 0xBB, 0xCC, 0xDD};  // length 10 = remaining 12 - 2
    uni.insert(uni.end(), mpu.begin(), mpu.end());
    mmtp::PacketHeader h;
    REQUIRE(mmtp::parsePacket(ByteView(uni.data(), uni.size()), h) == mmtp::ParseStatus::Ok);
    CHECK(h.version == 1);
    CHECK_FALSE(h.packet_counter_flag);
    CHECK(h.type == mmtp::PayloadType::Mpu);
    CHECK(h.packet_id == 20);
    CHECK(h.timestamp == 0x243228f6u);
    CHECK(h.packet_sequence_number == 0x00154d22u);
    REQUIRE(h.qos);
    CHECK(*h.qos == 0x8000);
    CHECK(h.header_bytes == 14);
    mmtp::MpuPayloadHeader m;
    REQUIRE(mmtp::parseMpuPayload(h.payload, m) == mmtp::ParseStatus::Ok);
    CHECK(m.length == 10);
    CHECK(m.fragment_type == 2);
    CHECK(m.timed);
    CHECK(m.fragmentation_indicator == 2);
    CHECK_FALSE(m.aggregation);
    CHECK(m.fragment_counter == 0x9b);
    CHECK(m.mpu_sequence_number == 0x3e3);
    CHECK(m.data.size() == 4);

    // Sinclair: 62 00 | 00 64 | ts | seq | packet_counter | a4 00 | ...
    Bytes sinc{0x62, 0x00, 0x00, 0x64, 0xca, 0x99, 0x38, 0x63, 0x32, 0x9b, 0x67, 0x3c, 0x51, 0xf9, 0x1d, 0x4b, 0xa4, 0x00, 0x00, 0x02, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    REQUIRE(mmtp::parsePacket(ByteView(sinc.data(), sinc.size()), h) == mmtp::ParseStatus::Ok);
    CHECK(h.packet_counter_flag);
    CHECK(h.flag_a);
    CHECK(h.packet_id == 100);
    REQUIRE(h.packet_counter);
    CHECK(*h.packet_counter == 0x51f91d4bu);
    CHECK(h.qos == 0xa400);
    CHECK(h.header_bytes == 18);

    // signaling packet: 61 02 | 00 00 | ... | 3c 00 | f_i=0 H=0 A=0 fc=0 | message
    Bytes sig{0x61, 0x02, 0x00, 0x00, 0xbc, 0x1c, 0x4b, 0xff, 0x02, 0x32, 0x68, 0x26, 0x2b, 0x10, 0xea, 0x7b, 0x9b, 0xff, 0x3c, 0x00, 0x81, 0x00, 0x01};
    REQUIRE(mmtp::parsePacket(ByteView(sig.data(), sig.size()), h) == mmtp::ParseStatus::Ok);
    CHECK(h.type == mmtp::PayloadType::Signaling);
    CHECK(h.flag_b);
    mmtp::SignalingPayloadHeader sp;
    REQUIRE(mmtp::parseSignalingPayload(h.payload, sp) == mmtp::ParseStatus::Ok);
    CHECK(sp.fragmentation_indicator == 0);
    CHECK_FALSE(sp.aggregation);
    CHECK(sp.data.size() == 3);

    Bytes v2{0x80, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(mmtp::parsePacket(ByteView(v2.data(), v2.size()), h) == mmtp::ParseStatus::BadVersion);
    Bytes shortp{0x40, 0x00, 0x00};
    CHECK(mmtp::parsePacket(ByteView(shortp.data(), shortp.size()), h) == mmtp::ParseStatus::TooShort);
}

namespace {
void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(Bytes& b, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
Bytes asset(const std::string& id, const char* type, std::uint16_t pid, bool default_flag, std::optional<std::uint32_t> timescale, bool with_ts_desc) {
    Bytes a;
    a.push_back(0);  // identifier_type
    put32(a, 0);     // scheme
    put32(a, static_cast<std::uint32_t>(id.size()));
    a.insert(a.end(), id.begin(), id.end());
    a.insert(a.end(), type, type + 4);
    std::uint8_t flags = default_flag ? 0x80 : 0x00;
    if (timescale) flags |= 0x01;
    a.push_back(flags);
    if (timescale) {
        a.push_back(1);  // clock relation id
        a.push_back(1);  // timescale flag
        put32(a, *timescale);
    }
    a.push_back(1);  // location_count
    a.push_back(0);  // type 0
    put16(a, pid);
    Bytes desc;
    if (with_ts_desc) {
        put16(desc, 0x0001);
        desc.push_back(12);
        put32(desc, 42);
        put32(desc, 0xe8000000u);
        put32(desc, 0x12345678u);
    }
    put16(a, static_cast<std::uint16_t>(desc.size()));
    a.insert(a.end(), desc.begin(), desc.end());
    return a;
}
}  // namespace

TEST_CASE("MMTP: MP table (complete with package id, subset without) and mmt_atsc3_message", "[mmtp]") {
    Bytes table;
    table.push_back(0x11);  // table_id complete
    table.push_back(3);     // version
    put16(table, 0);        // length (ignored)
    table.push_back(0x00);  // mode
    const std::string pkg = "usc03m-1";
    table.push_back(static_cast<std::uint8_t>(pkg.size()));
    table.insert(table.end(), pkg.begin(), pkg.end());
    put16(table, 0);        // descriptors length
    table.push_back(2);     // assets
    Bytes a1 = asset("videoasset01", "hev1", 20, true, 90000u, true);
    Bytes a2 = asset("audioasset01", "ac-4", 21, false, std::nullopt, false);
    table.insert(table.end(), a1.begin(), a1.end());
    table.insert(table.end(), a2.begin(), a2.end());
    Bytes msg{0x00, 0x11, 0x00};  // message id 0x0011, version 0
    put16(msg, static_cast<std::uint16_t>(table.size()));
    msg.insert(msg.end(), table.begin(), table.end());

    mmtp::Message m;
    std::string err;
    REQUIRE(mmtp::parseMessage(ByteView(msg.data(), msg.size()), m, &err));
    CHECK(m.message_id == 0x0011);
    REQUIRE(m.mp_table);
    CHECK(m.mp_table->complete());
    CHECK(m.mp_table->package_id == "usc03m-1");
    CHECK(m.mp_table->version == 3);
    REQUIRE(m.mp_table->assets.size() == 2);
    CHECK(m.mp_table->assets[0].assetIdText() == "videoasset01");
    CHECK(m.mp_table->assets[0].asset_type == "hev1");
    CHECK(m.mp_table->assets[0].default_asset);
    CHECK(m.mp_table->assets[0].timescale == 90000u);
    CHECK(m.mp_table->assets[0].packetId() == 20);
    REQUIRE(m.mp_table->assets[0].mpu_timestamps.size() == 1);
    CHECK(m.mp_table->assets[0].mpu_timestamps[0].mpu_sequence_number == 42);
    CHECK(m.mp_table->assets[0].mpu_timestamps[0].ntp == 0xe800000012345678ull);
    CHECK(m.mp_table->assets[1].asset_type == "ac-4");
    CHECK(m.mp_table->assets[1].packetId() == 21);
    CHECK_FALSE(m.mp_table->assets[1].default_asset);

    // subset table 0x12: no package id
    Bytes sub{0x12, 1, 0, 0, 0x00, 1};
    Bytes a3 = asset("videoasset01", "hev1", 20, false, std::nullopt, true);
    sub.insert(sub.end(), a3.begin(), a3.end());
    Bytes smsg{0x00, 0x12, 0x00};
    put16(smsg, static_cast<std::uint16_t>(sub.size()));
    smsg.insert(smsg.end(), sub.begin(), sub.end());
    REQUIRE(mmtp::parseMessage(ByteView(smsg.data(), smsg.size()), m, &err));
    REQUIRE(m.mp_table);
    CHECK_FALSE(m.mp_table->complete());
    CHECK(m.mp_table->package_id.empty());
    REQUIRE(m.mp_table->assets.size() == 1);
    CHECK(m.mp_table->assets[0].asset_type == "hev1");

    // mmt_atsc3_message with gzip USBD
    const std::string usbd = "<BundleDescriptionMMT><UserServiceDescription serviceId=\"3\"><Name lang=\"eng\">usc03m</Name>"
                             "<MPUComponent mmtPackageId=\"usc03m-1\"/><ComponentInfo componentType=\"1\" componentRole=\"0\" componentId=\"videoasset01\" componentName=\"Video\"/>"
                             "</UserServiceDescription></BundleDescriptionMMT>";
    const auto gz = util::gzipCompress(ByteView(reinterpret_cast<const std::uint8_t*>(usbd.data()), usbd.size()));
    Bytes am{0x81, 0x00, 0x00};
    Bytes body;
    put16(body, 3);        // service id
    put16(body, 0x0001);   // USBD
    body.push_back(7);     // content version
    body.push_back(2);     // gzip
    const std::string uri = "usbd.xml";
    body.push_back(static_cast<std::uint8_t>(uri.size()));
    body.insert(body.end(), uri.begin(), uri.end());
    put32(body, static_cast<std::uint32_t>(gz.size()));
    body.insert(body.end(), gz.begin(), gz.end());
    put32(am, static_cast<std::uint32_t>(body.size()));
    am.insert(am.end(), body.begin(), body.end());
    REQUIRE(mmtp::parseMessage(ByteView(am.data(), am.size()), m, &err));
    REQUIRE(m.atsc3);
    CHECK(m.atsc3->service_id == 3);
    CHECK(m.atsc3->content_type == 0x0001);
    CHECK(m.atsc3->content_version == 7);
    CHECK(m.atsc3->uri == "usbd.xml");
    CHECK(m.atsc3->inflated);
    CHECK(std::string(m.atsc3->content.begin(), m.atsc3->content.end()) == usbd);
    mmtp::MmtUsbd u;
    REQUIRE(mmtp::parseMmtUsbd(usbd, u, &err));
    CHECK(u.service_id == 3);
    CHECK(u.names[0].second == "usc03m");
    CHECK(u.mmt_package_id == "usc03m-1");
    REQUIRE(u.components.size() == 1);
    CHECK(u.components[0].component_type == 1);
    CHECK(u.components[0].component_id == "videoasset01");
    CHECK(u.components[0].component_name == "Video");
}
