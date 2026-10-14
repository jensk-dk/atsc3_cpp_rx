#pragma once
// Extended FDT (RFC 6726 FDT-Instance + A/331 A.3.3.2 ATSC attributes; spec 5.8.5).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atsc3::route {

struct FdtFile {
    std::uint32_t toi = 0;
    std::string content_location;
    std::optional<std::string> content_type, content_encoding, content_md5;
    std::optional<std::uint64_t> content_length, transfer_length;
    std::vector<std::string> app_context_id_list;
    std::vector<std::uint32_t> filter_codes;
    std::string raw_xml;
};

struct Efdt {
    std::optional<std::uint8_t> efdt_version;
    std::optional<std::uint64_t> expires;              // NTP seconds as transmitted
    std::optional<std::uint32_t> max_expires_delta, max_transport_size, max_cache_memory;
    std::optional<std::string> file_template;          // $TOI$, $TOI%05d$, $$
    std::vector<std::string> app_context_id_list;
    std::vector<std::uint32_t> filter_codes;
    bool order = true;
    std::optional<std::string> default_content_type, default_content_encoding;  // FDT-Instance level
    std::vector<FdtFile> files;
    std::string raw_xml;

    const FdtFile* find(std::uint32_t toi) const;
    /// Content-Location for a TOI: explicit File entry, else the expanded file template.
    std::optional<std::string> locationFor(std::uint32_t toi) const;
};

/// Parses an FDT-Instance document or an S-TSID `EFDT` wrapper around it (both on-air forms).
bool parseEfdt(const std::string& xml, Efdt& out, std::string* error = nullptr);

/// Expands `$TOI$` / `$TOI%0Nd$` / `$$` (A/331 fileTemplate grammar).
std::optional<std::string> expandFileTemplate(const std::string& tmpl, std::uint32_t toi);

}  // namespace atsc3::route
