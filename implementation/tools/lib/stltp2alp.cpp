#include "atsc3/tools/stltp2alp.hpp"

#include <chrono>

#include "atsc3/tuners/pcap/bbp_container.hpp"
#include "atsc3/tuners/pcap/pcap_reader.hpp"

namespace atsc3::tools {

namespace {
class Sink final : public service::ITunerSink {
public:
    Sink(const Stltp2AlpOptions& o, Stltp2AlpResult& r, pcap::PcapWriter& alp_out, pcap::PcapWriter* bbp_out,
         std::string& err)
        : opts_(o), res_(r), alp_out_(alp_out), bbp_out_(bbp_out), err_(err),
          deframer_([this](std::uint8_t plp, ByteView alp) {
              ++res_.alp_per_plp[plp];
              if (alp_out_.write(current_ts_, alp.data(), alp.size())) ++res_.alp_written;
              else if (err_.empty()) err_ = "write error on ALP output";
          }) {}

    void onBbp(const io::BasebandPacket& p) override {
        if (opts_.plp && *opts_.plp != p.plp_id) return;
        ++res_.bbp_per_plp[p.plp_id];
        current_ts_ = p.time.capture_ns;
        if (bbp_out_) {
            const auto rec = pcap::makeBbpContainerRecord(p.plp_id, p.error ? pcap::kBbpFlagError : 0, p.bytes);
            if (bbp_out_->write(current_ts_, rec)) ++res_.bbp_written;
        }
        deframer_.feed(p.plp_id, p.bytes, p.error);
    }
    void onAlp(const io::AlpPacket& p) override {  // ALPTP tunnels deliver ALP directly
        if (opts_.plp && *opts_.plp != p.plp_id) return;
        ++res_.alp_per_plp[p.plp_id];
        if (alp_out_.write(p.time.capture_ns, p.bytes.data(), p.bytes.size())) ++res_.alp_written;
    }
    void onIp(const io::IpDatagram&) override {}
    void onTunerEvent(const service::TunerEvent& e) override {
        if (e.type == service::TunerEventType::FormatDetected) res_.detection = e.message;
    }
    const phy::DeframerStats& deframerStats() const { return deframer_.stats(); }

private:
    const Stltp2AlpOptions& opts_;
    Stltp2AlpResult& res_;
    pcap::PcapWriter& alp_out_;
    pcap::PcapWriter* bbp_out_;
    std::string& err_;
    std::int64_t current_ts_ = 0;
    phy::BbpDeframer deframer_;
};
}  // namespace

int convertStltpToAlp(const Stltp2AlpOptions& opts, Stltp2AlpResult& result, std::string* error) {
    std::string err;
    pcap::VirtualTunerConfig cfg;
    cfg.path = opts.input;
    cfg.pacing = pcap::Pacing::NonRealTime;
    cfg.duration_ns = opts.duration_ns;
    cfg.start_offset_ns = opts.start_offset_ns;
    cfg.sidecar = "off";
    if (opts.force_stltp) cfg.force_mode = pcap::CaptureFormat::Stltp;
    pcap::VirtualTuner tuner(cfg);
    if (tuner.openError() != service::TunerError::None) {
        if (error) *error = tuner.openMessage();
        return tuner.openError() == service::TunerError::NoAtscContent ? 3 : 2;
    }
    if (tuner.mode() != pcap::CaptureFormat::Stltp) {
        if (error) *error = std::string("input is not STLTP: ") + tuner.detection().summary();
        return 3;
    }
    auto alp_out = pcap::PcapWriter::create(opts.output, pcap::linktype::kAtscAlp, &err);
    if (!alp_out) {
        if (error) *error = err;
        return 2;
    }
    std::unique_ptr<pcap::PcapWriter> bbp_out;
    if (opts.bbp_output) {
        bbp_out = pcap::PcapWriter::create(*opts.bbp_output, pcap::kBbpContainerLinkType, &err);
        if (!bbp_out) {
            if (error) *error = err;
            return 2;
        }
    }
    Sink sink(opts, result, *alp_out, bbp_out.get(), err);
    if (tuner.start(sink, tuner.output()) != service::TunerError::None) {
        if (error) *error = "cannot start virtual tuner";
        return 2;
    }
    while (!tuner.waitFinished(std::chrono::seconds(3600))) {
    }
    tuner.stop();
    result.tuner = tuner.stats();
    result.deframer = sink.deframerStats();
    if (!err.empty()) {
        if (error) *error = err;
        return 2;
    }
    return 0;
}

}  // namespace atsc3::tools
