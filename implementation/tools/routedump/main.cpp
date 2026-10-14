// routedump: extract every ROUTE service of a capture into a directory tree with the SLS fragments,
// the recovered objects and a static MPD that plays the captured segments offline (e.g. VLC).
//
//   routedump [options] <in.pcap> <out-dir>
//     --service <id>     only this service id (repeatable)
//     --start <sec>      skip the first <sec> seconds of capture time
//     --duration <sec>   process only <sec> seconds of capture time
//     --no-nrt           do not write non-media objects / packages
//     --no-unpack        keep packages as single files
//     --no-sls           do not write the sls/ directories
//     --sidecar <mode>   auto|off|<file> (DekTec .xa3 for raw-IP captures)
//     -q                 quiet
//
// Output: <out>/bsid-<bsid>/<serviceId>-<name>/{manifest.mpd, <segments...>, sls/, nrt/}, <out>/summary.txt
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "atsc3/stack.hpp"
#include "atsc3/tools/routedump.hpp"

namespace {
int usage() {
    std::fprintf(stderr,
                 "usage: routedump [--service ID]... [--start SEC] [--duration SEC] [--no-nrt] [--no-unpack] [--no-sls]\n"
                 "                 [--sidecar auto|off|FILE] [-q] <in.pcap> <out-dir>\n");
    return 2;
}
std::int64_t secondsToNs(const char* s) { return static_cast<std::int64_t>(std::atof(s) * 1e9); }
}  // namespace

int main(int argc, char** argv) {
    atsc3::tools::RouteDumpOptions opts;
    bool quiet = false;
    std::string positional[2];
    int npos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--service") == 0 && i + 1 < argc) opts.services.insert(static_cast<std::uint16_t>(std::atoi(argv[++i])));
        else if (std::strcmp(a, "--start") == 0 && i + 1 < argc) opts.start_offset_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--duration") == 0 && i + 1 < argc) opts.duration_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--sidecar") == 0 && i + 1 < argc) opts.sidecar = argv[++i];
        else if (std::strcmp(a, "--no-nrt") == 0) opts.write_nrt = false;
        else if (std::strcmp(a, "--no-unpack") == 0) opts.unpack_packages = false;
        else if (std::strcmp(a, "--no-sls") == 0) opts.write_sls = false;
        else if (std::strcmp(a, "-q") == 0) quiet = true;
        else if (std::strcmp(a, "-v") == 0) opts.verbose = true;
        else if (std::strcmp(a, "--version") == 0) {
            std::printf("routedump %s\n", atsc3::versionString().data());
            return 0;
        } else if (a[0] == '-') return usage();
        else if (npos < 2) positional[npos++] = a;
        else return usage();
    }
    if (npos != 2) return usage();
    opts.input = positional[0];
    opts.out_dir = positional[1];

    atsc3::tools::RouteDumpResult res;
    std::string err;
    const int rc = atsc3::tools::routeDump(opts, res, &err);
    if (!quiet) std::fputs(res.summary().c_str(), stdout);
    if (rc != 0) {
        std::fprintf(stderr, "routedump: %s\n", err.c_str());
        return rc;
    }
    return 0;
}
