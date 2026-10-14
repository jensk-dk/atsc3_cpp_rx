#pragma once
// Embedded HTTP server (spec 5.17): REST API under /api/v1 over a service::Engine, capture directory
// listing for the virtual tuner, static serving of the built frontend with SPA fallback.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "atsc3/service/engine.hpp"

namespace atsc3::web {

struct ServerConfig {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string token;                       // optional bearer token (checked on /api when set)
    std::string staticDir;                   // frontend/dist; empty = API only
    std::vector<std::string> captureDirs;    // directories listed by GET /api/v1/source
    bool allowRemote = false;
};

struct CaptureEntry {
    std::string path, name, dir;
    std::uint64_t size = 0;
    std::int64_t mtime_unix = 0;
    std::string format;   // from the format detector (cached)
    std::string detection;
    bool sidecar = false;
};

class Server {
public:
    Server(service::Engine& engine, ServerConfig cfg);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds and starts the I/O thread; false with `error` when the port cannot be bound.
    bool start(std::string* error = nullptr);
    void stop();
    bool running() const;
    std::uint16_t port() const noexcept;   // actual port (0 in config picks a free one)

    /// Lists captures (pcap/pcapng) under the configured directories; detection cached per path+mtime.
    std::vector<CaptureEntry> listCaptures();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* name() noexcept;

}  // namespace atsc3::web
