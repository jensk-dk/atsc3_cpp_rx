#include "atsc3/tools/scan.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>

#include "atsc3/io/inet.hpp"
#include "atsc3/mmtp/packet.hpp"
#include "atsc3/mmtp/session.hpp"
#include "atsc3/route/lct.hpp"

namespace atsc3::tools {

namespace {

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}

/// Printable preview of table/object bytes: ASCII kept, everything else escaped like a Python bytes literal.
std::string preview(const std::uint8_t* p, std::size_t n, std::size_t limit) {
    std::string s;
    for (std::size_t i = 0; i < n && i < limit; ++i) {
        const std::uint8_t c = p[i];
        if (c == '\n') s += "\\n";
        else if (c == '\r') s += "\\r";
        else if (c == '\t') s += "\\t";
        else if (c == '\\') s += "\\\\";
        else if (c >= 0x20 && c < 0x7F) s.push_back(static_cast<char>(c));
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            s += buf;
        }
    }
    return s;
}

std::string flowName(std::uint32_t dst, std::uint16_t port) { return io::ipv4ToString(dst) + ":" + std::to_string(port); }

const char* layoutName(alp::Layout l) {
    switch (l) {
        case alp::Layout::Single: return "single";
        case alp::Layout::Segment: return "segment";
        case alp::Layout::Concatenation: return "concatenation";
        case alp::Layout::MpegTs: return "ts";
    }
    return "?";
}

/// Byte ranges of one object as the LCT packets delivered them.
struct ObjectState {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;  // merged, sorted, [begin, end)
    std::uint64_t extent = 0;
    std::optional<std::uint64_t> transfer_length;
    std::string first_hex;
    std::vector<std::uint8_t> data;   // TSI 0 only
    bool keep_data = false;

