#include "atsc3/tuners/pcap/bbp_container.hpp"

namespace atsc3::pcap {

bool parseBbpContainerRecord(ByteView record, BbpContainerRecord& out) noexcept {
    if (record.size() < kBbpContainerPrefix + 1) return false;
    out.plp_id = record[0];
    out.flags = record[1];
    if (out.plp_id > 63) return false;
    out.bbp = record.subview(kBbpContainerPrefix);
    return true;
}

std::vector<std::uint8_t> makeBbpContainerRecord(std::uint8_t plp_id, std::uint8_t flags, ByteView bbp) {
    std::vector<std::uint8_t> r;
    r.reserve(kBbpContainerPrefix + bbp.size());
    r.push_back(plp_id);
    r.push_back(flags);
    r.push_back(0);
    r.push_back(0);
    r.insert(r.end(), bbp.begin(), bbp.end());
    return r;
}

}  // namespace atsc3::pcap
