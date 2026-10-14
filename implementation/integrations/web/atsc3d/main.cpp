// atsc3d: runs the stack on a capture (pcap virtual tuner) and serves the REST API and the Vue frontend
// (spec 5.17.1). RF/UDP sources follow with their adapters.
//
//   atsc3d [--pcap FILE] [--captures DIR]... [--www DIR] [--web ADDR:PORT] [--token T] [--allow-remote]
//          [--nonrealtime] [--speed X] [--loop] [--no-web] [--duration SEC]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "atsc3/service/engine.hpp"
#include "atsc3/tuners/pcap/virtual_tuner.hpp"
#include "atsc3/stack.hpp"
#ifdef ATSC3_HAVE_WEB
#include "atsc3/web/server.hpp"
#endif

#ifndef ATSC3D_DEFAULT_CAPTURES
#define ATSC3D_DEFAULT_CAPTURES ""
#endif
#ifndef ATSC3D_DEFAULT_WWW
#define ATSC3D_DEFAULT_WWW ""
#endif

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

int usage() {
    std::fprintf(stderr,
                 "usage: atsc3d [--pcap FILE] [--captures DIR]... [--www DIR] [--web ADDR:PORT] [--token T] [--allow-remote]\n"
                 "              [--nonrealtime] [--speed X] [--loop] [--duration SEC] [--no-web]\n"
                 "defaults: --web 127.0.0.1:8080, --captures %s, --www %s\n",
                 ATSC3D_DEFAULT_CAPTURES, ATSC3D_DEFAULT_WWW);
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    std::string pcap, bind = "127.0.0.1";
    int port = 8080;
    std::vector<std::string> captures;
    std::string www = ATSC3D_DEFAULT_WWW, token;
    bool allow_remote = false, nonrealtime = false, loop = false, no_web = false;
    double speed = 1.0, duration_s = 0.0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (std::strcmp(a, "--version") == 0 || std::strcmp(a, "-V") == 0) {
            std::printf("atsc3d %s\n", atsc3::versionString().data());
            return 0;
        } else if (std::strcmp(a, "--pcap") == 0) {
            const char* v = next();
            if (!v) return usage();
            pcap = v;
        } else if (std::strcmp(a, "--captures") == 0) {
            const char* v = next();
            if (!v) return usage();
            captures.emplace_back(v);
        } else if (std::strcmp(a, "--www") == 0) {
            const char* v = next();
            if (!v) return usage();
            www = v;
        } else if (std::strcmp(a, "--web") == 0) {
            const char* v = next();
            if (!v) return usage();
            const std::string s(v);
            const auto colon = s.rfind(':');
            if (colon == std::string::npos) port = std::atoi(s.c_str());
            else {
                if (colon > 0) bind = s.substr(0, colon);
                port = std::atoi(s.c_str() + colon + 1);
            }
        } else if (std::strcmp(a, "--token") == 0) {
            const char* v = next();
            if (!v) return usage();
            token = v;
        } else if (std::strcmp(a, "--speed") == 0) {
            const char* v = next();
            if (!v) return usage();
            speed = std::atof(v);
        } else if (std::strcmp(a, "--duration") == 0) {
            const char* v = next();
            if (!v) return usage();
            duration_s = std::atof(v);
        } else if (std::strcmp(a, "--allow-remote") == 0) allow_remote = true;
        else if (std::strcmp(a, "--nonrealtime") == 0) nonrealtime = true;
        else if (std::strcmp(a, "--loop") == 0) loop = true;
        else if (std::strcmp(a, "--no-web") == 0) no_web = true;
        else return usage();
    }
    if (captures.empty() && std::strlen(ATSC3D_DEFAULT_CAPTURES) > 0) captures.emplace_back(ATSC3D_DEFAULT_CAPTURES);
    if (!pcap.empty()) {
        std::error_code ec;
        const auto dir = std::filesystem::absolute(std::filesystem::path(pcap), ec).parent_path();
        bool covered = false;
        for (const std::string& d : captures) {
            const auto base = std::filesystem::weakly_canonical(d, ec).string();
            const auto cand = std::filesystem::weakly_canonical(dir, ec).string();
            if (cand.compare(0, base.size(), base) == 0) covered = true;
        }
        if (!covered) captures.push_back(dir.string());
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    atsc3::service::Engine engine;
    if (!pcap.empty()) {
        atsc3::pcap::VirtualTunerConfig cfg;
        cfg.path = pcap;
        cfg.pacing = nonrealtime ? atsc3::pcap::Pacing::NonRealTime : atsc3::pcap::Pacing::RealTime;
        cfg.speed = speed;
        cfg.loop = loop;
        std::string err;
        if (!engine.open(std::make_shared<atsc3::pcap::VirtualTuner>(cfg), atsc3::service::OpenOptions{}, &err)) {
            std::fprintf(stderr, "atsc3d: cannot open %s: %s\n", pcap.c_str(), err.c_str());
            return 2;
        }
        std::printf("atsc3d: opened %s (%s)\n", pcap.c_str(), engine.status().detection.c_str());
    }
#ifdef ATSC3_HAVE_WEB
    std::unique_ptr<atsc3::web::Server> server;
    if (!no_web) {
        atsc3::web::ServerConfig wc;
        wc.bindAddress = bind;
        wc.port = static_cast<std::uint16_t>(port);
        wc.token = token;
        wc.allowRemote = allow_remote;
        wc.captureDirs = captures;
        std::error_code ec;
        if (!www.empty() && std::filesystem::is_regular_file(std::filesystem::path(www) / "index.html", ec)) wc.staticDir = www;
        else if (!www.empty()) std::fprintf(stderr, "atsc3d: no frontend at %s (run npm ci && npm run build in integrations/web/frontend/); serving the API only\n", www.c_str());
        server = std::make_unique<atsc3::web::Server>(engine, wc);
        std::string err;
        if (!server->start(&err)) {
            std::fprintf(stderr, "atsc3d: %s\n", err.c_str());
            return 2;
        }
        std::printf("atsc3d: web UI at http://%s:%u/  (API /api/v1/status)\n", bind.c_str(), static_cast<unsigned>(server->port()));
    }
#else
    (void)port;
    (void)allow_remote;
    if (!no_web) std::fprintf(stderr, "atsc3d: built without the web server (ATSC3_WEB=OFF)\n");
#endif
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(duration_s * 1000.0));
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (duration_s > 0 && std::chrono::steady_clock::now() >= deadline) break;
    }
    engine.close();
    return 0;
}
