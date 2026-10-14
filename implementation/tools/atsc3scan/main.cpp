// atsc3scan: capture inventory (spec 6.1). Runs the pcap virtual tuner's format detection and then the stack's own
// parsers over the capture: link-layer facts (BBP, ALP, LMT), LLS tables, ROUTE/LCT flows with object completeness,
// MMTP flows with their signaling, and the UDP flow table.
//
//   atsc3scan [options] <capture.pcap>
//     --limit N          scan only the first N pcap records
//     --start SEC        skip the first SEC seconds of capture time
//     --duration SEC     scan only SEC seconds of capture time
//     --slt              print the SLT XML of every LLS SLT
//     --sls              print every contiguous TSI 0 (SLS) object as text
//     --objs             list every ROUTE object with bytes, span, transfer length and completeness
//     --mmt              print the MP tables and mmt_atsc3_messages of every MMTP flow
//     --sidecar auto|off|FILE   DekTec .xa3 sidecar lookup (default auto)
//     --probe N          format detector probe window in records
// Exit codes: 0 ATSC 3.0 content found, 2 usage or open error, 3 no ATSC 3.0 content.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "atsc3/stack.hpp"
#include "atsc3/tools/scan.hpp"

namespace {
int usage() {
    std::fprintf(stderr,
                 "usage: atsc3scan [--limit N] [--start SEC] [--duration SEC] [--slt] [--sls] [--objs] [--mmt]\n"
                 "                 [--sidecar auto|off|FILE] [--probe N] <capture.pcap>\n");
    return 2;
}
std::int64_t secondsToNs(const char* s) { return static_cast<std::int64_t>(std::atof(s) * 1e9); }
}  // namespace

int main(int argc, char** argv) {
    atsc3::tools::ScanOptions opts;
    atsc3::tools::ScanPrint print;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--limit") == 0 && i + 1 < argc) opts.max_records = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(a, "--start") == 0 && i + 1 < argc) opts.start_offset_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--duration") == 0 && i + 1 < argc) opts.duration_ns = secondsToNs(argv[++i]);
        else if (std::strcmp(a, "--sidecar") == 0 && i + 1 < argc) opts.sidecar = argv[++i];
        else if (std::strcmp(a, "--probe") == 0 && i + 1 < argc) opts.probe_packets = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--slt") == 0) print.slt = true;
        else if (std::strcmp(a, "--sls") == 0) print.sls = true;
        else if (std::strcmp(a, "--objs") == 0) print.objs = true;
        else if (std::strcmp(a, "--mmt") == 0) print.mmt = true;
        else if (std::strcmp(a, "--version") == 0 || std::strcmp(a, "-V") == 0) {
            std::printf("atsc3scan %s\n", atsc3::versionString().data());
            return 0;
        } else if (a[0] == '-') return usage();
        else if (opts.input.empty()) opts.input = a;
        else return usage();
    }
    if (opts.input.empty()) return usage();
    opts.keep_sls_objects = print.sls;

    atsc3::tools::ScanReport report;
    std::string err;
    const int rc = atsc3::tools::scanCapture(opts, report, &err);
    std::printf("# %s\n", opts.input.c_str());
    if (rc != 0) {
        if (!report.detection.empty()) std::printf("%s\n", report.detection.c_str());
        std::fprintf(stderr, "atsc3scan: %s\n", err.c_str());
        return rc;
    }
    const std::string text = report.text(print);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
