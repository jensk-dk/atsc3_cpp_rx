#include "atsc3/route/efdt.hpp"

#include <cstdio>

#include "xml/xml.hpp"

namespace atsc3::route {

using namespace atsc3::xml;

const FdtFile* Efdt::find(std::uint32_t toi) const {
    for (const FdtFile& f : files) {
        if (f.toi == toi) return &f;
    }
    return nullptr;
}

std::optional<std::string> Efdt::locationFor(std::uint32_t toi) const {
    if (const FdtFile* f = find(toi)) return f->content_location;
    if (file_template) return expandFileTemplate(*file_template, toi);
    return std::nullopt;
}

std::optional<std::string> expandFileTemplate(const std::string& t, std::uint32_t toi) {
    std::string out;
    std::size_t i = 0;
    bool any = false;
    while (i < t.size()) {
        if (t[i] != '$') {
            out.push_back(t[i++]);
            continue;
        }
        if (i + 1 < t.size() && t[i + 1] == '$') {
            out.push_back('$');
            i += 2;
            continue;
        }
        const std::size_t end = t.find('$', i + 1);
        if (end == std::string::npos) return std::nullopt;
        const std::string ident = t.substr(i + 1, end - i - 1);
        if (ident.rfind("TOI", 0) != 0) return std::nullopt;
        const std::string fmt = ident.substr(3);  // "" or "%05d"
        char buf[32];
        if (fmt.empty()) {
            std::snprintf(buf, sizeof buf, "%u", toi);
        } else {
            if (fmt.size() < 3 || fmt[0] != '%' || fmt[1] != '0' || fmt.back() != 'd') return std::nullopt;
            int width = 0;
            for (std::size_t k = 2; k + 1 < fmt.size(); ++k) {
                if (fmt[k] < '0' || fmt[k] > '9') return std::nullopt;
                width = width * 10 + (fmt[k] - '0');
            }
            if (width > 20) return std::nullopt;
            std::snprintf(buf, sizeof buf, "%0*u", width, toi);
        }
        out += buf;
        any = true;
        i = end + 1;
    }
    (void)any;
    return out;
}

bool parseEfdt(const std::string& text, Efdt& out, std::string* error) {
    out = Efdt{};
    pugi::xml_document doc;
    if (!load(doc, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error)) return false;
    pugi::xml_node fdt = root(doc, "FDT-Instance");
    if (!fdt) {
        const pugi::xml_node wrapper = root(doc, "EFDT");
        if (wrapper) fdt = child(wrapper, "FDT-Instance");
    }
    if (!fdt) {
        if (error) *error = "no FDT-Instance element";
        return false;
    }
    if (const auto v = attrUInt(fdt, "efdtVersion")) out.efdt_version = static_cast<std::uint8_t>(*v);
    if (const auto v = attrInt(fdt, "Expires"); v && *v >= 0) out.expires = static_cast<std::uint64_t>(*v);
    out.max_expires_delta = attrUInt(fdt, "maxExpiresDelta");
    out.max_transport_size = attrUInt(fdt, "maxTransportSize");
    out.max_cache_memory = attrUInt(fdt, "maxCacheMemory");
    out.file_template = attr(fdt, "fileTemplate");
    if (const auto l = attr(fdt, "appContextIdList")) out.app_context_id_list = splitList(*l);
    if (const auto l = attr(fdt, "filterCodes")) out.filter_codes = splitUIntList(*l);
    out.order = attrBool(fdt, "order", true);
    out.default_content_type = attr(fdt, "Content-Type");
    out.default_content_encoding = attr(fdt, "Content-Encoding");
    for (const pugi::xml_node& f : children(fdt, "File")) {
        FdtFile file;
        const auto toi = attrUInt(f, "TOI");
        if (!toi) continue;
        file.toi = *toi;
        file.content_location = attrOr(f, "Content-Location", "");
        file.content_type = attr(f, "Content-Type");
        if (!file.content_type) file.content_type = out.default_content_type;
        file.content_encoding = attr(f, "Content-Encoding");
        if (!file.content_encoding) file.content_encoding = out.default_content_encoding;
        file.content_md5 = attr(f, "Content-MD5");
        if (const auto v = attrInt(f, "Content-Length"); v && *v >= 0) file.content_length = static_cast<std::uint64_t>(*v);
        if (const auto v = attrInt(f, "Transfer-Length"); v && *v >= 0) file.transfer_length = static_cast<std::uint64_t>(*v);
        if (const auto l = attr(f, "appContextIdList")) file.app_context_id_list = splitList(*l);
        if (const auto l = attr(f, "filterCodes")) file.filter_codes = splitUIntList(*l);
        file.raw_xml = serialize(f);
        out.files.push_back(std::move(file));
    }
    out.raw_xml = serialize(fdt);
    return true;
}

}  // namespace atsc3::route
