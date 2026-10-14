#include "atsc3/tools/routedump.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "atsc3/io/inet.hpp"
#include "atsc3/route/multipart.hpp"
#include "atsc3/sls/route_receiver.hpp"

namespace fs = std::filesystem;

namespace atsc3::tools {

namespace {

bool writeFile(const fs::path& p, const std::uint8_t* data, std::size_t n, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot write " + p.string();
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}
bool writeFile(const fs::path& p, const std::string& s, std::string& err) {
    return writeFile(p, reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), err);
}

std::string sanitizeComponent(std::string s) {
    for (char& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) c = '_';
    }
    while (!s.empty() && s.front() == '.') s.erase(s.begin());
    return s.empty() ? "_" : s;
}

bool isMediaType(const std::optional<std::string>& ct) {
    if (!ct) return false;
    return *ct == "video" || *ct == "audio" || *ct == "subtitles" || *ct == "text";
}

// Everything one service writes: SLS fragments, objects, the per-representation segment registry.
struct ServiceState {
    ServiceReport report;
    fs::path dir;
    std::unique_ptr<sls::RouteReceiver> receiver;
    std::optional<sls::SlsBundle> sls;
    std::string mpd_xml;
    std::map<std::string, std::set<std::uint64_t>> segments;        // rep id -> keys
    std::map<std::string, RepresentationReport> reps;
    std::set<std::string> written;                                  // relative paths written once
    std::set<std::uint64_t> seen_objects;                           // (tsi<<32|toi) for media de-dup
};

class Dumper {
public:
    Dumper(const RouteDumpOptions& o, RouteDumpResult& r) : opts_(o), res_(r) {
        lls::Callbacks lc;
        lc.onSlt = [this](const lls::TableInfo& info, const lls::Slt& slt) { onSlt(info, slt); };
        lc.onError = [this](const std::string& m) { note("LLS: " + m); };
        lls_ = std::make_unique<lls::Processor>(std::move(lc));
        demux_.tapAll([this](const ip::UdpDatagram& d) { lls_->onDatagram(d); });
    }

    void onSlt(const lls::TableInfo& info, const lls::Slt& slt) {
        const std::uint16_t bsid = slt.bsid.empty() ? 0 : slt.bsid.front();
        for (const lls::SltService& svc : slt.services) {
            if (!opts_.services.empty() && !opts_.services.count(svc.service_id)) continue;
            const std::uint32_t k = (static_cast<std::uint32_t>(info.header.group_id) << 16) | svc.service_id;
            if (services_.count(k)) continue;
            auto st = std::make_unique<ServiceState>();
            ServiceReport& rep = st->report;
            rep.lls_group = info.header.group_id;
            rep.bsid = bsid;
            rep.service_id = svc.service_id;
            rep.name = svc.short_service_name.value_or("");
            std::string dirname = std::to_string(svc.service_id);
            if (!rep.name.empty()) dirname += "-" + sanitizeComponent(rep.name);
            st->dir = fs::path(opts_.out_dir) / ("bsid-" + std::to_string(bsid)) / dirname;
            rep.dir = st->dir.string();
            if (!svc.bss) {
                rep.protocol = svc.configuration == lls::SltService::Configuration::Broadband ? "broadband" : "none";
                rep.skipped = true;
                rep.skip_reason = "no broadcast signaling";
            } else if (svc.bss->sls_protocol == 2) {
                rep.protocol = "MMTP";
                rep.skipped = true;
                rep.skip_reason = "MMTP service (not extracted by routedump)";
            } else if (svc.bss->sls_protocol != 1) {
                rep.protocol = "unknown";
                rep.skipped = true;
                rep.skip_reason = "slsProtocol " + std::to_string(svc.bss->sls_protocol);
            } else {
                rep.protocol = "ROUTE";
                ServiceState* raw = st.get();
                sls::RouteReceiverCallbacks cb;
                cb.onSls = [this, raw](const sls::SlsBundle& b) { onSls(*raw, b); };
                cb.onObject = [this, raw](std::shared_ptr<const route::DeliveredObject> o, const sls::SessionAddress& a) { onObject(*raw, *o, a); };
                cb.onObjectError = [raw](const route::ObjectError& e, const sls::SessionAddress& a) {
                    ++raw->report.object_errors;
                    if (raw->report.warnings.size() < 50)
                        raw->report.warnings.push_back("object " + a.toString() + " tsi " + std::to_string(e.tsi) + " toi " + std::to_string(e.toi) + ": " + e.reason);
                };
                cb.onError = [raw](const std::string& m) { raw->report.warnings.push_back(m); };
                st->receiver = sls::RouteReceiver::fromSlt(demux_, svc, std::move(cb));
                if (!st->receiver) {
                    rep.skipped = true;
                    rep.skip_reason = "incomplete BroadcastSvcSignaling";
                }
            }
            services_.emplace(k, std::move(st));
        }
    }

