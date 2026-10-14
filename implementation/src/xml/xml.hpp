#pragma once
// Namespace-tolerant XML helpers on pugixml (spec 5.16 parsing policy): local-name matching,
// schema defaults, xs:list / xs:dateTime / xs:duration parsing, raw subtree capture.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

#include "atsc3/byte_view.hpp"

namespace atsc3::xml {

/// Loads XML, tolerating a UTF-8 BOM, CRLF and leading whitespace. `error` receives a message.
bool load(pugi::xml_document& doc, ByteView bytes, std::string* error = nullptr);

/// Local name without namespace prefix ("slt:SLT" -> "SLT").
std::string_view localName(const char* qualified) noexcept;
bool nameIs(const pugi::xml_node& n, std::string_view local) noexcept;

/// First child with the local name (any prefix/namespace).
pugi::xml_node child(const pugi::xml_node& n, std::string_view local);
std::vector<pugi::xml_node> children(const pugi::xml_node& n, std::string_view local);
/// Root element regardless of namespace; empty node when the root's local name differs.
pugi::xml_node root(const pugi::xml_document& doc, std::string_view local);

/// Attribute lookup by local name (also matches "xml:lang" when asked for "lang" with xml_prefix=true).
std::optional<std::string> attr(const pugi::xml_node& n, std::string_view local);
std::string attrOr(const pugi::xml_node& n, std::string_view local, const std::string& def);
bool attrBool(const pugi::xml_node& n, std::string_view local, bool def);
std::optional<std::uint32_t> attrUInt(const pugi::xml_node& n, std::string_view local);
std::uint32_t attrUIntOr(const pugi::xml_node& n, std::string_view local, std::uint32_t def);
std::optional<std::int64_t> attrInt(const pugi::xml_node& n, std::string_view local);
std::optional<double> attrDouble(const pugi::xml_node& n, std::string_view local);
/// xml:lang or lang (on-air streams use both).
std::optional<std::string> lang(const pugi::xml_node& n);

/// Whitespace-separated list.
std::vector<std::string> splitList(std::string_view text);
std::vector<std::uint32_t> splitUIntList(std::string_view text);

/// xs:dateTime -> Unix time in nanoseconds (fractional seconds and zone offsets honoured).
std::optional<std::int64_t> parseDateTimeNs(std::string_view text);
/// xs:duration (days/hours/minutes/seconds only) -> seconds; years/months are rejected.
std::optional<double> parseDurationSeconds(std::string_view text);

/// Serialises a node subtree (for raw_xml fields).
std::string serialize(const pugi::xml_node& n);
/// The namespace URI declared for the element (xmlns or its prefix), empty when none.
std::string namespaceOf(const pugi::xml_node& n);

}  // namespace atsc3::xml
