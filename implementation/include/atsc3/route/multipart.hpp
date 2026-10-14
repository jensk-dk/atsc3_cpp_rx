#pragma once
// MIME multipart/related and multipart/signed parsing (RFC 2387, RFC 1847) for SLS bundles and
// ROUTE package mode (A/331 A.3.3.4); also HTTP entity headers for entity mode (A.3.3.3).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"

namespace atsc3::route {

struct EntityHeaders {
    std::map<std::string, std::string> fields;  // lower-case names, unfolded values
    std::optional<std::string> get(const std::string& lower_name) const;
    /// Media type without parameters, lower-case ("application/route-usd+xml").
    std::string contentType() const;
    std::optional<std::string> contentTypeParam(const std::string& name) const;  // e.g. boundary, charset
    std::optional<std::string> contentLocation() const { return get("content-location"); }
    std::optional<std::string> contentEncoding() const { return get("content-encoding"); }
};

struct MimePart {
    EntityHeaders headers;
    ByteView body;
};

struct Multipart {
    EntityHeaders headers;       // the top-level entity headers (Content-Type: multipart/...)
    std::string boundary;
    std::vector<MimePart> parts;
    bool is_signed = false;      // multipart/signed: parts[0] is the signed content, parts[1] the signature
};

/// Parses an entity header block terminated by an empty line. Returns bytes consumed (0 if not a header block).
std::size_t parseEntityHeaders(ByteView data, EntityHeaders& out);

/// Parses a multipart body given its top-level headers (or a body that starts with them).
bool parseMultipart(ByteView data, Multipart& out, std::string* error = nullptr);

}  // namespace atsc3::route
