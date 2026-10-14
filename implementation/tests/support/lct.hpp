#pragma once
// LCT/ROUTE packet builder for tests (V=1, C=0, S=1, O=1, H=0; optional EXT_TOL/EXT_FTI, close flags).
#include <cstdint>
#include <optional>
#include <vector>

#include "atsc3/tools/packets.hpp"

namespace atsc3::test {

struct LctSpec {
    std::uint32_t tsi = 0, toi = 0;
    std::uint8_t codepoint = 1;
    std::uint32_t start_offset = 0;
    std::optional<std::uint32_t> tol24;            // EXT_TOL HET 194
    std::optional<std::uint64_t> fti_transfer_len; // EXT_FTI HET 64 (HEL 4)
    bool close_object = false, close_session = false;
    bool dataless = false;
};

inline tools::Bytes lct(const LctSpec& s, const tools::Bytes& payload = {}) {
    using tools::Bytes;
    Bytes b;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    std::uint8_t hdr_words = 4;
    if (s.tol24) hdr_words = static_cast<std::uint8_t>(hdr_words + 1);
    if (s.fti_transfer_len) hdr_words = static_cast<std::uint8_t>(hdr_words + 4);
    std::uint8_t flags = 0xA0;  // S=1 O=1
    if (s.close_session) flags |= 0x02;
    if (s.close_object) flags |= 0x01;
    b.push_back(0x12);  // V=1 C=0 PSI=10
    b.push_back(flags);
    b.push_back(hdr_words);
    b.push_back(s.codepoint);
    put32(0);  // CCI
    put32(s.tsi);
    put32(s.toi);
    if (s.tol24) {
        b.push_back(194);
        b.push_back(static_cast<std::uint8_t>(*s.tol24 >> 16));
        b.push_back(static_cast<std::uint8_t>(*s.tol24 >> 8));
        b.push_back(static_cast<std::uint8_t>(*s.tol24));
    }
    if (s.fti_transfer_len) {
        b.push_back(64);
        b.push_back(4);
        for (int i = 5; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(*s.fti_transfer_len >> (8 * i)));
        b.push_back(0);
        b.push_back(0);                 // reserved / FEC instance id
        b.push_back(0x05);
        b.push_back(0x3C);              // ESL 1340
        put32(0);                       // MSBL
    }
    if (s.dataless) return b;
    put32(s.start_offset);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

}  // namespace atsc3::test
