#include "atsc3/version.hpp"

namespace atsc3 {

Version version() noexcept {
    return Version{ATSC3_VERSION_MAJOR, ATSC3_VERSION_MINOR, ATSC3_VERSION_PATCH};
}

std::string_view versionString() noexcept {
    return ATSC3_VERSION_STRING;
}

}  // namespace atsc3
