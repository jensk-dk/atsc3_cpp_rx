#pragma once
// DekTec StreamXpress ".xa3" modulation profile as capture sidecar (spec 5.1.2 rule 8, Q8).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/io/units.hpp"
#include "atsc3/service/tuner.hpp"

namespace atsc3::pcap {

struct SidecarXa3 {
    std::string path;
    io::L1Info l1;
    /// PLP id per referenced pcap file (basename as written in Atsc3/Subframe/Plp/Source/PcapFile@filename).
    std::vector<std::pair<std::string, std::uint8_t>> plp_sources;

    /// PLP id for the given capture path (basename match), else the first PLP, else nullopt.
    std::optional<std::uint8_t> plpForCapture(const std::string& capture_path) const;
};

/// Parses an .xa3 file. Unknown content never fails; a malformed file returns nullopt with error.
std::optional<SidecarXa3> loadXa3(const std::string& path, std::string* error = nullptr);
/// Parses .xa3 XML from memory (tests).
std::optional<SidecarXa3> parseXa3(const std::string& xml, const std::string& path_for_messages,
                                   std::string* error = nullptr);

/// Auto-resolution: <capture>.xa3, <stem>.xa3, then any .xa3 in the directory naming the capture.
std::optional<std::string> findXa3Sidecar(const std::string& capture_path);

/// Basename of a Windows or POSIX path.
std::string pathBasename(const std::string& path);

}  // namespace atsc3::pcap