    void onSls(ServiceState& st, const sls::SlsBundle& b) {
        ++st.report.sls_updates;
        st.sls = b;
        st.report.have_usbd |= b.usbd.has_value();
        st.report.have_stsid |= b.stsid.has_value();
        st.report.have_mpd |= b.mpd.has_value();
        st.report.have_held |= b.held.has_value();
        if (b.mpd) st.mpd_xml = b.fragment(sls::FragmentKind::Mpd)->text;
        if (st.report.name.empty() && b.usbd && !b.usbd->names.empty()) st.report.name = b.usbd->names.front().text;
        for (const std::string& w : b.warnings) st.report.warnings.push_back("sls: " + w);
        if (!opts_.write_sls) return;
        std::string err;
        const fs::path sdir = st.dir / "sls";
        for (const sls::SlsFragment& f : b.fragments) {
            std::string name = f.location.empty() ? std::string(sls::toString(f.kind)) + ".xml" : safeRelativePath(f.location);
            if (!writeFile(sdir / name, f.text, err)) note(err);
            else count(f.text.size());
            // stable names for the typed fragments as well
            const char* alias = nullptr;
            switch (f.kind) {
                case sls::FragmentKind::Usbd: alias = "usbd.xml"; break;
                case sls::FragmentKind::Stsid: alias = "stsid.xml"; break;
                case sls::FragmentKind::Mpd: alias = "mpd.xml"; break;
                case sls::FragmentKind::Held: alias = "held.xml"; break;
                case sls::FragmentKind::Envelope: alias = "envelope.xml"; break;
                default: break;
            }
            if (alias && name != alias) writeFile(sdir / alias, f.text, err);
        }
        char toi[32];
        std::snprintf(toi, sizeof toi, "bundle-toi-0x%08X.mime", b.toi);
        const auto* bundle_obj = st.receiver ? st.receiver->sls() : nullptr;
        (void)bundle_obj;
        std::ostringstream info;
        info << "toi=" << b.toi << " version=" << b.toi_bits.version << " signed=" << (b.is_signed ? 1 : 0) << "\n";
        for (const sls::SlsFragment& f : b.fragments)
            info << sls::toString(f.kind) << " " << f.content_type << " " << f.location << (f.version ? " v" + std::to_string(*f.version) : "") << "\n";
        writeFile(sdir / (std::string(toi) + ".txt"), info.str(), err);
    }

    // Finds the Representation a media object belongs to and its $Number$/$Time$ key (sls::matchSegment).
    static bool classifySegment(ServiceState& st, const route::DeliveredObject& obj, const std::string& name, std::string& rep_id,
                                std::uint64_t& key, bool& is_init) {
        is_init = false;
        if (!st.sls || !st.sls->mpd) return false;
        const auto m = sls::matchSegment(*st.sls->mpd, name, obj.rep_id, obj.toi);
        if (!m) return false;
        rep_id = m->rep_id;
        key = m->key;
        is_init = m->is_init;
        return true;
    }

