#include "atsc3/tuners/pcap/pcap_reader.hpp"

#include <cstring>

namespace atsc3::pcap {

namespace {
constexpr std::uint32_t kMagicUs = 0xA1B2C3D4;
constexpr std::uint32_t kMagicNs = 0xA1B23C4D;
constexpr std::uint32_t kMagicUsSwapped = 0xD4C3B2A1;
constexpr std::uint32_t kMagicNsSwapped = 0x4D3CB2A1;
constexpr std::uint32_t kPcapngShb = 0x0A0D0D0A;
constexpr std::uint32_t kByteOrderMagic = 0x1A2B3C4D;

std::uint32_t bswap32(std::uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

std::uint32_t rd32(const std::uint8_t* p, bool swap) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return swap ? bswap32(v) : v;
}
std::uint16_t rd16(const std::uint8_t* p, bool swap) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return swap ? bswap16(v) : v;
}
}  // namespace

std::unique_ptr<PcapReader> PcapReader::open(const std::string& path, std::string* error) {
    std::unique_ptr<PcapReader> r(new PcapReader());
    r->path_ = path;
    r->f_ = std::fopen(path.c_str(), "rb");
    if (!r->f_) {
        if (error) *error = "cannot open " + path;
        return nullptr;
    }
    if (!r->readHeader(error)) return nullptr;
    return r;
}

PcapReader::~PcapReader() {
    if (f_) std::fclose(f_);
}

bool PcapReader::readExact(void* buf, std::size_t n) {
    return std::fread(buf, 1, n, f_) == n;
}

bool PcapReader::readHeader(std::string* error) {
    std::uint8_t h[24];
    if (!readExact(h, 4)) {
        if (error) *error = "file too short";
        return false;
    }
    std::uint32_t magic;
    std::memcpy(&magic, h, 4);
    if (magic == kPcapngShb) {
        pcapng_ = true;
        // SHB: block total length(4), byte-order magic(4), version(2+2), section length(8), options, total length(4)
        std::uint8_t rest[8];
        if (!readExact(rest, 8)) return false;
        std::uint32_t bom;
        std::memcpy(&bom, rest + 4, 4);
        if (bom == kByteOrderMagic) swap_ = false;
        else if (bswap32(bom) == kByteOrderMagic) swap_ = true;
        else {
            if (error) *error = "bad pcapng byte-order magic";
            return false;
        }
        const std::uint32_t total = rd32(rest, swap_);
        if (total < 28) return false;
        std::fseek(f_, static_cast<long>(total) - 12, SEEK_CUR);
        return true;
    }
    if (magic == kMagicUs || magic == kMagicNs) swap_ = false;
    else if (magic == kMagicUsSwapped || magic == kMagicNsSwapped) swap_ = true;
    else {
        if (error) *error = "not a pcap/pcapng file";
        return false;
    }
    nanos_ = (magic == kMagicNs || magic == kMagicNsSwapped);
    if (!readExact(h + 4, 20)) {
        if (error) *error = "truncated pcap header";
        return false;
    }
    snaplen_ = rd32(h + 16, swap_);
    classic_link_ = static_cast<std::uint16_t>(rd32(h + 20, swap_) & 0xFFFF);
    if_link_.assign(1, classic_link_);
    return true;
}

void PcapReader::rewind() {
    std::fseek(f_, 0, SEEK_SET);
    truncated_ = false;
    index_ = 0;
    if_link_.clear();
    if_tsres_ns_.clear();
    readHeader(nullptr);
}

bool PcapReader::next(PcapRecord& rec) {
    if (!f_ || truncated_) return false;
    const bool ok = pcapng_ ? nextNg(rec) : nextClassic(rec);
    if (ok) ++index_;
    return ok;
}

bool PcapReader::nextClassic(PcapRecord& rec) {
    std::uint8_t h[16];
    const std::size_t got = std::fread(h, 1, 16, f_);
    if (got == 0) return false;
    if (got < 16) {
        truncated_ = true;
        return false;
    }
    const std::uint32_t sec = rd32(h, swap_);
    const std::uint32_t frac = rd32(h + 4, swap_);
    const std::uint32_t incl = rd32(h + 8, swap_);
    rec.orig_len = rd32(h + 12, swap_);
    rec.interface_id = 0;
    rec.link_type = classic_link_;
    rec.ts_ns = static_cast<std::int64_t>(sec) * 1'000'000'000LL +
                static_cast<std::int64_t>(frac) * (nanos_ ? 1LL : 1000LL);
    if (incl > 0x7FFFFFFF) {
        truncated_ = true;
        return false;
    }
    rec.data.resize(incl);
    if (incl && !readExact(rec.data.data(), incl)) {
        truncated_ = true;
        return false;
    }
    return true;
}