    void add(std::uint64_t offset, ByteView bytes) {
        const std::uint64_t end = offset + bytes.size();
        extent = std::max(extent, end);
        if (offset == 0 && first_hex.empty() && !bytes.empty()) first_hex = hex(bytes.data(), std::min<std::size_t>(bytes.size(), 12));
        if (keep_data && !bytes.empty()) {
            if (data.size() < end) data.resize(static_cast<std::size_t>(end));
            std::copy(bytes.begin(), bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        if (bytes.empty()) return;
        // insert and merge
        auto it = std::lower_bound(ranges.begin(), ranges.end(), std::make_pair(offset, end));
        it = ranges.insert(it, {offset, end});
        if (it != ranges.begin() && std::prev(it)->second >= it->first) {
            auto prev = std::prev(it);
            prev->second = std::max(prev->second, it->second);
            it = ranges.erase(it);
            it = prev;
        }
        while (std::next(it) != ranges.end() && std::next(it)->first <= it->second) {
            it->second = std::max(it->second, std::next(it)->second);
            ranges.erase(std::next(it));
        }
    }
    std::uint64_t received() const {
        std::uint64_t n = 0;
        for (const auto& r : ranges) n += r.second - r.first;
        return n;
    }
    bool contiguous() const {
        if (ranges.empty()) return extent == 0;
        return ranges.size() == 1 && ranges.front().first == 0 && ranges.front().second == extent;
    }
};

struct MmtpFlowState {
    MmtpFlowReport report;
    std::unique_ptr<mmtp::Session> session;
    std::set<std::string> atsc3_seen;   // content_type + uri
};

class Scanner {
public:
    Scanner(const ScanOptions& o, ScanReport& r)
        : opts_(o), res_(r),
          decap_([this] {
              alp::Callbacks cb;
              cb.onIpv4 = [this](std::uint8_t plp, ByteView ip, std::optional<std::uint8_t> sid) { demux_.onIpv4(plp, ip, time_, sid); };
              cb.onSignaling = [this](std::uint8_t plp, const alp::SignalingInfo& info, ByteView table) {
                  ++res_.alp.signaling_types[info.type];
                  link_.onSignaling(plp, info, table);
              };
              return cb;
          }()),
          deframer_([this](std::uint8_t plp, ByteView alp) { onAlpPacket(plp, alp); }),
          link_([this] {
              link::LinkSignaling::Callbacks cb;
              cb.onLmt = [this](std::uint8_t plp, const link::Lmt& lmt) {
                  for (const LmtReport& l : res_.lmts)
                      if (l.carrier_plp == plp && l.lmt.version == lmt.version) return;
                  if (res_.lmts.size() < opts_.max_lmts) res_.lmts.push_back({plp, lmt});
              };
              return cb;
          }()),
          lls_([this] {
              lls::Callbacks cb;
              cb.onTable = [this](const lls::TableInfo& info, ByteView payload) {
                  LlsTableReport t;
                  t.group = info.header.group_id;
                  t.table_id = info.header.table_id;
                  t.version = info.header.version;
                  t.source_ip = info.source_ip;
                  t.in_signed_multi_table = info.in_signed_multi_table;
                  t.length = payload.size();
                  t.preview = preview(payload.data(), payload.size(), 160);
                  t.text = info.raw_xml.empty() ? std::string(reinterpret_cast<const char*>(payload.data()), payload.size()) : info.raw_xml;
                  res_.lls_tables.push_back(std::move(t));
              };
              cb.onSignedMultiTable = [this](const lls::TableInfo& info, const lls::SignedMultiTable& smt) {
                  SignedMultiTableReport s;
                  s.group = info.header.group_id;
                  s.version = info.header.version;
                  s.source_ip = info.source_ip;
                  s.signature_length = smt.signature_length;
                  s.entries = smt.entries;
                  res_.signed_multi_tables.push_back(std::move(s));
              };
              return cb;
          }()) {
        demux_.tapAll([this](const ip::UdpDatagram& d) { onUdp(d); });
    }

    // ---- tuner units ---------------------------------------------------------------------------------------------
    void onBbp(const io::BasebandPacket& p) {
        note(p.time);
        ++res_.bbp.per_plp[p.plp_id];
        ++res_.bbp.lengths[p.bytes.size()];
        if (p.error) ++res_.bbp.errors;
        phy::BbpHeader h;
        if (phy::parseBbpHeader(p.bytes, h) == phy::BbpStatus::Ok) {
            ++res_.bbp.ofi[h.ofi];
            if (h.ofi != 0) ++res_.bbp.extensions[{h.ofi, h.ext_type}];
            if (h.pointer == phy::kPointerNoAlpStart) ++res_.bbp.pointer_no_start;
            else if (h.pointer == 0) ++res_.bbp.pointer_zero;
            else ++res_.bbp.pointer_other;
        } else {
            ++res_.bbp.invalid;
        }
        deframer_.feed(p.plp_id, p.bytes, p.error);
    }
    void onAlp(const io::AlpPacket& p) {
        note(p.time);
        onAlpPacket(p.plp_id, p.bytes);
    }
    void onIp(const io::IpDatagram& p) {
        note(p.time);
        demux_.onIpv4(p.plp_id, p.bytes, p.time);
    }
    void onEvent(const service::TunerEvent& e) {
        if (e.type == service::TunerEventType::FormatDetected) res_.detection = e.message;
    }

    void finish() {
        res_.bbp.deframer = deframer_.stats();
        res_.alp.decap = decap_.stats();
        res_.link = link_.stats();
        res_.demux = demux_.stats();
        res_.lls = lls_.stats();
        for (auto& [k, f] : udp_flows_) res_.udp_flows.push_back(f);
        std::sort(res_.udp_flows.begin(), res_.udp_flows.end(), [](const UdpFlowCount& a, const UdpFlowCount& b) {
            return a.packets != b.packets ? a.packets > b.packets : std::tie(a.dst, a.port, a.src) < std::tie(b.dst, b.port, b.src);
        });
        for (const auto& key : lct_order_) {
            LctFlowReport r = lct_flows_[key];
            r.objects = lct_objects_[key].size();
            res_.lct_flows.push_back(std::move(r));
        }
        for (auto& [k, st] : objects_) {
            ObjectReport o;
            o.dst = static_cast<std::uint32_t>(std::get<0>(k));
            o.port = std::get<1>(k);
            o.tsi = std::get<2>(k);
            o.toi = std::get<3>(k);
            o.received = st.received();
            o.extent = st.extent;
            o.transfer_length = st.transfer_length;
            o.contiguous = st.contiguous();
            o.complete = o.contiguous && (!o.transfer_length || *o.transfer_length == o.extent);
            o.first_hex = st.first_hex;
            o.data = std::move(st.data);
            if (o.complete) ++res_.objects_complete;
            else ++res_.objects_incomplete;
            res_.objects.push_back(std::move(o));
        }
        for (const auto& key : mmtp_order_) {
            MmtpFlowState& st = mmtp_flows_[key];
            for (const mmtp::PacketIdStats& p : st.session->packetIds()) {
                st.report.length_mismatch += p.length_mismatch;
                st.report.sequence_gaps += p.sequence_gaps;
            }
            res_.mmtp_flows.push_back(st.report);
        }
        for (auto& [k, f] : unclassified_) res_.unclassified.push_back(f);
        std::sort(res_.unclassified.begin(), res_.unclassified.end(), [](const UdpFlowCount& a, const UdpFlowCount& b) {
            return a.packets != b.packets ? a.packets > b.packets : std::tie(a.dst, a.port) < std::tie(b.dst, b.port);
        });
    }

private:
    using FlowKey = std::pair<std::uint32_t, std::uint16_t>;
    using ObjectKey = std::tuple<std::uint32_t, std::uint16_t, std::uint64_t, std::uint64_t>;

    void note(const io::UnitTime& t) {
        if (t.capture_ns == 0) return;
        if (res_.first_capture_ns == 0) res_.first_capture_ns = t.capture_ns;
        res_.last_capture_ns = t.capture_ns;
    }

    void onAlpPacket(std::uint8_t plp, ByteView bytes) {
        const std::uint64_t index = res_.alp.packets++;
        alp::Header h;
        const alp::ParseStatus st = alp::parseHeader(bytes, h);
        if (st == alp::ParseStatus::Ok) {
            ++res_.alp.types[alp::toString(h.type)];
            ++res_.alp.layouts[layoutName(h.layout)];
            if (h.sid) ++res_.alp.sids[*h.sid];
            if (h.extension) {
                char key[16];
                std::snprintf(key, sizeof key, "0x%02x ", h.extension->type);
                ++res_.alp.extensions[key + hex(h.extension->bytes.data(), h.extension->bytes.size())];
            }
            const long trailing = h.layout == alp::Layout::MpegTs ? 0 : static_cast<long>(bytes.size()) - static_cast<long>(h.totalLength());
            if (h.payload_length == 0 || trailing != 0) {
                ++res_.alp.odd;
                if (res_.alp.odd_packets.size() < opts_.max_odd_packets)
                    res_.alp.odd_packets.push_back({index, layoutName(h.layout), h.payload_length, trailing, hex(bytes.data(), std::min<std::size_t>(bytes.size(), 16))});
            }
        } else {
            ++res_.alp.types["invalid"];
            ++res_.alp.odd;
            if (res_.alp.odd_packets.size() < opts_.max_odd_packets)
                res_.alp.odd_packets.push_back({index, "invalid", 0, 0, hex(bytes.data(), std::min<std::size_t>(bytes.size(), 16))});
        }
        decap_.feed(plp, bytes);
    }

    void onUdp(const ip::UdpDatagram& d) {
        UdpFlowCount& f = udp_flows_[std::make_tuple(d.src_ip, d.dst_ip, d.dst_port)];
        f.src = d.src_ip;
        f.dst = d.dst_ip;
        f.port = d.dst_port;
        f.plp = d.plp;
        ++f.packets;
        f.bytes += 28 + d.payload.size();
        if (lls_.onDatagram(d)) return;
        route::LctPacket lct;
        if (route::parseLct(d.payload, lct) == route::LctStatus::Ok) {
            onLct(d, lct);
            return;
        }
        mmtp::PacketHeader mh;
        if (mmtp::parsePacket(d.payload, mh) == mmtp::ParseStatus::Ok && mh.version == 1) {
            onMmtp(d, mh);
            return;
        }
        UdpFlowCount& u = unclassified_[{d.dst_ip, d.dst_port}];
        u.dst = d.dst_ip;
        u.port = d.dst_port;
        u.plp = d.plp;
        ++u.packets;
        u.bytes += 28 + d.payload.size();
    }

    void onLct(const ip::UdpDatagram& d, const route::LctPacket& p) {
        const FlowKey key{d.dst_ip, d.dst_port};
        auto it = lct_flows_.find(key);
        if (it == lct_flows_.end()) {
            it = lct_flows_.emplace(key, LctFlowReport{}).first;
            it->second.dst = d.dst_ip;
            it->second.port = d.dst_port;
            lct_order_.push_back(key);
        }
        LctFlowReport& f = it->second;
        const route::LctHeader& h = p.header;
        ++f.packets;
        ++f.tsi[h.tsi];
        ++f.codepoints[h.codepoint];
        for (const route::LctExtension& e : h.extensions) ++f.hets[e.het];
        char shape[32];
        std::snprintf(shape, sizeof shape, "%u,%u,%u,%u,%u", h.c, h.s, h.o, h.h, h.hdr_len_words);
        ++f.header_shapes[shape];
        if (h.close_object) ++f.close_object;
        if (h.close_session) ++f.close_session;
        lct_objects_[key].insert({h.tsi, h.toi});
        if (p.dataless) return;
        const ObjectKey ok{d.dst_ip, d.dst_port, h.tsi, h.toi};
        ObjectState& st = objects_[ok];
        st.keep_data = opts_.keep_sls_objects && h.tsi == 0;
        st.add(p.start_offset, p.payload);
        if (h.transfer_length) st.transfer_length = h.transfer_length;
    }

    void onMmtp(const ip::UdpDatagram& d, const mmtp::PacketHeader& h) {
        const FlowKey key{d.dst_ip, d.dst_port};
        auto it = mmtp_flows_.find(key);
        if (it == mmtp_flows_.end()) {
            it = mmtp_flows_.emplace(key, MmtpFlowState{}).first;
            MmtpFlowState& st = it->second;
            st.report.dst = d.dst_ip;
            st.report.port = d.dst_port;
            mmtp_order_.push_back(key);
            MmtpFlowState* raw = &st;
            mmtp::SessionCallbacks cb;
            cb.onMessage = [this, raw](std::uint16_t pid, const mmtp::Message& m) { onMmtMessage(*raw, pid, m); };
            st.session = std::make_unique<mmtp::Session>(std::move(cb));
        }
        MmtpFlowState& st = it->second;
        MmtpFlowReport& f = st.report;
        ++f.packets;
        ++f.packet_ids[h.packet_id];
        ++f.payload_types[mmtp::toString(h.type)];
        ++f.byte0[d.payload[0]];
        ++f.byte1[d.payload[1]];
        if (h.type == mmtp::PayloadType::Mpu) {
            mmtp::MpuPayloadHeader mpu;
            if (mmtp::parseMpuPayload(h.payload, mpu) == mmtp::ParseStatus::Ok) {
                ++f.mpu_fragment_types[mpu.fragment_type];
                ++f.mpu_fragmentation[mpu.fragmentation_indicator];
                ++f.mpu_aggregation[mpu.aggregation ? 1 : 0];
            } else {
                ++f.bad_header;
            }
        } else if (h.type == mmtp::PayloadType::Signaling) {
            mmtp::SignalingPayloadHeader sig;
            if (mmtp::parseSignalingPayload(h.payload, sig) == mmtp::ParseStatus::Ok) {
                ++f.signaling_fragmentation[std::to_string(sig.fragmentation_indicator) + "," + (sig.aggregation ? "1" : "0")];
            } else {
                ++f.bad_header;
            }
        }
        st.session->onDatagram(d);
    }

    void onMmtMessage(MmtpFlowState& st, std::uint16_t pid, const mmtp::Message& m) {
        MmtpFlowReport& f = st.report;
        char key[32];
        std::snprintf(key, sizeof key, "pid %u: 0x%04x", pid, m.message_id);
        ++f.messages[key];
        if (m.mp_table && f.mp_tables.size() < opts_.max_mp_tables) f.mp_tables.push_back({m.message_id, pid, *m.mp_table});
        if (m.atsc3) {
            ++f.atsc3_content_types[m.atsc3->content_type];
            const std::string seen = std::to_string(m.atsc3->content_type) + "|" + m.atsc3->uri;
            if (st.atsc3_seen.insert(seen).second) {
                Atsc3MessageReport r;
                r.packet_id = pid;
                r.message = *m.atsc3;
                r.content_length = m.atsc3->content.size();
                if (r.message.content.size() > 300) r.message.content.resize(300);
                f.atsc3_messages.push_back(std::move(r));
            }
        }
    }

    const ScanOptions& opts_;
    ScanReport& res_;
    io::UnitTime time_;
    ip::Demux demux_;
    alp::Decap decap_;
    phy::BbpDeframer deframer_;
    link::LinkSignaling link_;
    lls::Processor lls_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, UdpFlowCount> udp_flows_;
    std::map<FlowKey, LctFlowReport> lct_flows_;
    std::map<FlowKey, std::set<std::pair<std::uint64_t, std::uint64_t>>> lct_objects_;
    std::vector<FlowKey> lct_order_;
    std::map<ObjectKey, ObjectState> objects_;
    std::map<FlowKey, MmtpFlowState> mmtp_flows_;
    std::vector<FlowKey> mmtp_order_;
    std::map<FlowKey, UdpFlowCount> unclassified_;
};

class Sink final : public service::ITunerSink {
public:
    explicit Sink(Scanner& s) : scanner_(s) {}
    void onBbp(const io::BasebandPacket& p) override { scanner_.onBbp(p); }
    void onAlp(const io::AlpPacket& p) override { scanner_.onAlp(p); }
    void onIp(const io::IpDatagram& p) override { scanner_.onIp(p); }
    void onTunerEvent(const service::TunerEvent& e) override { scanner_.onEvent(e); }

private:
    Scanner& scanner_;
};

// ---- printing ------------------------------------------------------------------------------------------------------

template <class M, class K, class V>
void printMap(std::ostream& os, const M& m, K key, V value) {
    os << "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) os << ", ";
        first = false;
        key(os, k);
        os << ": ";
        value(os, v);
    }
    os << "}";
}
template <class M>
void printCounts(std::ostream& os, const M& m) {
    printMap(os, m, [](std::ostream& o, const auto& k) { o << +k; }, [](std::ostream& o, const auto& v) { o << v; });
}
template <class M>
void printNamedCounts(std::ostream& os, const M& m) {
    printMap(os, m, [](std::ostream& o, const std::string& k) { o << k; }, [](std::ostream& o, const auto& v) { o << v; });
}
std::string hex4(std::uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%04x", v);
    return b;
}
std::string hex2(std::uint8_t v) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%02x", v);
    return b;
}

}  // namespace

