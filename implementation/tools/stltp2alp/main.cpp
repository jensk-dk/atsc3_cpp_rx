// stltp2alp: convert an STLTP capture (A/324 tunnel, Ethernet/raw-IP pcap) into a DLT 289 ALP capture.
//
//   stltp2alp [options] <in.pcap> <out-alp.pcap>
//     --bbp <file>       also write the Baseband Packets as a BBP container pcap (link type 147)
//     --plp <n>          keep only PLP n
//     --start <sec>      skip the first <sec> seconds of capture time
//     --duration <sec>   convert only <sec> seconds of capture time
//     --force            skip format detection and treat the input as STLTP
//     -q                 quiet
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "atsc3/stack.hpp"
#include "atsc3/tools/stltp2alp.hpp"

namespace {
int usage() {
    std::fprintf(stderr,
                 "usage: stltp2alp [--bbp FILE] [--plp N] [--start SEC] [--duration SEC] [--force] [-q] <in.pcap> <out.pcap>\n");
    return 2;
}
std::int64_t secondsToNs(const char* s) { return static_cast<std::int64_t>(std::atof(s) * 1e9); }
}  // namespace

int main(int argc, char** argv) {
    atsc3::tools::Stltp2AlpOptions opts;
    bool quiet = false;
    std::string positional[2];
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--bbp") == 0 && i + 1 < argc) opts.bbp_output = argv[++i];
        else if (std::strcmp(a, "--plp") == 0 && i + 1 < argc) opts.plp = static_cast<std::uint8_t>(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--start") == 0 && i + 1 < argc) opts.start_offset_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--duration") == 0 && i + 1 < argc) opts.duration_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--force") == 0) opts.force_stltp = true;
        else if (std::strcmp(a, "-q") == 0) quiet = true;
        else if (std::strcmp(a, "--version") == 0) {
            std::printf("stltp2alp %s\n", atsc3::versionString().data());
            return 0;
        } else if (a[0] == '-') return usage();
        else if (npos < 2) positional[npos++] = a;
        else return usage();
    }
    if (npos != 2) return usage();
    opts.input = positional[0];
    opts.output = positional[1];

    atsc3::tools::Stltp2AlpResult res;
    std::string err;
    const int rc = atsc3::tools::convertStltpToAlp(opts, res, &err);
    if (rc != 0) {
        std::fprintf(stderr, "stltp2alp: %s\n", err.c_str());
        return rc;
    }
    if (!quiet) {
        std::printf("%s\n", res.detection.c_str());
        std::printf("records=%llu bbp=%llu alp=%llu written=%llu", static_cast<unsigned long long>(res.tuner.records),
                    static_cast<unsigned long long>(res.tuner.delivered_bbp),
                    static_cast<unsigned long long>(res.deframer.alp), static_cast<unsigned long long>(res.alp_written));
        if (opts.bbp_output) std::printf(" bbp_written=%llu", static_cast<unsigned long long>(res.bbp_written));
        std::printf("\nper PLP:");
        for (const auto& [plp, n] : res.bbp_per_plp)
            std::printf(" plp%u: bbp=%llu alp=%llu", plp, static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(res.alp_per_plp[plp]));
        std::printf("\ndeframer: pointer_no_start=%llu orphan_prefix=%llu pending_mismatch=%llu alp_invalid=%llu "
                    "counter_jumps=%llu bbp_invalid=%llu flushed=%llu\n",
                    static_cast<unsigned long long>(res.deframer.pointer_no_start),
                    static_cast<unsigned long long>(res.deframer.orphan_prefix),
                    static_cast<unsigned long long>(res.deframer.pending_mismatch),
                    static_cast<unsigned long long>(res.deframer.alp_invalid),
                    static_cast<unsigned long long>(res.deframer.counter_jumps),
                    static_cast<unsigned long long>(res.deframer.bbp_invalid),
                    static_cast<unsigned long long>(res.deframer.flushed));
    }
    return 0;
}
