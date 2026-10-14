#pragma once
// SLS bundle: the multipart/related (or multipart/signed) object carried on TSI 0 of the ROUTE
// SLS session, split into typed fragments (spec 5.9.1–5.9.2; A/331 §7.1.6, Annex C TOI bits).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/sls/fragments.hpp"
#include "atsc3/sls/mpd.hpp"

namespace atsc3::sls {

/// A/331 Annex C: TOI = G<<31 | R<<24 | D<<23 | H<<22 | A<<19 | M<<18 | S<<17 | U<<16 | version.
struct SlsToi {
    bool gzip = false;      // G
    std::uint8_t r = 0;     // reserved bits
    bool dwd = false;       // D
    bool held = false;      // H
    bool apd = false;       // A
    bool mpd = false;       // M
    bool stsid = false;     // S
    bool usbd = false;      // U
    std::uint16_t version = 0;
};
SlsToi decodeSlsToi(std::uint32_t toi) noexcept;

enum class FragmentKind { Envelope, Usbd, Stsid, Mpd, Held, Dwd, Apd, Rsat, Unknown };
const char* toString(FragmentKind k) noexcept;

struct SlsFragment {
    FragmentKind kind = FragmentKind::Unknown;
    std::string content_type;   // lower-case, no parameters
    std::string location;       // Content-Location (part header) or envelope match
    std::optional<std::uint32_t> version;  // from the envelope item
    std::string text;           // the part body
};

struct SlsBundle {
    std::uint32_t toi = 0;
    SlsToi toi_bits;
    bool is_signed = false;
    std::vector<std::uint8_t> signature;   // multipart/signed second part (verification is report-only)
    std::optional<Envelope> envelope;
    std::optional<Usbd> usbd;
    std::optional<Stsid> stsid;
    std::optional<Mpd> mpd;
    std::optional<Held> held;
    std::vector<SlsFragment> fragments;    // every part, in order
    std::vector<std::string> warnings;     // envelope items without a part, parse failures
    const SlsFragment* fragment(FragmentKind k) const;
};

/// Parses an SLS object (inflated when gzip). `toi` is informational (Annex C bits).
bool parseSlsBundle(ByteView object, std::uint32_t toi, SlsBundle& out, std::string* error = nullptr);

}  // namespace atsc3::sls
