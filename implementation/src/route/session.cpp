#include "atsc3/route/session.hpp"

#include <algorithm>

#include "atsc3/route/multipart.hpp"
#include "atsc3/util/gzip.hpp"

namespace atsc3::route {

const char* toString(Completion c) noexcept {
    switch (c) {
        case Completion::TransferLength: return "transfer-length";
        case Completion::FdtLength: return "fdt-length";
        case Completion::CloseObject: return "close-object";
        case Completion::Carousel: return "carousel";
        default: return "timeout";
    }
}

Session::Session(SessionCallbacks cb, SessionConfig cfg) : cb_(std::move(cb)), cfg_(cfg) {}

void Session::reset() {
    objects_.clear();
    for (auto& [tsi, ch] : channels_) ch.inband_efdt.reset();
}

void Session::setChannel(const ChannelConfig& cfg) {
    Channel& ch = channels_[cfg.tsi];
    ch.cfg = cfg;
    ch.configured = true;
}

void Session::removeChannel(std::uint32_t tsi) { channels_.erase(tsi); }

std::optional<ChannelConfig> Session::channel(std::uint32_t tsi) const {
    const auto it = channels_.find(tsi);
    if (it == channels_.end() || !it->second.configured) return std::nullopt;
    return it->second.cfg;
}

const Efdt* Session::efdt(std::uint32_t tsi) const {
    const auto it = channels_.find(tsi);
    if (it == channels_.end()) return nullptr;
    if (it->second.inband_efdt) return &*it->second.inband_efdt;
    if (it->second.cfg.efdt) return &*it->second.cfg.efdt;
    return nullptr;
}

FormatId Session::formatFor(std::uint32_t tsi, std::uint8_t cp) const {
    switch (cp) {
        case 1: case 5: case 6: case 7: case 8: return FormatId::File;
        case 2: case 9: return FormatId::Entity;
        case 3: return FormatId::UnsignedPackage;
        case 4: return FormatId::SignedPackage;
        default: break;
    }
    if (cp >= 128) {
        const auto it = channels_.find(tsi);
        if (it != channels_.end()) {
            for (const PayloadRule& r : it->second.cfg.payloads) {
                if (r.codepoint == cp) return r.format_id;
            }
        }
        return FormatId::File;
    }
    return FormatId::Unknown;
}

bool Session::contiguous(const Partial& p, std::uint64_t upto) const {
    std::uint64_t covered = 0;
    for (const auto& [b, e] : p.ranges) {  // ranges kept sorted and merged
        if (b > covered) return false;
        covered = std::max(covered, e);
        if (covered >= upto) return true;
    }
    return covered >= upto;
}

std::vector<Session::PendingObject> Session::pending() const {
    std::vector<PendingObject> out;
    for (const auto& [k, p] : objects_) {
        PendingObject o;
        o.tsi = static_cast<std::uint32_t>(k >> 32);
        o.toi = static_cast<std::uint32_t>(k & 0xffffffffu);
        for (const auto& [b, e] : p.ranges) o.received += e - b;
        o.expected = p.expected;
        o.first_ns = p.first_ns;
        o.last_ns = p.last_ns;
        out.push_back(o);
    }
    return out;
}

void Session::expire(std::int64_t now) {
    for (auto it = objects_.begin(); it != objects_.end();) {
        const std::uint32_t tsi = static_cast<std::uint32_t>(it->first >> 32);
        const std::uint32_t toi = static_cast<std::uint32_t>(it->first & 0xFFFFFFFF);
        const auto ch = channels_.find(tsi);
        const bool rt = ch != channels_.end() && ch->second.cfg.real_time;
        const std::int64_t timeout = rt ? cfg_.object_timeout_ns : cfg_.nrt_object_timeout_ns;
        if (now - it->second.last_ns > timeout) {
            ++stats_.objects_timeout;
            fail(it->first, it->second, tsi, toi, "timeout");
            it = objects_.erase(it);
        } else {
            ++it;
        }
    }
}

void Session::fail(Key, const Partial& p, std::uint32_t tsi, std::uint32_t toi, const std::string& reason) {
    if (!cb_.onObjectError) return;
    ObjectError e;
    e.tsi = tsi;
    e.toi = toi;
    e.reason = reason;
    e.expected = p.expected;
    for (const auto& [b, en] : p.ranges) e.received += en - b;
    cb_.onObjectError(e);
}

void Session::onDatagram(const ip::UdpDatagram& d) {
    expire(d.time.t_ns);
    LctPacket pkt;
    if (parseLct(d.payload, pkt) != LctStatus::Ok) {
        ++stats_.bad_lct;
        return;
    }
    ++stats_.packets;
    const std::uint32_t tsi = static_cast<std::uint32_t>(pkt.header.tsi);
    const std::uint32_t toi = static_cast<std::uint32_t>(pkt.header.toi);
    if (pkt.dataless) {
        ++stats_.dataless;
        if (pkt.header.close_object) {
            const auto it = objects_.find(key(tsi, toi));
            if (it != objects_.end() && it->second.expected && contiguous(it->second, *it->second.expected)) {
                complete(it->first, it->second, tsi, toi, Completion::CloseObject);
                objects_.erase(it);
            }
        }
        return;
    }
    const auto chit = channels_.find(tsi);
    if (chit == channels_.end() && !cfg_.deliver_undescribed && tsi != 0) return;
    const Key k = key(tsi, toi);
    auto it = objects_.find(k);
    const bool known_length = pkt.header.transfer_length.has_value();
    // Carousel completion: a packet at offset 0 for an object we already hold contiguously (no length known).
    if (it != objects_.end() && pkt.start_offset == 0 && !it->second.expected && it->second.packets > 0 &&
        !it->second.ranges.empty() && contiguous(it->second, it->second.highest) && it->second.highest > 0 &&
        it->second.ranges.front().first == 0 && it->second.data.size() >= pkt.payload.size()) {
        // same first bytes -> repetition of the whole object
        if (std::equal(pkt.payload.begin(), pkt.payload.end(), it->second.data.begin())) {
            complete(k, it->second, tsi, toi, Completion::Carousel);
            objects_.erase(it);
            it = objects_.end();
        }
    }
    if (it == objects_.end()) {
        Partial p;
        p.first_ns = d.time.t_ns;
        p.codepoint = pkt.header.codepoint;
        it = objects_.emplace(k, std::move(p)).first;
    }
    Partial& p = it->second;
    p.last_ns = d.time.t_ns;
    ++p.packets;
    stats_.bytes += pkt.payload.size();
    if (pkt.header.presentation_time_ntp) p.presentation_time_ntp = pkt.header.presentation_time_ntp;
    if (pkt.header.close_object) p.close_seen = true;
    // expected length: in-band extension, else EFDT
    if (known_length) {
        p.expected = *pkt.header.transfer_length;
    } else if (!p.expected) {
        if (const Efdt* e = efdt(tsi)) {
            if (const FdtFile* f = e->find(toi)) {
                if (f->transfer_length) p.expected = *f->transfer_length;
                else if (f->content_length && !f->content_encoding) p.expected = *f->content_length;
            }
        }
    }
    const std::uint64_t begin = pkt.start_offset;
    const std::uint64_t end = begin + pkt.payload.size();
    const std::uint64_t limit = p.expected ? *p.expected : cfg_.max_object_bytes;
    if (end > limit || end > cfg_.max_object_bytes) {
        ++stats_.out_of_bounds;
        return;
    }
    if (p.data.size() < end) p.data.resize(static_cast<std::size_t>(end));
    bool dup = false;
    for (const auto& [b, e] : p.ranges) {
        if (b <= begin && e >= end) {
            dup = true;
            break;
        }
    }
    if (dup) ++stats_.duplicates;
    std::copy(pkt.payload.begin(), pkt.payload.end(), p.data.begin() + static_cast<std::ptrdiff_t>(begin));
    p.ranges.emplace_back(begin, end);
    std::sort(p.ranges.begin(), p.ranges.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& r : p.ranges) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    p.ranges.swap(merged);
    p.highest = std::max(p.highest, end);
    if (cfg_.fragments && cb_.onFragment && chit != channels_.end() && chit->second.cfg.real_time) {
        const std::uint64_t prefix = p.ranges.front().first == 0 ? p.ranges.front().second : 0;
        if (prefix > p.delivered_prefix) {
            cb_.onFragment(tsi, toi, ByteView(p.data.data() + p.delivered_prefix, static_cast<std::size_t>(prefix - p.delivered_prefix)), p.delivered_prefix);
            p.delivered_prefix = prefix;
        }
    }
    if (p.expected && contiguous(p, *p.expected)) {
        complete(k, p, tsi, toi, known_length ? Completion::TransferLength : Completion::FdtLength);
        objects_.erase(it);
    } else if (p.close_seen && !p.expected && contiguous(p, p.highest)) {
        p.expected = p.highest;
        complete(k, p, tsi, toi, Completion::CloseObject);
        objects_.erase(it);
    }
}

void Session::handleInbandEfdt(std::uint32_t tsi, const std::vector<std::uint8_t>& data) {
    Efdt e;
    std::string text(data.begin(), data.end());
    if (!parseEfdt(text, e)) {
        ++stats_.decode_failed;
        return;
    }
    Channel& ch = channels_[tsi];
    if (ch.inband_efdt && ch.inband_efdt->efdt_version && e.efdt_version && *ch.inband_efdt->efdt_version == *e.efdt_version &&
        ch.inband_efdt->raw_xml == e.raw_xml)
        return;
    ch.inband_efdt = std::move(e);
    ++stats_.efdt_updates;
    if (cb_.onEfdt) cb_.onEfdt(tsi, *ch.inband_efdt);
}

void Session::applyMetadata(DeliveredObject& obj, const Channel* ch) {
    if (const Efdt* e = efdt(obj.tsi)) {
        if (const FdtFile* f = e->find(obj.toi)) {
            obj.content_location = f->content_location;
            obj.content_type = f->content_type;
            obj.content_encoding = f->content_encoding;
            obj.content_md5 = f->content_md5;
            obj.content_length = f->content_length;
            obj.app_context_id_list = f->app_context_id_list;
        } else if (auto loc = e->locationFor(obj.toi)) {
            obj.content_location = loc;
            obj.from_template = true;
            obj.content_type = e->default_content_type;
            obj.content_encoding = e->default_content_encoding;
        }
        if (obj.app_context_id_list.empty()) obj.app_context_id_list = e->app_context_id_list;
    }
    if (ch) {
        obj.rep_id = ch->cfg.rep_id;
        obj.media_content_type = ch->cfg.media_content_type;
    }
}

void Session::complete(Key, Partial& p, std::uint32_t tsi, std::uint32_t toi, Completion how) {
    const std::uint64_t len = p.expected.value_or(p.highest);
    p.data.resize(static_cast<std::size_t>(len));
    if (toi == 0) {  // Extended FDT instance for this channel
        handleInbandEfdt(tsi, p.data);
        ++stats_.objects_complete;
        return;
    }
    auto obj = std::make_shared<DeliveredObject>();
    obj->tsi = tsi;
    obj->toi = toi;
    obj->codepoint = p.codepoint;
    obj->format = formatFor(tsi, p.codepoint);
    obj->completion = how;
    obj->transfer_length = len;
    obj->first_ns = p.first_ns;
    obj->last_ns = p.last_ns;
    obj->packets = p.packets;
    obj->close_object_seen = p.close_seen;
    obj->presentation_time_ntp = p.presentation_time_ntp;
    const auto chit = channels_.find(tsi);
    applyMetadata(*obj, chit == channels_.end() ? nullptr : &chit->second);
    obj->data = std::move(p.data);
    if (obj->format == FormatId::Entity) {  // HTTP entity: headers then body
        EntityHeaders hdr;
        const std::size_t n = parseEntityHeaders(ByteView(obj->data.data(), obj->data.size()), hdr);
        if (n > 0) {
            if (const auto v = hdr.contentLocation()) obj->content_location = v;
            if (!hdr.contentType().empty()) obj->content_type = hdr.contentType();
            if (const auto v = hdr.contentEncoding()) obj->content_encoding = v;
            obj->data.erase(obj->data.begin(), obj->data.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    if (obj->content_encoding && *obj->content_encoding == "gzip") {
        if (auto inflated = util::gunzip(ByteView(obj->data.data(), obj->data.size()), cfg_.max_object_bytes)) {
            obj->data = std::move(*inflated);
            obj->content_decoded = true;
        } else {
            ++stats_.decode_failed;
        }
    }
    ++stats_.objects_complete;
    if (cb_.onObject) cb_.onObject(std::move(obj));
}

}  // namespace atsc3::route
