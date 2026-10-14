#include <catch2/catch_test_macros.hpp>

#include "atsc3/alp/header.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

namespace {
alp::Header parse(const Bytes& b, alp::ParseStatus expect = alp::ParseStatus::Ok) {
    alp::Header h;
    REQUIRE(alp::parseHeader(ByteView(b.data(), b.size()), h) == expect);
    return h;
}
}  // namespace

TEST_CASE("single packet without additional header (sample bytes 05 4B)", "[alp][header]") {
    Bytes b{0x05, 0x4B};
    b.insert(b.end(), 1355, 0x45);
    const auto h = parse(b);
    CHECK(h.type == alp::PacketType::Ipv4);
    CHECK(h.layout == alp::Layout::Single);
    CHECK_FALSE(h.header_mode);
    CHECK(h.header_length == 2);
    CHECK(h.payload_length == 1355);
    CHECK(h.totalLength() == 1357);
    CHECK_FALSE(h.sid);
    CHECK_FALSE(h.extension);
}

TEST_CASE("zero-length vendor marker 08 00 05 F1 00 xx and 08 00 05 F0 07 + 8 bytes", "[alp][header]") {
    // Header::extension views into the input, so the buffers must outlive the header.
    const Bytes m1{0x08, 0x00, 0x05, 0xF1, 0x00, 0x04};
    const auto h1 = parse(m1);
    CHECK(h1.header_mode);
    CHECK(h1.payload_length == 0);
    CHECK(h1.header_length == 6);
    REQUIRE(h1.extension);
    CHECK(h1.extension->type == 0xF1);
    CHECK(h1.extension->bytes.size() == 1);
    CHECK(h1.extension->bytes[0] == 0x04);
    const Bytes m2{0x08, 0x00, 0x05, 0xF0, 0x07, 0xd8, 0xed, 0x13, 0x0f, 0xdd, 0x4b, 0xa6, 0xe4};
    const auto h2 = parse(m2);
    REQUIRE(h2.extension);
    CHECK(h2.extension->type == 0xF0);
    CHECK(h2.extension->bytes.size() == 8);
    CHECK(h2.extension->bytes[7] == 0xe4);
    CHECK(h2.header_length == 13);
    parse(Bytes{0x08, 0x00, 0x05, 0xF0}, alp::ParseStatus::NeedMore);
    parse(Bytes{0x08, 0x00, 0x05, 0xF1, 0x00}, alp::ParseStatus::NeedMore);  // extension byte missing
}

TEST_CASE("single packet with HM, long length, SID and no extension", "[alp][header]") {
    Bytes b{0x08, 0x34, 0x0E};  // length = (1<<11)|0x034 = 2100, SIF=1, HEF=0
    b.push_back(0x2A);         // SID
    b.insert(b.end(), 2100, 0);
    const auto h = parse(b);
    CHECK(h.payload_length == 2100);
    CHECK(h.header_length == 4);
    REQUIRE(h.sid);
    CHECK(*h.sid == 0x2A);
}

TEST_CASE("segmentation header fields", "[alp][header]") {
    // PC=1, S/C=0, length 100; add. header: SN=3, LSI=1, SIF=0, HEF=0 -> 0001 1100
    Bytes b{0x10, 0x64, 0x1C};
    b.insert(b.end(), 100, 1);
    const auto h = parse(b);
    CHECK(h.layout == alp::Layout::Segment);
    CHECK(h.segment_sn == 3);
    CHECK(h.last_segment);
    CHECK(h.payload_length == 100);
    CHECK(h.header_length == 3);
    parse(Bytes{0x10, 0x64, 0x04}, alp::ParseStatus::Invalid);  // SN 0 with LSI is forbidden
}

TEST_CASE("concatenation header with 2 and 3 components (stuffing bits)", "[alp][header]") {
    // count=0 -> 2 components; one explicit 12-bit length (0x064=100) + 4 stuffing bits; total 250
    Bytes b{0x18, 0xFA, 0x00, 0x06, 0x40};
    b.insert(b.end(), 250, 7);
    auto h = parse(b);
    CHECK(h.layout == alp::Layout::Concatenation);
    CHECK(h.component_count == 0);
    CHECK(h.header_length == 5);
    CHECK(h.component_lengths[0] == 100);
    CHECK(h.component_lengths[1] == 150);
    // count=1 -> 3 components; two explicit lengths 0x064, 0x032 (3 bytes, no stuffing); total 300
    Bytes c{0x19, 0x2C, 0x02, 0x06, 0x40, 0x32};
    c.insert(c.end(), 300, 7);
    h = parse(c);
    CHECK(h.component_count == 1);
    CHECK(h.header_length == 6);
    CHECK(h.component_lengths[0] == 100);
    CHECK(h.component_lengths[1] == 50);
    CHECK(h.component_lengths[2] == 150);
    Bytes bad{0x18, 0x64, 0x00, 0x06, 0x40};  // explicit 100 but total 100 -> implied 0
    bad.insert(bad.end(), 100, 0);
    parse(bad, alp::ParseStatus::Invalid);
}

TEST_CASE("link-layer signaling header (LMT) and type extension", "[alp][header]") {
    Bytes b{0x80, 0x10, 0x01, 0xFF, 0xFF, 0x38, 0x00};  // type 100, length 16, LMT type 1, ext FFFF, version 0x38, format 00 enc 00
    b.insert(b.end(), 16, 0);
    const auto h = parse(b);
    CHECK(h.type == alp::PacketType::Signaling);
    CHECK(h.header_length == 7);
    CHECK(h.signaling.type == 1);
    CHECK(h.signaling.type_extension == 0xFFFF);
    CHECK(h.signaling.version == 0x38);
    CHECK(h.signaling.format == 0);
    Bytes t{0xC0, 0x02, 0x12, 0x34, 0xAA, 0xBB};
    const auto h2 = parse(t);
    CHECK(h2.type == alp::PacketType::TypeExtension);
    CHECK(h2.extended_type == 0x1234);
    CHECK(h2.header_length == 4);
}

TEST_CASE("MPEG-2 TS encapsulation header forms", "[alp][header][ts]") {
    const auto h = parse(Bytes{0xE4});  // 111 0010 0 -> NUMTS 2, AHF 0
    CHECK(h.layout == alp::Layout::MpegTs);
    CHECK(h.num_ts == 2);
    CHECK(h.header_length == 1);
    CHECK(h.payload_length == 374);
    const auto h16 = parse(Bytes{0xE0});
    CHECK(h16.num_ts == 16);
    const auto hd = parse(Bytes{0xE7, 0x85});  // NUMTS 3, AHF 1, HDM 1, DNP 5
    CHECK(hd.ahf);
    CHECK(hd.hdm);
    CHECK(hd.dnp == 5);
    CHECK(hd.header_length == 2);
    CHECK(hd.payload_length == 187 + 2 * 184);
    parse(Bytes{0xE7}, alp::ParseStatus::NeedMore);
}

TEST_CASE("truncated and reserved headers", "[alp][header]") {
    parse(Bytes{}, alp::ParseStatus::NeedMore);
    parse(Bytes{0x05}, alp::ParseStatus::NeedMore);
    parse(Bytes{0x08, 0x00}, alp::ParseStatus::NeedMore);
    const auto r = parse(Bytes{0x20, 0x01, 0x00});
    CHECK(r.type == alp::PacketType::Reserved1);
}
