#pragma once
#include <cstdint>
#include <string_view>

namespace atsc3 {

struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

/// Library version as compiled.
Version version() noexcept;

/// "major.minor.patch" string.
std::string_view versionString() noexcept;

}  // namespace atsc3
