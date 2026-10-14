#include <catch2/catch_test_macros.hpp>

#include "atsc3/route/lct.hpp"
#include "lct.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

TEST_CASE("LCT: plain HDR_LEN 4 header (on-air media packet shape)", "[route][lct]") {
    LctSpec spec;
    spec.tsi = 100;
    spec.toi = 1676551953;
    spec.codepoint = 8;
    spec.start_offset = 1340;
    const Bytes pkt = lct(spec, Bytes{1, 2, 3});
    route::LctPacket p;
    REQUIRE(route::parseLct(ByteView(pkt.data(), pkt.size()), p) == route::LctStatus::Ok);
    CHECK(p.header.version == 1);
    CHECK(p.header.psi == 2);
    CHECK(p.header.s == 1);
    CHECK(p.header.o == 1);
    CHECK(p.header.hdr_len_words == 4);
    CHECK(p.header.codepoint == 8);
    CHECK(p.header.tsi == 100);
    CHECK(p.header.toi == 1676551953);
    CHECK(p.header.header_bytes == 16);
    CHECK(p.start_offset == 1340);
    CHECK(p.payload.size() == 3);
    CHECK_FALSE(p.header.transfer_length);
    CHECK_FALSE(p.header.close_object);
}

TEST_CASE("LCT: EXT_TOL (HET 194) 24-bit transfer length, HDR_LEN 5", "[route][lct]") {
    LctSpec spec;
    spec.toi = 0x470001;
    spec.codepoint = 3;
    spec.tol24 = 597u;
    const Bytes pkt = lct(spec, Bytes(10, 0xAA));
    route::LctPacket p;
    REQUIRE(route::parseLct(ByteView(pkt.data(), pkt.size()), p) == route::LctStatus::Ok);
    CHECK(p.header.hdr_len_words == 5);
    REQUIRE(p.header.transfer_length);
    CHECK(*p.header.transfer_length == 597);
    CHECK(p.header.extensions.size() == 1);
    CHECK(p.header.extensions[0].het == 194);
    CHECK(p.payload.size() == 10);
}

TEST_CASE("LCT: EXT_FTI transfer length is the first 48 bits (A/331, RFC 5775)", "[route][lct]") {
    // 40 04 | 00 00 00 00 02 55 | 00 00 | 05 3C | 00 00 00 00  -> TL 597, ESL 1340
    LctSpec s;
    s.tsi = 300;
    s.toi = 7;
    s.codepoint = 8;
    s.fti_transfer_len = 597;
    const Bytes pkt = lct(s, Bytes(4, 1));
    route::LctPacket p;
    REQUIRE(route::parseLct(ByteView(pkt.data(), pkt.size()), p) == route::LctStatus::Ok);
    CHECK(p.header.hdr_len_words == 8);
    REQUIRE(p.header.fti);
    CHECK(p.header.fti->transfer_length == 597);
    CHECK(p.header.fti->encoding_symbol_length == 1340);
    REQUIRE(p.header.transfer_length);
    CHECK(*p.header.transfer_length == 597);
}

TEST_CASE("LCT: close flags, dataless packets, EXT_TOL 48 and EXT_TIME", "[route][lct]") {
    SECTION("close object with data") {
        LctSpec s;
        s.close_object = true;
        s.tol24 = 5;
        const Bytes pkt = lct(s, Bytes(5, 9));
        route::LctPacket p;
        REQUIRE(route::parseLct(ByteView(pkt.data(), pkt.size()), p) == route::LctStatus::Ok);
        CHECK(p.header.close_object);
        CHECK_FALSE(p.dataless);
    }
    SECTION("dataless close-session packet") {
        LctSpec s;
        s.close_session = true;
        s.dataless = true;
        const Bytes pkt = lct(s);
        route::LctPacket p;
        REQUIRE(route::parseLct(ByteView(pkt.data(), pkt.size()), p) == route::LctStatus::Ok);
        CHECK(p.dataless);
        CHECK(p.header.close_session);
    }
    SECTION("EXT_TOL 48-bit (HET 67) and EXT_TIME SCT") {
        Bytes b{0x12, 0xA0, 9, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2};
        // HET 67 HEL 2: 48-bit length
        b.insert(b.end(), {67, 2, 0, 0, 0, 0x01, 0x00, 0x00});
        // EXT_TIME: HET 2 HEL 3, use bits SCT-High|SCT-Low, high=0x11223344 low=0x55667788
        b.insert(b.end(), {2, 3, 0xC0, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
        b.insert(b.end(), {0, 0, 0, 0, 0xEE});
        route::LctPacket p;
        REQUIRE(route::parseLct(ByteView(b.data(), b.size()), p) == route::LctStatus::Ok);
        REQUIRE(p.header.transfer_length);
        CHECK(*p.header.transfer_length == 0x10000);
        REQUIRE(p.header.sender_current_time_ntp);
        CHECK(*p.header.sender_current_time_ntp == 0x1122334455667788ull);
        CHECK(p.payload.size() == 1);
        CHECK(p.payload[0] == 0xEE);
    }
    SECTION("rejects version 0 and header longer than the packet") {
        Bytes b = lct(LctSpec{}, Bytes{1});
        b[0] = 0x02;
        route::LctPacket p;
        CHECK(route::parseLct(ByteView(b.data(), b.size()), p) == route::LctStatus::BadVersion);
        b[0] = 0x12;
        b[2] = 40;
        CHECK(route::parseLct(ByteView(b.data(), b.size()), p) == route::LctStatus::BadHeaderLength);
        const Bytes tiny{0x12, 0xA0};
        CHECK(route::parseLct(ByteView(tiny.data(), tiny.size()), p) == route::LctStatus::TooShort);
    }
}