    void onObject(ServiceState& st, const route::DeliveredObject& obj, const sls::SessionAddress& addr) {
        std::string err;
        const bool media_channel = isMediaType(obj.media_content_type) || obj.rep_id.has_value();
        std::string name = obj.content_location ? safeRelativePath(*obj.content_location) : "";
        if (name.empty()) name = "tsi" + std::to_string(obj.tsi) + "-toi" + std::to_string(obj.toi) + ".bin";
        std::string rep_id;
        std::uint64_t key = 0;
        bool is_init = false;
        const bool classified = classifySegment(st, obj, name, rep_id, key, is_init);
        if (classified || media_channel) {
            const std::uint64_t id = (static_cast<std::uint64_t>(obj.tsi) << 32) | obj.toi;
            const bool repeat = !st.seen_objects.insert(id).second && !is_init;
            if (!repeat || !st.written.count(name)) {
                if (!writeFile(st.dir / name, obj.data.data(), obj.data.size(), err)) {
                    note(err);
                    return;
                }
                st.written.insert(name);
                count(obj.data.size());
            }
            ++st.report.media_objects;
            st.report.media_bytes += obj.data.size();
            if (classified) {
                RepresentationReport& rr = st.reps[rep_id];
                rr.rep_id = rep_id;
                if (!rr.content_type) rr.content_type = obj.media_content_type;
                if (is_init) rr.init = true;
                else st.segments[rep_id].insert(key);
            } else if (st.report.warnings.size() < 50) {
                st.report.warnings.push_back("media object " + name + " (tsi " + std::to_string(obj.tsi) + ") matches no MPD Representation");
            }
            return;
        }
        if (!opts_.write_nrt) return;
        const fs::path base = st.dir / "nrt";
        if (!writeFile(base / name, obj.data.data(), obj.data.size(), err)) {
            note(err);
            return;
        }
        count(obj.data.size());
        ++st.report.nrt_objects;
        st.report.nrt_bytes += obj.data.size();
        const bool package = obj.format == route::FormatId::UnsignedPackage || obj.format == route::FormatId::SignedPackage ||
                             (obj.content_type && obj.content_type->rfind("multipart/", 0) == 0);
        if (package && opts_.unpack_packages) {
            route::Multipart mp;
            if (route::parseMultipart(ByteView(obj.data.data(), obj.data.size()), mp)) {
                const fs::path pdir = base / (name + ".d");
                std::size_t n = 0;
                for (const route::MimePart& part : mp.parts) {
                    std::string pname = part.headers.contentLocation() ? safeRelativePath(*part.headers.contentLocation()) : "part-" + std::to_string(n);
                    if (pname.empty()) pname = "part-" + std::to_string(n);
                    if (writeFile(pdir / pname, part.body.data(), part.body.size(), err)) count(part.body.size());
                    ++n;
                }
            }
        }
        (void)addr;
    }

    void finish() {
        for (auto& [k, stp] : services_) {
            ServiceState& st = *stp;
            ServiceReport& rep = st.report;
            for (auto& [rid, rr] : st.reps) {
                const auto segs = st.segments.find(rid);
                if (segs != st.segments.end() && !segs->second.empty()) {
                    rr.segments = segs->second.size();
                    rr.first_key = *segs->second.begin();
                    rr.last_key = *segs->second.rbegin();
                    std::uint64_t prev = rr.first_key;
                    for (const std::uint64_t v : segs->second) {
                        if (v > prev + 1 && st.sls && st.sls->mpd) {
                            const sls::Representation* r = st.sls->mpd->representation(rid);
                            if (r && sls::addressingOf(r->segment_template) == sls::Addressing::Number) ++rr.gaps;
                        }
                        prev = v;
                    }
                }
                rep.representations.push_back(rr);
            }
            if (!st.mpd_xml.empty() && !st.segments.empty()) {
                std::map<std::string, std::vector<std::uint64_t>> segs;
                for (const auto& [rid, s] : st.segments) segs[rid].assign(s.begin(), s.end());
                std::string out, err;
                if (sls::makeStaticMpd(st.mpd_xml, segs, out, &err)) {
                    const fs::path p = st.dir / "manifest.mpd";
                    if (writeFile(p, out, err)) {
                        rep.mpd_path = p.string();
                        count(out.size());
                    } else {
                        rep.mpd_error = err;
                    }
                } else {
                    rep.mpd_error = err;
                }
            } else if (rep.protocol == "ROUTE" && !rep.skipped) {
                rep.mpd_error = st.mpd_xml.empty() ? "no MPD received" : "no media segments captured";
            }
            if (st.receiver) {
                rep.sls_updates = st.receiver->stats().sls_updates;
                rep.object_errors = st.receiver->stats().object_errors;
            }
            res_.services.push_back(rep);
        }
        std::sort(res_.services.begin(), res_.services.end(), [](const ServiceReport& a, const ServiceReport& b) {
            return std::tie(a.bsid, a.service_id) < std::tie(b.bsid, b.service_id);
        });
        res_.demux = demux_.stats();
        res_.lls = lls_->stats();
        res_.services.shrink_to_fit();
    }

