#pragma once
// Per-service ROUTE receiver (spec 5.9.1, 5.9.3): opens the SLS session from the SLT entry, parses
// SLS bundles, and opens/updates the media and NRT sessions/channels described by the S-TSID.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atsc3/ip/demux.hpp"
#include "atsc3/lls/tables.hpp"
#include "atsc3/route/session.hpp"
#include "atsc3/sls/bundle.hpp"

namespace atsc3::sls {

struct SessionAddress {
    std::uint32_t dst_ip = 0;    // host order
    std::uint16_t dst_port = 0;
    std::uint32_t src_ip = 0;    // 0 = any
    std::string toString() const;
};

struct RouteReceiverCallbacks {
    /// A new or changed SLS bundle (duplicates of the previous bundle are suppressed).
    std::function<void(const SlsBundle&)> onSls;
    /// Delivery objects other than the SLS bundle (media segments, NRT files, packages).
    std::function<void(std::shared_ptr<const route::DeliveredObject>, const SessionAddress&)> onObject;
    std::function<void(const route::ObjectError&, const SessionAddress&)> onObjectError;
    std::function<void(const SessionAddress&, std::uint32_t tsi, const route::Efdt&)> onEfdt;
    std::function<void(const std::string&)> onError;
};

struct RouteReceiverConfig {
    route::SessionConfig session;
    bool open_media_sessions = true;   // false = SLS only
    bool deliver_undescribed = false;  // objects on TSIs the S-TSID does not describe (default: drop)
};

struct RouteReceiverStats {
    std::uint64_t sls_objects = 0, sls_updates = 0, sls_errors = 0, stsid_applied = 0, sessions_opened = 0;
    std::uint64_t objects = 0, object_errors = 0;
};

class RouteReceiver {
public:
    RouteReceiver(ip::Demux& demux, SessionAddress sls, RouteReceiverCallbacks cb, RouteReceiverConfig cfg = {});
    ~RouteReceiver();
    RouteReceiver(const RouteReceiver&) = delete;
    RouteReceiver& operator=(const RouteReceiver&) = delete;

    /// nullptr when the service is not ROUTE-signaled over broadcast.
    static std::unique_ptr<RouteReceiver> fromSlt(ip::Demux& demux, const lls::SltService& svc, RouteReceiverCallbacks cb,
                                                  RouteReceiverConfig cfg = {});

    /// Opens (true) or closes (false) the media/NRT sessions described by the current S-TSID.
    void setOpenMediaSessions(bool open);
    bool openMediaSessions() const noexcept { return cfg_.open_media_sessions; }
    const SessionAddress& slsAddress() const noexcept { return sls_addr_; }
    const SlsBundle* sls() const noexcept { return sls_ ? &*sls_ : nullptr; }
    std::vector<SessionAddress> sessions() const;
    const route::Session* session(const SessionAddress& a) const;
    const RouteReceiverStats& stats() const noexcept { return stats_; }

private:
    struct Sess {
        SessionAddress addr;
        ip::SubscriptionId sub = 0;
        std::unique_ptr<route::Session> session;
        std::set<std::uint32_t> tsis;
        bool is_sls = false;
    };
    static std::uint64_t key(const SessionAddress& a) { return (static_cast<std::uint64_t>(a.dst_ip) << 16) | a.dst_port; }
    Sess& ensureSession(const SessionAddress& a);
    void onSlsObject(const route::DeliveredObject& obj);
    void applyStsid(const Stsid& st);

    ip::Demux& demux_;
    SessionAddress sls_addr_;
    RouteReceiverCallbacks cb_;
    RouteReceiverConfig cfg_;
    RouteReceiverStats stats_;
    std::map<std::uint64_t, std::unique_ptr<Sess>> sessions_;
    std::optional<SlsBundle> sls_;
    std::uint64_t sls_hash_ = 0;
};

}  // namespace atsc3::sls