double ScanReport::durationSeconds() const noexcept {
    return first_capture_ns && last_capture_ns >= first_capture_ns ? static_cast<double>(last_capture_ns - first_capture_ns) / 1e9 : 0.0;
}

std::string ScanReport::text(const ScanPrint& print) const {
    std::ostringstream os;
    os << detection << "\n";
    os << "tuner: output=" << service::toString(output) << " records=" << tuner.records << " delivered bbp=" << tuner.delivered_bbp
       << " alp=" << tuner.delivered_alp << " ip=" << tuner.delivered_ip << " dropped non_ip=" << tuner.dropped_non_ip
       << " non_udp=" << tuner.dropped_non_udp << " mixed=" << tuner.dropped_mixed << " filtered=" << tuner.dropped_filtered
       << " time_jumps=" << tuner.time_jumps;
    char dur[32];
    std::snprintf(dur, sizeof dur, "%.3f", durationSeconds());
    os << " duration=" << dur << "s\n";
    if (!sidecar.empty()) os << "sidecar: " << sidecar << "\n";
    if (l1) {
        os << "l1 (" << l1->source << "):";
        for (const io::L1Info::Plp& p : l1->plps) {
            os << " plp " << +p.plp_id;
            if (!p.modulation.empty()) os << " " << p.modulation;
            if (!p.code_rate.empty()) os << " " << p.code_rate;
            if (p.lls_flag) os << " lls";
            os << ";";
        }
        os << "\n";
    }
    if (stltp) {
        const stltp::Stats& s = *stltp;
        os << "stltp: outer=" << s.outer_packets << " not_rtp=" << s.outer_not_rtp << " other_pt=" << s.outer_other_pt
           << " seq_gaps=" << s.outer_seq_gaps << " resync=" << s.outer_resync << " protocol_version=" << +s.protocol_version
           << " inner=" << s.inner_packets << " bbp=" << s.bbp << " preamble=" << s.preamble << " tmp=" << s.tmp
           << " security=" << s.security << " other=" << s.inner_other << " alptp=" << s.alptp_packets << " bpps no_start="
           << s.bpps_no_start << " seq_gap=" << s.bpps_seq_gap << " incomplete=" << s.bpps_incomplete << " overrun=" << s.bpps_overrun
           << "\n";
    }
    if (!bbp.per_plp.empty()) {
        os << "bbp: per plp ";
        printCounts(os, bbp.per_plp);
        os << " lengths ";
        printCounts(os, bbp.lengths);
        os << " ofi ";
        printCounts(os, bbp.ofi);
        os << " ext(ofi,type) ";
        printMap(os, bbp.extensions, [](std::ostream& o, const auto& k) { o << "(" << +k.first << "," << +k.second << ")"; },
                 [](std::ostream& o, const auto& v) { o << v; });
        os << " pointer 0=" << bbp.pointer_zero << " 8191=" << bbp.pointer_no_start << " other=" << bbp.pointer_other
           << " invalid=" << bbp.invalid << " errors=" << bbp.errors << "\n";
        const phy::DeframerStats& d = bbp.deframer;
        os << "deframer: alp=" << d.alp << " pointer_no_start=" << d.pointer_no_start << " orphan_prefix=" << d.orphan_prefix
           << " pending_mismatch=" << d.pending_mismatch << " alp_invalid=" << d.alp_invalid << " counter_jumps=" << d.counter_jumps
           << " bbp_invalid=" << d.bbp_invalid << " flushed=" << d.flushed << "\n";
    }
    if (alp.packets) {
        os << "alp: packets=" << alp.packets << " types ";
        printNamedCounts(os, alp.types);
        os << " layouts ";
        printNamedCounts(os, alp.layouts);
        os << " sids ";
        printCounts(os, alp.sids);
        os << " signaling ";
        printCounts(os, alp.signaling_types);
        os << "\n  extensions ";
        printNamedCounts(os, alp.extensions);
        os << "\n  odd packets (zero length or trailing bytes): " << alp.odd;
        for (const AlpOddPacket& p : alp.odd_packets)
            os << " [#" << p.index << " " << p.layout << " len=" << p.length << " trailing=" << p.trailing << " " << p.head_hex << "]";
        if (alp.odd > alp.odd_packets.size()) os << " ...";
        os << "\n";
        const alp::Stats& d = alp.decap;
        os << "  decap: ipv4=" << d.ipv4 << " compressed_ip=" << d.compressed_ip << " signaling=" << d.signaling << " type_extension="
           << d.type_extension << " ts=" << d.ts << " segments=" << d.segments << " reassembled=" << d.reassembled << " concatenated="
           << d.concatenated << " components=" << d.components << " zero_length=" << d.zero_length << " private_extension="
           << d.private_extension << " reserved_type=" << d.reserved_type << " invalid=" << d.invalid << " truncated=" << d.truncated
           << " reassembly_drops=" << d.reassembly_drops << " unsupported=" << d.unsupported << "\n";
        os << "link: signaling=" << link.tables << " lmt=" << link.lmt << " (new " << link.lmt_new << ") rdt=" << link.rdt << " user_private="
           << link.user_private << " invalid=" << link.invalid << " unsupported=" << link.unsupported << "\n";
        for (const LmtReport& l : lmts) {
            os << "  LMT ver=" << +l.lmt.version << " carrier plp=" << +l.carrier_plp << " num_PLPs=" << l.lmt.plps.size() << "\n";
            for (const link::LmtPlp& p : l.lmt.plps)
                for (const link::LmtMulticast& m : p.multicasts) {
                    os << "    PLP " << +p.plp_id << ": " << io::ipv4ToString(m.src_ip) << ":" << m.src_port << " -> "
                       << io::ipv4ToString(m.dst_ip) << ":" << m.dst_port;
                    if (m.sid) os << " SID=" << +*m.sid;
                    if (m.context_id) os << " CID=" << +*m.context_id;
                    os << "\n";
                }
        }
    }
    os << "ip: ipv4=" << demux.ipv4 << " bad=" << demux.bad_ipv4 << " checksum_bad=" << demux.ip_checksum_bad << " non_udp=" << demux.non_udp
       << " other=" << demux.ipv6_or_other << " fragments=" << demux.fragments << " reassembled=" << demux.reassembled << " fragments_dropped="
       << demux.fragments_dropped << " udp=" << demux.udp << " udp_bad_length=" << demux.udp_bad_length << " udp_checksum_bad="
       << demux.udp_checksum_bad << "\n";
    os << "udp flows (src -> dst:port  packets  bytes):\n";
    for (const UdpFlowCount& f : udp_flows)
        os << "  " << io::ipv4ToString(f.src) << " -> " << flowName(f.dst, f.port) << "  " << f.packets << "  " << f.bytes << "\n";
    os << "lls: datagrams=" << lls.datagrams << " tables=" << lls.tables << " new=" << lls.new_tables << " duplicates=" << lls.duplicates
       << " signed_multi_tables=" << lls.signed_multi_tables << " bad_header=" << lls.bad_header << " gunzip_failed=" << lls.gunzip_failed
       << " parse_failed=" << lls.parse_failed << "\n";
    // tables in arrival order, the entries of a SignedMultiTable indented under it
    {
        std::size_t smt = 0;
        for (std::size_t i = 0; i < lls_tables.size();) {
            const LlsTableReport& t = lls_tables[i];
            if (t.in_signed_multi_table && smt < signed_multi_tables.size()) {
                const SignedMultiTableReport& s = signed_multi_tables[smt++];
                os << "  SignedMultiTable group=" << +s.group << " ver=" << +s.version << " src=" << io::ipv4ToString(s.source_ip)
                   << " count=" << s.entries.size() << " siglen=" << s.signature_length << "\n";
                for (std::size_t k = 0; k < s.entries.size() && i < lls_tables.size() && lls_tables[i].in_signed_multi_table; ++k, ++i) {
                    const LlsTableReport& e = lls_tables[i];
                    os << "    sub table_id=" << hex2(e.table_id) << " " << lls::tableName(e.table_id) << " ver=" << +e.version << " len=" << e.length
                       << ": " << e.preview << "\n";
                    if (print.slt && e.table_id == lls::kSlt) os << e.text << "\n";
                }
                continue;
            }
            os << "  table_id=" << hex2(t.table_id) << " " << lls::tableName(t.table_id) << " group=" << +t.group << " ver=" << +t.version
               << " src=" << io::ipv4ToString(t.source_ip) << " len=" << t.length << ": " << t.preview << "\n";
            if (print.slt && t.table_id == lls::kSlt) os << t.text << "\n";
            ++i;
        }
    }
    os << "lct flows:\n";
    for (const LctFlowReport& f : lct_flows) {
        os << "  " << flowName(f.dst, f.port) << "  packets=" << f.packets << " tsi ";
        printCounts(os, f.tsi);
        os << " cp ";
        printCounts(os, f.codepoints);
        os << " het ";
        printCounts(os, f.hets);
        os << " objects=" << f.objects << " B=" << f.close_object << " A=" << f.close_session << " hdr(C,S,O,H,HL) ";
        printMap(os, f.header_shapes, [](std::ostream& o, const std::string& k) { o << "(" << k << ")"; }, [](std::ostream& o, const auto& v) { o << v; });
        os << "\n";
    }
    os << "mmtp flows:\n";
    for (const MmtpFlowReport& f : mmtp_flows) {
        os << "  " << flowName(f.dst, f.port) << "  packets=" << f.packets << " pid ";
        printCounts(os, f.packet_ids);
        os << " type ";
        printNamedCounts(os, f.payload_types);
        os << " byte0 ";
        printMap(os, f.byte0, [](std::ostream& o, std::uint8_t k) { o << hex2(k); }, [](std::ostream& o, const auto& v) { o << v; });
        os << " byte1 ";
        printMap(os, f.byte1, [](std::ostream& o, std::uint8_t k) { o << hex2(k); }, [](std::ostream& o, const auto& v) { o << v; });
        os << "\n    mpu FT ";
        printCounts(os, f.mpu_fragment_types);
        os << " f_i ";
        printCounts(os, f.mpu_fragmentation);
        os << " aggregation ";
        printCounts(os, f.mpu_aggregation);
        os << "; signaling (f_i,A) ";
        printMap(os, f.signaling_fragmentation, [](std::ostream& o, const std::string& k) { o << "(" << k << ")"; }, [](std::ostream& o, const auto& v) { o << v; });
        os << "; bad_header=" << f.bad_header << " length_mismatch=" << f.length_mismatch << " seq_gaps=" << f.sequence_gaps << "\n";
        os << "    messages ";
        printNamedCounts(os, f.messages);
        os << "\n    mmt_atsc3 content types ";
        printMap(os, f.atsc3_content_types,
                 [](std::ostream& o, std::uint16_t k) { o << hex4(k) << " " << mmtp::atsc3ContentTypeName(k); },
                 [](std::ostream& o, const auto& v) { o << v; });
        os << "\n";
        if (!print.mmt) continue;
        for (const Atsc3MessageReport& m : f.atsc3_messages)
            os << "    mmt_atsc3_message pid=" << m.packet_id << " service_id=" << m.message.service_id << " content_type=" << hex4(m.message.content_type)
               << " " << mmtp::atsc3ContentTypeName(m.message.content_type) << " ver=" << +m.message.content_version << " comp="
               << +m.message.compression << " uri='" << m.message.uri << "' len=" << m.content_length << ": "
               << preview(m.message.content.data(), m.message.content.size(), 300) << "\n";
        for (const MpTableReport& t : f.mp_tables) {
            os << "    MPT msg=" << hex4(t.message_id) << " pid=" << t.packet_id << " table_id=" << hex2(t.table.table_id) << " ver=" << +t.table.version
               << " mode=" << +t.table.mode << " package_id='" << t.table.package_id << "' assets=" << t.table.assets.size() << "\n";
            for (const mmtp::Asset& a : t.table.assets) {
                os << "      asset scheme=" << a.asset_id_scheme << " id='" << a.assetIdText() << "' type='" << a.asset_type << "' default="
                   << (a.default_asset ? 1 : 0) << " clockrel=" << (a.clock_relation_id ? std::to_string(*a.clock_relation_id) : "-") << " timescale="
                   << (a.timescale ? std::to_string(*a.timescale) : "-") << " locs=[";
                for (std::size_t i = 0; i < a.locations.size(); ++i) {
                    const mmtp::AssetLocation& l = a.locations[i];
                    if (i) os << ", ";
                    if (l.type == 0) os << "pid " << l.packet_id;
                    else if (l.type == 1) os << io::ipv4ToString(l.dst_ip) << ":" << l.dst_port << " pid " << l.packet_id;
                    else os << "type " << +l.type;
                }
                os << "] desc=[";
                for (std::size_t i = 0; i < a.mpu_timestamps.size(); ++i) {
                    if (i) os << ", ";
                    char b[64];
                    std::snprintf(b, sizeof b, "mpu_timestamp(seq=%u, ntp=0x%016llx)", a.mpu_timestamps[i].mpu_sequence_number,
                                  static_cast<unsigned long long>(a.mpu_timestamps[i].ntp));
                    os << b;
                }
                for (std::uint16_t tag : a.descriptor_tags)
                    if (tag != 0x0001) os << (a.mpu_timestamps.empty() ? "" : ", ") << hex4(tag);
                os << "]\n";
            }
        }
    }
    if (!unclassified.empty()) {
        os << "unclassified udp flows (dst:port  packets  bytes):\n";
        for (const UdpFlowCount& f : unclassified) os << "  " << flowName(f.dst, f.port) << "  " << f.packets << "  " << f.bytes << "\n";
    }
    os << "objects: complete=" << objects_complete << " incomplete=" << objects_incomplete << "\n";
    if (print.objs) {
        for (const ObjectReport& o : objects) {
            os << "  obj " << flowName(o.dst, o.port) << " tsi=" << o.tsi << " toi=" << o.toi << " bytes=" << o.received << " span=" << o.extent
               << " tol=" << (o.transfer_length ? std::to_string(*o.transfer_length) : "-") << " first=" << o.first_hex
               << (o.complete ? " complete" : o.contiguous ? " contiguous" : " incomplete") << "\n";
        }
    }
    if (print.sls) {
        for (const ObjectReport& o : objects) {
            if (o.tsi != 0 || !o.contiguous || o.data.empty()) continue;
            os << "--- SLS object " << flowName(o.dst, o.port) << " toi=" << o.toi << " len=" << o.data.size()
               << " tol=" << (o.transfer_length ? std::to_string(*o.transfer_length) : "-") << "\n";
            os << preview(o.data.data(), o.data.size(), print.max_sls_bytes) << "\n";
        }
    }
    return os.str();
}

