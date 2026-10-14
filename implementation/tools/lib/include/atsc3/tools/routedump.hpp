#pragma once
// routedump (spec 6.1): pcap (STLTP / ALP / BBP / raw IP) → per-service directories
// with the SLS fragments, every recovered ROUTE object (media segments, NRT files, unpacked packages)
// and a static MPD that plays the captured segments offline (VLC, dash.js, ffmpeg).
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atsc3/alp/decap.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/ip/demux.hpp"
#include "atsc3/lls/processor.hpp"
#include "atsc3/phy/bbp.hpp"

namespace atsc3::tools {

struct RouteDumpOptions {
    std::string input;
    std::string out_dir;
    std::int64_t duration_ns = 0;        // 0 = whole file
    std::int64_t start_offset_ns = 0;
    std::set<std::uint16_t> services;    // empty = all
    bool write_nrt = true;               // non-media objects and packages
    bool unpack_packages = true;         // multipart/related packages -> <name>.d/<part location>
    bool write_sls = true;               // sls/ directory with fragments and raw bundles
    bool all_flows = false;              // also capture ROUTE flows not announced by any SLT (not implemented)
    std::string sidecar = "auto";
    bool verbose = false;
};

struct RepresentationReport {
    std::string rep_id;
    std::optional<std::string> content_type;
    std::size_t segments = 0;
    bool init = false;
    std::uint64_t first_key = 0, last_key = 0;
    std::size_t gaps = 0;
};

struct ServiceReport {
    std::uint8_t lls_group = 0;
    std::uint16_t bsid = 0;
    std::uint16_t service_id = 0;
    std::string name;
    std::string dir;
    std::string protocol;             // "ROUTE", "MMTP", "broadband", "none"
    bool skipped = false;
    std::string skip_reason;
    std::size_t sls_updates = 0;
    bool have_usbd = false, have_stsid = false, have_mpd = false, have_held = false;
    std::size_t media_objects = 0, nrt_objects = 0, object_errors = 0;
    std::uint64_t media_bytes = 0, nrt_bytes = 0;
    std::vector<RepresentationReport> representations;
    std::optional<std::string> mpd_path;  // the static MPD, when written
    std::string mpd_error;
    std::vector<std::string> warnings;
};

struct RouteDumpResult {
    std::string detection;
    pcap::CaptureFormat mode = pcap::CaptureFormat::Unsupported;
    pcap::VirtualTuner::Stats tuner;
    phy::DeframerStats deframer;
    alp::Stats decap;
    ip::DemuxStats demux;
    lls::ProcessorStats lls;
    std::vector<ServiceReport> services;
    std::size_t files_written = 0;
    std::uint64_t bytes_written = 0;
    std::string summary() const;
};

/// Returns 0 on success, 2 on usage/open/write errors, 3 when the input carries no ATSC 3.0 content,
/// 4 when no ROUTE service produced anything.
int routeDump(const RouteDumpOptions& opts, RouteDumpResult& result, std::string* error);

/// Makes a file-system safe relative path from a Content-Location (scheme/host and ".." removed).
std::string safeRelativePath(const std::string& content_location);

}  // namespace atsc3::tools
