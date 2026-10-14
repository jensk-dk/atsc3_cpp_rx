#pragma once
// atsc3:// MRL and option parsing (spec 5.19.1). VLC-free so it can be unit-tested with the stack's test suite.
//   atsc3://<path-to-capture>[?opt=value&...]      pcap/pcapng through the virtual tuner
//   atsc3://udp@<group>:<port>[?iface=...]         live UDP (recognised, not implemented yet)
//   atsc3://tuner/<name>[?freq=...&plp=...]        RF tuner (recognised, not implemented yet)
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace atsc3::vlcplugin {

struct Mrl {
    enum class Kind { Capture, Udp, Tuner, Invalid };
    Kind kind = Kind::Invalid;
    std::string target;                            // capture path, "group:port" or tuner name
    std::map<std::string, std::string> options;    // query terms, percent-decoded
    std::string error;
};

/// Parses what VLC hands over after "atsc3://" (demux_t::psz_location). A leading "/" lost by VLC's MRL split is
/// restored when the path does not exist as given but does with it.
Mrl parseMrl(const std::string& location);

/// Resolved options: MRL query terms override the VLC variables the caller passes as defaults.
struct Options {
    std::string service;             // "" | serviceId | "major.minor"
    std::optional<std::uint16_t> bsid;
    bool realtime = true;
    double speed = 1.0;
    bool loop = false;
    double start_s = 0.0;
    std::string sidecar = "auto";    // "auto" | "off" | path
    bool esg = true;
    std::string alerts = "log";      // "osd" | "log" | "off"
    int live_delay_ms = 0;           // 0 = 2 x the longest segment duration seen (min 1500 ms)
    bool verbose_stats = false;
    bool show_hidden = false;
    int program = 0;                 // VLC --program (0 = unset)
};
/// Applies the MRL query terms on top of `o`. Unknown terms are listed in `unknown`.
void applyOptions(const Mrl& mrl, Options& o, std::string* unknown = nullptr);

/// "major.minor" -> (major, minor); nullopt when the text is not of that form.
std::optional<std::pair<std::uint16_t, std::uint16_t>> parseChannel(const std::string& text);

}  // namespace atsc3::vlcplugin
