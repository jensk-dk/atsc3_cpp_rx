#include "atsc3/lls/processor.hpp"

#include "atsc3/io/inet.hpp"
#include "atsc3/util/gzip.hpp"
#include "xml/xml.hpp"

namespace atsc3::lls {

const char* tableName(std::uint8_t id) noexcept {
    switch (id) {
        case kSlt: return "SLT";
        case kRrt: return "RRT";
        case kSystemTime: return "SystemTime";
        case kAeat: return "AEAT";
        case kOmn: return "OnscreenMessageNotification";
        case kCdt: return "CDT";
        case kDrct: return "DRCT";
        case kSignedMultiTable: return "SignedMultiTable";
        case kUserDefined: return "UserDefined";
        default: return "reserved";
    }
}

namespace {
std::uint64_t fnv1a(ByteView d) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::uint8_t b : d) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}
bool isXmlTable(std::uint8_t id) { return id != kSignedMultiTable; }
}  // namespace

Processor::Processor(Callbacks cb) : cb_(std::move(cb)) {}

void Processor::reset() {
    registry_.clear();
    hashes_.clear();
    slts_.clear();
    stts_.clear();
    announced_groups_ = 0;
}

std::vector<std::uint8_t> Processor::groups() const {
    std::vector<std::uint8_t> g;
    for (const auto& [k, r] : registry_) {
        const std::uint8_t group = static_cast<std::uint8_t>(k >> 8);
        if (g.empty() || g.back() != group) g.push_back(group);
    }
    return g;
}

std::optional<Slt> Processor::slt(std::uint8_t group) const {
    const auto it = slts_.find(group);
    if (it == slts_.end()) return std::nullopt;
    return it->second;
}

std::optional<SystemTime> Processor::systemTime(std::uint8_t group) const {
    const auto it = stts_.find(group);
    if (it == stts_.end()) return std::nullopt;
    return it->second;
}

const TableRecord* Processor::table(std::uint8_t group, std::uint8_t table_id) const {
    const auto it = registry_.find(static_cast<std::uint16_t>((group << 8) | table_id));
    return it == registry_.end() ? nullptr : &it->second;
}

std::vector<const TableRecord*> Processor::tables() const {
    std::vector<const TableRecord*> v;
    for (const auto& [k, r] : registry_) v.push_back(&r);
    return v;
}

bool Processor::onDatagram(const ip::UdpDatagram& d) {
    if (d.dst_ip != kLlsAddress || d.dst_port != kLlsPort) return false;
    ++stats_.datagrams;
    onLlsTable(d.payload, d.src_ip, d.plp, d.time.t_ns);
    return true;
}

void Processor::onLlsTable(ByteView t, std::uint32_t source_ip, std::uint8_t plp, std::int64_t t_ns) {
    if (t.size() < 4) {
        ++stats_.bad_header;
        return;
    }
    TableHeader h;
    h.table_id = t[0];
    h.group_id = t[1];
    h.group_count_minus1 = t[2];
    h.version = t[3];
    announced_groups_ = static_cast<std::uint8_t>(h.group_count_minus1 + 1);
    const ByteView payload = t.subview(4);
    if (h.table_id == kSignedMultiTable) {
        ++stats_.signed_multi_tables;
        ++stats_.tables;
        // LLS_payload_count(8) { id(8) version(8) length(16) payload } signature_length(16) signature
        if (payload.empty()) {
            ++stats_.bad_header;
            return;
        }
        SignedMultiTable smt;
        const std::size_t count = payload[0];
        std::size_t o = 1;
        std::vector<std::pair<TableHeader, ByteView>> inner;
        for (std::size_t i = 0; i < count; ++i) {
            if (o + 4 > payload.size()) {
                ++stats_.bad_header;
                return;
            }
            SignedMultiTableEntry e;
            e.table_id = payload[o];
            e.version = payload[o + 1];
            e.length = io::be16(payload.data() + o + 2);
            o += 4;
            if (o + e.length > payload.size()) {
                ++stats_.bad_header;
                return;
            }
            TableHeader ih = h;
            ih.table_id = e.table_id;
            ih.version = e.version;
            inner.emplace_back(ih, payload.subview(o, e.length));
            smt.entries.push_back(e);
            o += e.length;
        }
        if (o + 2 <= payload.size()) {
            smt.signature_length = io::be16(payload.data() + o);
            o += 2;
            const ByteView sig = payload.subview(o, smt.signature_length);
            smt.signature.assign(sig.begin(), sig.end());
        }
        TableInfo info;
        info.header = h;
        info.source_ip = source_ip;
        info.plp = plp;
        info.t_ns = t_ns;
        if (cb_.onSignedMultiTable) cb_.onSignedMultiTable(info, smt);
        for (const auto& [ih, body] : inner) handleTable(ih, body, source_ip, plp, t_ns, true, h.group_id);
        return;
    }
    handleTable(h, payload, source_ip, plp, t_ns, false, 0);
}

