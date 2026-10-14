#include "atsc3/tuners/pcap/sidecar_xa3.hpp"

#include <sys/stat.h>
#include <dirent.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <pugixml.hpp>

#include "atsc3/io/inet.hpp"

namespace atsc3::pcap {

namespace {
bool fileExists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
std::string dirOf(const std::string& p) {
    const auto pos = p.find_last_of('/');
    return pos == std::string::npos ? "." : (pos == 0 ? "/" : p.substr(0, pos));
}
bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char a = s[s.size() - suffix.size() + i];
        const char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}
std::uint32_t ipFromParts(const pugi::xml_node& n) {
    std::uint32_t v = 0;
    const char* names[4] = {"ip0", "ip1", "ip2", "ip3"};
    for (const char* name : names) v = (v << 8) | (n.attribute(name).as_uint(0) & 0xFFu);
    return v;
}
}  // namespace

std::string pathBasename(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::optional<std::uint8_t> SidecarXa3::plpForCapture(const std::string& capture_path) const {
    const std::string base = pathBasename(capture_path);
    for (const auto& [name, plp] : plp_sources) {
        if (name == base) return plp;
    }
    if (!l1.plps.empty()) return l1.plps.front().plp_id;
    return std::nullopt;
}

std::optional<SidecarXa3> parseXa3(const std::string& xml, const std::string& path, std::string* error) {
    pugi::xml_document doc;
    const pugi::xml_parse_result res = doc.load_buffer(xml.data(), xml.size());
    if (!res) {
        if (error) *error = std::string("xa3 parse error: ") + res.description();
        return std::nullopt;
    }
    const pugi::xml_node root = doc.child("ModulationParameters");
    const pugi::xml_node atsc = root.child("Atsc3");
    if (!atsc) {
        if (error) *error = "xa3: no ModulationParameters/Atsc3 element";
        return std::nullopt;
    }
    SidecarXa3 sc;
    sc.path = path;
    sc.l1.source = path;
    sc.l1.lmt_origin = io::LmtOrigin::Sidecar;
    const pugi::xml_node general = atsc.child("General");
    sc.l1.lls_flag = general.attribute("lls_flag").as_bool(false);
    sc.l1.bandwidth = general.attribute("bandwidth").as_string();
    sc.l1.minor_version = general.attribute("minor_version").as_int(-1);
    const pugi::xml_node pre = atsc.child("Preamble");
    sc.l1.fft_size = pre.attribute("fft_size").as_string();
    sc.l1.guard_interval = pre.attribute("guard_interval").as_string();
    for (pugi::xml_node sub : atsc.children("Subframe")) {
        if (sc.l1.pilot_pattern.empty()) sc.l1.pilot_pattern = sub.attribute("pilot_pattern").as_string();
        for (pugi::xml_node plp : sub.children("Plp")) {
            io::L1Info::Plp p;
            p.plp_id = static_cast<std::uint8_t>(plp.attribute("plp_id").as_uint(0) & 0x3F);
            p.lls_flag = plp.attribute("lls_flag").as_bool(false);
            p.modulation = plp.attribute("modulation").as_string();
            p.code_rate = plp.attribute("code_rate").as_string();
            p.fec_code_length = plp.attribute("fec_code_length").as_string();
            p.fec_outer_code = plp.attribute("fec_outer_code").as_string();
            p.layer = plp.attribute("layer").as_string();
            p.plp_type = plp.attribute("plp_type").as_string();
            sc.l1.plps.push_back(p);
            for (pugi::xml_node src : plp.child("Source").children("PcapFile")) {
                const std::string fn = src.attribute("filename").as_string();
                if (!fn.empty()) sc.plp_sources.emplace_back(pathBasename(fn), p.plp_id);
            }
        }
    }
    for (pugi::xml_node lplp : atsc.child("Signaling").child("Lmt").children("Plp")) {
        const std::uint8_t plp_id = static_cast<std::uint8_t>(lplp.attribute("plp_id").as_uint(0) & 0x3F);
        for (pugi::xml_node mc : lplp.children("Multicast")) {
            io::LmtEntry e;
            e.plp_id = plp_id;
            const pugi::xml_node s = mc.child("Src");
            const pugi::xml_node d = mc.child("Dst");
            e.src_ip = ipFromParts(s);
            e.src_port = static_cast<std::uint16_t>(s.attribute("port").as_uint(0) & 0xFFFFu);
            e.dst_ip = ipFromParts(d);
            e.dst_port = static_cast<std::uint16_t>(d.attribute("port").as_uint(0) & 0xFFFFu);
            if (e.dst_ip != 0) sc.l1.lmt.push_back(e);
        }
    }
    return sc;
}

std::optional<SidecarXa3> loadXa3(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseXa3(ss.str(), path, error);
}

std::optional<std::string> findXa3Sidecar(const std::string& capture_path) {
    if (fileExists(capture_path + ".xa3")) return capture_path + ".xa3";
    const auto dot = capture_path.find_last_of('.');
    const auto slash = capture_path.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string stem = capture_path.substr(0, dot) + ".xa3";
        if (fileExists(stem)) return stem;
    }
    const std::string dir = dirOf(capture_path);
    const std::string base = pathBasename(capture_path);
    DIR* d = ::opendir(dir.c_str());
    if (!d) return std::nullopt;
    std::optional<std::string> found;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (!endsWithNoCase(name, ".xa3") || name.front() == '#') continue;
        const std::string full = dir + "/" + name;
        const auto sc = loadXa3(full);
        if (!sc) continue;
        for (const auto& [src, plp] : sc->plp_sources) {
            if (src == base) {
                found = full;
                break;
            }
        }
        if (found) break;
    }
    ::closedir(d);
    return found;
}

}  // namespace atsc3::pcap
