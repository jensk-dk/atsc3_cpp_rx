#pragma once
#include <cstdint>
#include <string>

#include "atsc3/byte_view.hpp"

namespace atsc3::pcap {

/// Strips the link-layer header for IP-bearing link types and returns the IPv4 packet view.
/// Returns false for non-IPv4 frames (IPv6, ARP, ...) or unknown link types.
bool stripToIpv4(std::uint16_t link_type, ByteView frame, ByteView& ipv4) noexcept;

/// True when the frame is an ICMP packet whose payload looks like a textual marker; copies the text.
bool extractMarkerText(ByteView ipv4, std::string& text);

}  // namespace atsc3::pcap