bool PcapReader::nextNg(PcapRecord& rec) {
    for (;;) {
        std::uint8_t bh[8];
        const std::size_t got = std::fread(bh, 1, 8, f_);
        if (got == 0) return false;
        if (got < 8) {
            truncated_ = true;
            return false;
        }
        const std::uint32_t type = rd32(bh, swap_);
        const std::uint32_t total = rd32(bh + 4, swap_);
        if (total < 12 || (total & 3) != 0) {
            truncated_ = true;
            return false;
        }
        std::vector<std::uint8_t> body(total - 12);
        if (!body.empty() && !readExact(body.data(), body.size())) {
            truncated_ = true;
            return false;
        }
        std::uint8_t trailer[4];
        if (!readExact(trailer, 4)) {
            truncated_ = true;
            return false;
        }
        switch (type) {
            case kPcapngShb: {
                // new section: reset interfaces
                if (body.size() < 16) {
                    truncated_ = true;
                    return false;
                }
                if_link_.clear();
                if_tsres_ns_.clear();
                break;
            }
            case 1: {  // IDB
                if (body.size() < 8) break;
                const std::uint16_t lt = rd16(body.data(), swap_);
                std::int64_t tick_ns = 1000;  // default 10^-6
                std::size_t o = 8;
                while (o + 4 <= body.size()) {
                    const std::uint16_t code = rd16(body.data() + o, swap_);
                    const std::uint16_t len = rd16(body.data() + o + 2, swap_);
                    o += 4;
                    if (code == 0) break;
                    if (code == 9 && len >= 1 && o < body.size()) {
                        const std::uint8_t v = body[o];
                        if (v & 0x80) {
                            tick_ns = 1'000'000'000LL;
                            for (int i = 0; i < (v & 0x7F); ++i) tick_ns /= 2;
                        } else {
                            tick_ns = 1'000'000'000LL;
                            for (int i = 0; i < v; ++i) tick_ns /= 10;
                        }
                        if (tick_ns < 1) tick_ns = 1;
                    }
                    o += (static_cast<std::size_t>(len) + 3) & ~static_cast<std::size_t>(3);
                }
                if_link_.push_back(lt);
                if_tsres_ns_.push_back(tick_ns);
                break;
            }
            case 6: {  // EPB
                if (body.size() < 20) break;
                rec.interface_id = rd32(body.data(), swap_);
                const std::uint64_t ts = (static_cast<std::uint64_t>(rd32(body.data() + 4, swap_)) << 32) |
                                         rd32(body.data() + 8, swap_);
                const std::uint32_t cap = rd32(body.data() + 12, swap_);
                rec.orig_len = rd32(body.data() + 16, swap_);
                if (cap > body.size() - 20) {
                    truncated_ = true;
                    return false;
                }
                const std::size_t ifi = rec.interface_id;
                rec.link_type = ifi < if_link_.size() ? if_link_[ifi] : 0;
                const std::int64_t tick = ifi < if_tsres_ns_.size() ? if_tsres_ns_[ifi] : 1000;
                rec.ts_ns = static_cast<std::int64_t>(ts) * tick;
                rec.data.assign(body.begin() + 20, body.begin() + 20 + cap);
                return true;
            }
            case 3: {  // SPB
                if (body.size() < 4) break;
                rec.interface_id = 0;
                rec.orig_len = rd32(body.data(), swap_);
                std::size_t cap = rec.orig_len;
                if (cap > body.size() - 4) cap = body.size() - 4;
                rec.link_type = if_link_.empty() ? 0 : if_link_[0];
                rec.ts_ns = 0;
                rec.data.assign(body.begin() + 4, body.begin() + 4 + static_cast<std::ptrdiff_t>(cap));
                return true;
            }
            default:
                break;  // skip NRB, ISB, custom blocks
        }
    }
}

// ---------------- writer ----------------

std::unique_ptr<PcapWriter> PcapWriter::create(const std::string& path, std::uint16_t link_type, std::string* error) {
    std::unique_ptr<PcapWriter> w(new PcapWriter());
    w->f_ = std::fopen(path.c_str(), "wb");
    if (!w->f_) {
        if (error) *error = "cannot create " + path;
        return nullptr;
    }
    std::uint8_t h[24] = {};
    const std::uint32_t magic = kMagicNs;
    std::memcpy(h, &magic, 4);
    const std::uint16_t vmaj = 2, vmin = 4;
    std::memcpy(h + 4, &vmaj, 2);
    std::memcpy(h + 6, &vmin, 2);
    const std::uint32_t snap = 262144, lt = link_type;
    std::memcpy(h + 16, &snap, 4);
    std::memcpy(h + 20, &lt, 4);
    if (std::fwrite(h, 1, 24, w->f_) != 24) return nullptr;
    return w;
}

PcapWriter::~PcapWriter() {
    if (f_) std::fclose(f_);
}

bool PcapWriter::write(std::int64_t ts_ns, const std::uint8_t* data, std::size_t len) {
    std::uint8_t h[16];
    const std::uint32_t sec = static_cast<std::uint32_t>(ts_ns / 1'000'000'000LL);
    const std::uint32_t ns = static_cast<std::uint32_t>(ts_ns % 1'000'000'000LL);
    const std::uint32_t l = static_cast<std::uint32_t>(len);
    std::memcpy(h, &sec, 4);
    std::memcpy(h + 4, &ns, 4);
    std::memcpy(h + 8, &l, 4);
    std::memcpy(h + 12, &l, 4);
    return std::fwrite(h, 1, 16, f_) == 16 && (len == 0 || std::fwrite(data, 1, len, f_) == len);
}

}  // namespace atsc3::pcap
