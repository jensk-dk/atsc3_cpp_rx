#pragma once
// A pcap virtual tuner over a capture for Engine::open in tests.
#include <memory>
#include <string>

#include "atsc3/tuners/pcap/virtual_tuner.hpp"

namespace atsc3::test {

inline std::shared_ptr<pcap::VirtualTuner> pcapTuner(const std::string& path, pcap::VirtualTunerConfig cfg = {}) {
    cfg.path = path;
    return std::make_shared<pcap::VirtualTuner>(std::move(cfg));
}

}  // namespace atsc3::test
