#include "atsc3/sls/route_receiver.hpp"

#include "atsc3/io/inet.hpp"

namespace atsc3::sls {

std::string SessionAddress::toString() const {
    std::string s = io::ipv4ToString(dst_ip) + ":" + std::to_string(dst_port);
    if (src_ip) s = io::ipv4ToString(src_ip) + "->" + s;
    return s;
}

RouteReceiver::RouteReceiver(ip::Demux& demux, SessionAddress sls, RouteReceiverCallbacks cb, RouteReceiverConfig cfg)
    : demux_(demux), sls_addr_(sls), cb_(std::move(cb)), cfg_(cfg) {
    Sess& s = ensureSession(sls_addr_);
    s.is_sls = true;
    route::ChannelConfig c;
    c.tsi = 0;
    s.session->setChannel(c);
    s.tsis.insert(0);
}

RouteReceiver::~RouteReceiver() {
    for (auto& [k, s] : sessions_) demux_.unsubscribe(s->sub);
}

std::unique_ptr<RouteReceiver> RouteReceiver::fromSlt(ip::Demux& demux, const lls::SltService& svc, RouteReceiverCallbacks cb,
                                                      RouteReceiverConfig cfg) {
    if (!svc.bss || svc.bss->sls_protocol != 1 || svc.bss->sls_destination_ip == 0 || svc.bss->sls_destination_port == 0) return nullptr;
    SessionAddress a;
    a.dst_ip = svc.bss->sls_destination_ip;
    a.dst_port = svc.bss->sls_destination_port;
    a.src_ip = svc.bss->sls_source_ip;
    return std::make_unique<RouteReceiver>(demux, a, std::move(cb), cfg);
}

void RouteReceiver::setOpenMediaSessions(bool open) {
    if (open == cfg_.open_media_sessions) return;
    cfg_.open_media_sessions = open;
    if (open) {
        if (sls_ && sls_->stsid) applyStsid(*sls_->stsid);
        return;
    }
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Sess& s = *it->second;
        if (!s.is_sls) {
            demux_.unsubscribe(s.sub);
            it = sessions_.erase(it);
            continue;
        }
        for (auto t = s.tsis.begin(); t != s.tsis.end();) {
            if (*t != 0) {
                s.session->removeChannel(*t);
                t = s.tsis.erase(t);
            } else {
                ++t;
            }
        }
        ++it;
    }
}

std::vector<SessionAddress> RouteReceiver::sessions() const {
    std::vector<SessionAddress> v;
    for (const auto& [k, s] : sessions_) v.push_back(s->addr);
    return v;
}

const route::Session* RouteReceiver::session(const SessionAddress& a) const {
    const auto it = sessions_.find(key(a));
    return it == sessions_.end() ? nullptr : it->second->session.get();
}

RouteReceiver::Sess& RouteReceiver::ensureSession(const SessionAddress& a) {
    auto it = sessions_.find(key(a));
    if (it != sessions_.end()) return *it->second;
    auto s = std::make_unique<Sess>();
    s->addr = a;
    Sess* raw = s.get();
    route::SessionCallbacks scb;
    scb.onObject = [this, raw](std::shared_ptr<const route::DeliveredObject> obj) {
        if (raw->is_sls && obj->tsi == 0) {
            onSlsObject(*obj);
            return;
        }
        ++stats_.objects;
        if (cb_.onObject) cb_.onObject(std::move(obj), raw->addr);
    };
    scb.onEfdt = [this, raw](std::uint32_t tsi, const route::Efdt& e) {
        if (cb_.onEfdt) cb_.onEfdt(raw->addr, tsi, e);
    };
    scb.onObjectError = [this, raw](const route::ObjectError& e) {
        ++stats_.object_errors;
        if (cb_.onObjectError) cb_.onObjectError(e, raw->addr);
    };
    route::SessionConfig sc = cfg_.session;
    sc.deliver_undescribed = cfg_.deliver_undescribed;
    s->session = std::make_unique<route::Session>(std::move(scb), sc);
    ip::FlowKey fk;
    fk.dst_ip = a.dst_ip;
    fk.dst_port = a.dst_port;
    fk.src_ip = a.src_ip;
    s->sub = demux_.subscribe(fk, [raw](const ip::UdpDatagram& d) { raw->session->onDatagram(d); });
    ++stats_.sessions_opened;
    it = sessions_.emplace(key(a), std::move(s)).first;
    return *it->second;
}

void RouteReceiver::onSlsObject(const route::DeliveredObject& obj) {
    ++stats_.sls_objects;
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : obj.data) h = (h ^ b) * 1099511628211ull;
    if (h == sls_hash_ && sls_) return;  // carousel repeat
    SlsBundle bundle;
    std::string err;
    if (!parseSlsBundle(ByteView(obj.data.data(), obj.data.size()), obj.toi, bundle, &err)) {
        ++stats_.sls_errors;
        if (cb_.onError) cb_.onError("SLS object TOI " + std::to_string(obj.toi) + " on " + sls_addr_.toString() + ": " + err);
        return;
    }
    sls_hash_ = h;
    ++stats_.sls_updates;
    sls_ = std::move(bundle);
    if (sls_->stsid && cfg_.open_media_sessions) applyStsid(*sls_->stsid);
    if (cb_.onSls) cb_.onSls(*sls_);
}

void RouteReceiver::applyStsid(const Stsid& st) {
    ++stats_.stsid_applied;
    std::map<std::uint64_t, std::set<std::uint32_t>> wanted;
    for (const Rs& rs : st.rs) {
        SessionAddress a;
        a.dst_ip = rs.dst_ip.value_or(sls_addr_.dst_ip);
        a.dst_port = rs.dst_port.value_or(sls_addr_.dst_port);
        a.src_ip = rs.src_ip.value_or(sls_addr_.src_ip);
        Sess& s = ensureSession(a);
        for (const Ls& ls : rs.ls) {
            route::ChannelConfig c = channelConfig(ls);
            if (c.tsi == 0 && s.is_sls) c.real_time = false;  // never override the SLS channel's role
            s.session->setChannel(c);
            s.tsis.insert(c.tsi);
            wanted[key(a)].insert(c.tsi);
        }
    }
    // Drop channels and sessions no longer described (the SLS session itself stays).
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Sess& s = *it->second;
        const auto w = wanted.find(it->first);
        if (w == wanted.end() && !s.is_sls) {
            demux_.unsubscribe(s.sub);
            it = sessions_.erase(it);
            continue;
        }
        for (auto t = s.tsis.begin(); t != s.tsis.end();) {
            const bool keep = (*t == 0 && s.is_sls) || (w != wanted.end() && w->second.count(*t));
            if (!keep) {
                s.session->removeChannel(*t);
                t = s.tsis.erase(t);
            } else {
                ++t;
            }
        }
        ++it;
    }
}

}  // namespace atsc3::sls