    ip::Demux& demux() { return demux_; }
    void note(const std::string& m) {
        if (notes_.size() < 200) notes_.push_back(m);
    }
    const std::vector<std::string>& notes() const { return notes_; }

private:
    void count(std::size_t n) {
        ++res_.files_written;
        res_.bytes_written += n;
    }
    const RouteDumpOptions& opts_;
    RouteDumpResult& res_;
    ip::Demux demux_;
    std::unique_ptr<lls::Processor> lls_;
    std::map<std::uint32_t, std::unique_ptr<ServiceState>> services_;
    std::vector<std::string> notes_;
};

// Tuner sink: BBP -> deframer -> decap -> demux, ALP -> decap -> demux, IP -> demux.
class Sink final : public service::ITunerSink {
public:
    explicit Sink(Dumper& d)
        : dumper_(d),
          decap_([this] {
              alp::Callbacks cb;
              cb.onIpv4 = [this](std::uint8_t plp, ByteView ip, std::optional<std::uint8_t> sid) { dumper_.demux().onIpv4(plp, ip, time_, sid); };
              return cb;
          }()),
          deframer_([this](std::uint8_t plp, ByteView alp) { decap_.feed(plp, alp); }) {}
    void onBbp(const io::BasebandPacket& p) override {
        time_ = p.time;
        deframer_.feed(p.plp_id, p.bytes, p.error);
    }
    void onAlp(const io::AlpPacket& p) override {
        time_ = p.time;
        decap_.feed(p.plp_id, p.bytes);
    }
    void onIp(const io::IpDatagram& p) override { dumper_.demux().onIpv4(p.plp_id, p.bytes, p.time); }
    void onTunerEvent(const service::TunerEvent& e) override {
        if (e.type == service::TunerEventType::FormatDetected) detection_ = e.message;
    }
    const std::string& detection() const { return detection_; }
    const phy::DeframerStats& deframer() const { return deframer_.stats(); }
    const alp::Stats& decap() const { return decap_.stats(); }

private:
    Dumper& dumper_;
    io::UnitTime time_;
    std::string detection_;
    alp::Decap decap_;
    phy::BbpDeframer deframer_;
};

}  // namespace

std::string safeRelativePath(const std::string& loc) {
    std::string s = loc;
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) {  // drop scheme and host
        const auto slash = s.find('/', scheme + 3);
        s = slash == std::string::npos ? "" : s.substr(slash + 1);
    }
    if (s.rfind("tag:", 0) == 0 || s.rfind("urn:", 0) == 0) s = sanitizeComponent(s);
    const auto q = s.find_first_of("?#");
    if (q != std::string::npos) s.erase(q);
    std::string out;
    std::string comp;
    auto flush = [&] {
        if (comp.empty() || comp == "." || comp == "..") {
            if (comp == "..") {
                if (!out.empty()) out += '/';
                out += "__";
            }
        } else {
            if (!out.empty()) out += '/';
            for (char& c : comp)
                if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < ' ') c = '_';
            out += comp;
        }
        comp.clear();
    };
    for (char c : s) {
        if (c == '/' || c == '\\') flush();
        else comp.push_back(c);
    }
    flush();
    return out;
}