int scanCapture(const ScanOptions& opts, ScanReport& report, std::string* error) {
    pcap::VirtualTunerConfig cfg;
    cfg.path = opts.input;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.max_records = opts.max_records;
    cfg.start_offset_ns = opts.start_offset_ns;
    cfg.duration_ns = opts.duration_ns;
    cfg.sidecar = opts.sidecar;
    if (opts.probe_packets) cfg.detector.probe_packets = opts.probe_packets;
    pcap::VirtualTuner tuner(cfg);
    report.detection = tuner.detection().summary();
    report.mode = tuner.mode();
    if (tuner.openError() != service::TunerError::None) {
        if (error) *error = tuner.openMessage();
        return tuner.openError() == service::TunerError::NoAtscContent ? 3 : 2;
    }
    report.output = tuner.output();
    if (tuner.sidecar()) report.sidecar = tuner.sidecar()->path;
    Scanner scanner(opts, report);
    Sink sink(scanner);
    if (tuner.start(sink, tuner.output()) != service::TunerError::None) {
        if (error) *error = "cannot start virtual tuner";
        return 2;
    }
    while (!tuner.waitFinished(std::chrono::seconds(3600))) {
    }
    tuner.stop();
    scanner.finish();
    report.tuner = tuner.stats();
    report.stltp = tuner.stltpStats();
    report.l1 = tuner.l1();
    return 0;
}

}  // namespace atsc3::tools
