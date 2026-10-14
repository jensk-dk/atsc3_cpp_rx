#pragma once
// STLTP capture → DLT 289 ALP capture (spec 6.1 `stltp2alp`): virtual tuner (STLTP mode, non-real-time)
// → BBP deframer → pcap writer. Optionally also writes the BBP container and filters one PLP.
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/phy/bbp.hpp"
#include "atsc3/stltp/demux.hpp"

namespace atsc3::tools {

struct Stltp2AlpOptions {
    std::string input;
    std::string output;                  // DLT 289 pcap
    std::optional<std::string> bbp_output;  // BBP container pcap (link type 147)
    std::optional<std::uint8_t> plp;     // keep only this PLP
    std::int64_t duration_ns = 0;        // 0 = whole file
    std::int64_t start_offset_ns = 0;
    bool force_stltp = false;            // skip detection
};

struct Stltp2AlpResult {
    pcap::VirtualTuner::Stats tuner;
    phy::DeframerStats deframer;
    std::map<std::uint8_t, std::uint64_t> bbp_per_plp, alp_per_plp;
    std::uint64_t alp_written = 0, bbp_written = 0;
    std::string detection;
};

/// Returns 0 on success, 2 on usage/open errors, 3 when the input is not STLTP.
int convertStltpToAlp(const Stltp2AlpOptions& opts, Stltp2AlpResult& result, std::string* error);

}  // namespace atsc3::tools