std::string RouteDumpResult::summary() const {
    std::ostringstream os;
    os << detection << "\n";
    os << "records=" << tuner.records << " ip=" << demux.udp << " lls_tables=" << lls.new_tables << " files=" << files_written
       << " bytes=" << bytes_written << "\n";
    for (const ServiceReport& s : services) {
        os << "service " << s.service_id << " (bsid " << s.bsid << ", group " << static_cast<unsigned>(s.lls_group) << ") \"" << s.name << "\" "
           << s.protocol;
        if (s.skipped) {
            os << ": skipped, " << s.skip_reason << "\n";
            continue;
        }
        os << "\n  dir " << s.dir << "\n  sls updates " << s.sls_updates << " usbd=" << s.have_usbd << " stsid=" << s.have_stsid
           << " mpd=" << s.have_mpd << " held=" << s.have_held << "\n  media objects " << s.media_objects << " (" << s.media_bytes
           << " B), nrt objects " << s.nrt_objects << " (" << s.nrt_bytes << " B), object errors " << s.object_errors << "\n";
        for (const RepresentationReport& r : s.representations)
            os << "  rep " << r.rep_id << (r.content_type ? " " + *r.content_type : "") << ": init=" << (r.init ? "yes" : "no") << " segments="
               << r.segments << (r.segments ? " keys " + std::to_string(r.first_key) + ".." + std::to_string(r.last_key) : "")
               << (r.gaps ? " gaps=" + std::to_string(r.gaps) : "") << "\n";
        if (s.mpd_path) os << "  offline MPD: " << *s.mpd_path << "\n";
        else if (!s.mpd_error.empty()) os << "  offline MPD: not written (" << s.mpd_error << ")\n";
        for (std::size_t i = 0; i < s.warnings.size() && i < 10; ++i) os << "  warning: " << s.warnings[i] << "\n";
        if (s.warnings.size() > 10) os << "  ... " << (s.warnings.size() - 10) << " more warnings\n";
    }
    return os.str();
}

int routeDump(const RouteDumpOptions& opts, RouteDumpResult& result, std::string* error) {
    std::error_code ec;
    fs::create_directories(opts.out_dir, ec);
    if (ec) {
        if (error) *error = "cannot create " + opts.out_dir + ": " + ec.message();
        return 2;
    }
    pcap::VirtualTunerConfig cfg;
    cfg.path = opts.input;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = opts.duration_ns;
    cfg.start_offset_ns = opts.start_offset_ns;
    cfg.sidecar = opts.sidecar;
    pcap::VirtualTuner tuner(cfg);
    if (tuner.openError() != service::TunerError::None) {
        if (error) *error = tuner.openMessage();
        return tuner.openError() == service::TunerError::NoAtscContent ? 3 : 2;
    }
    result.mode = tuner.mode();
    Dumper dumper(opts, result);
    Sink sink(dumper);
    if (tuner.start(sink, tuner.output()) != service::TunerError::None) {
        if (error) *error = "cannot start virtual tuner";
        return 2;
    }
    while (!tuner.waitFinished(std::chrono::seconds(3600))) {
    }
    tuner.stop();
    dumper.finish();
    result.detection = sink.detection();
    result.tuner = tuner.stats();
    result.deframer = sink.deframer();
    result.decap = sink.decap();
    std::string err;
    writeFile(fs::path(opts.out_dir) / "summary.txt", result.summary(), err);
    if (!dumper.notes().empty()) {
        std::string n;
        for (const std::string& m : dumper.notes()) n += m + "\n";
        writeFile(fs::path(opts.out_dir) / "notes.txt", n, err);
    }
    bool any = false;
    for (const ServiceReport& s : result.services) any |= !s.skipped && (s.media_objects > 0 || s.nrt_objects > 0 || s.sls_updates > 0);
    if (!any) {
        if (error) *error = result.services.empty() ? "no SLT received (no services)" : "no ROUTE service produced any object";
        return 4;
    }
    return 0;
}

}  // namespace atsc3::tools