void Processor::handleTable(const TableHeader& h, ByteView payload, std::uint32_t source_ip, std::uint8_t plp,
                            std::int64_t t_ns, bool in_smt, std::uint8_t smt_group) {
    ++stats_.tables;
    const std::uint16_t key = static_cast<std::uint16_t>((h.group_id << 8) | h.table_id);
    const std::uint64_t hash = fnv1a(payload);
    const auto rit = registry_.find(key);
    if (rit != registry_.end() && rit->second.info.header.version == h.version) {
        const auto hit = hashes_.find(key);
        if (hit != hashes_.end() && hit->second == hash) {
            ++stats_.duplicates;
            return;
        }
    }
    TableRecord rec;
    rec.info.header = h;
    rec.info.source_ip = source_ip;
    rec.info.plp = plp;
    rec.info.t_ns = t_ns;
    rec.info.in_signed_multi_table = in_smt;
    rec.info.multi_table_group = smt_group;
    bool decoded = false;
    if (isXmlTable(h.table_id)) {
        if (util::isGzip(payload) || (!payload.empty() && payload[0] != '<' && payload[0] != 0xEF)) {
            if (auto inflated = util::gunzip(payload)) {
                rec.payload = std::move(*inflated);
                decoded = true;
            } else {
                ++stats_.gunzip_failed;
                if (cb_.onError) cb_.onError(std::string("LLS ") + tableName(h.table_id) + ": gunzip failed");
                rec.payload.assign(payload.begin(), payload.end());
            }
        } else {
            rec.payload.assign(payload.begin(), payload.end());
            decoded = true;
        }
        if (decoded) {
            rec.info.raw_xml.assign(rec.payload.begin(), rec.payload.end());
            pugi::xml_document doc;
            if (xml::load(doc, ByteView(rec.payload.data(), rec.payload.size()))) {
                rec.info.root_namespace = xml::namespaceOf(doc.document_element());
            }
        }
    } else {
        rec.payload.assign(payload.begin(), payload.end());
    }
    ++stats_.new_tables;
    hashes_[key] = hash;
    TableRecord& stored = registry_[key] = std::move(rec);
    dispatch(stored.info, ByteView(stored.payload.data(), stored.payload.size()));
}

void Processor::dispatch(const TableInfo& info, ByteView payload) {
    std::string err;
    if (cb_.onTable) cb_.onTable(info, payload);
    if (info.raw_xml.empty()) return;  // undecodable or binary: raw delivery only
    switch (info.header.table_id) {
        case kSlt: {
            Slt slt;
            if (parseSlt(info.raw_xml, slt, &err)) {
                slts_[info.header.group_id] = slt;
                if (cb_.onSlt) cb_.onSlt(info, slt);
            } else {
                ++stats_.parse_failed;
                if (cb_.onError) cb_.onError("SLT parse failed: " + err);
            }
            break;
        }
        case kSystemTime: {
            SystemTime st;
            if (parseSystemTime(info.raw_xml, st, &err)) {
                stts_[info.header.group_id] = st;
                if (cb_.onSystemTime) cb_.onSystemTime(info, st);
            } else {
                ++stats_.parse_failed;
                if (cb_.onError) cb_.onError("SystemTime parse failed: " + err);
            }
            break;
        }
        case kAeat: {
            Aeat a;
            if (parseAeat(info.raw_xml, a, &err)) {
                if (cb_.onAeat) cb_.onAeat(info, a);
            } else {
                ++stats_.parse_failed;
                if (cb_.onError) cb_.onError("AEAT parse failed: " + err);
            }
            break;
        }
        case kRrt: {
            Rrt r;
            if (parseRrt(info.raw_xml, r, &err)) {
                if (cb_.onRrt) cb_.onRrt(info, r);
            } else {
                ++stats_.parse_failed;
                if (cb_.onError) cb_.onError("RRT parse failed: " + err);
            }
            break;
        }
        case kOmn: {
            Omn o;
            if (parseOmn(info.raw_xml, o, &err)) {
                if (cb_.onOmn) cb_.onOmn(info, o);
            } else {
                ++stats_.parse_failed;
                if (cb_.onError) cb_.onError("OnscreenMessageNotification parse failed: " + err);
            }
            break;
        }
        default:
            break;  // CDT, DRCT, UserDefined, reserved: raw only
    }
}

}  // namespace atsc3::lls
