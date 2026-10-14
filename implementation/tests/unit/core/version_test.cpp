#include <catch2/catch_test_macros.hpp>

#include "atsc3/byte_view.hpp"
#include "atsc3/version.hpp"

TEST_CASE("version is reported consistently", "[core]") {
    const auto v = atsc3::version();
    REQUIRE(atsc3::versionString() ==
            std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch));
}

TEST_CASE("ByteView subview is bounds-safe", "[core][util]") {
    const std::uint8_t bytes[4] = {1, 2, 3, 4};
    atsc3::ByteView view(bytes, 4);
    REQUIRE(view.subview(2).size() == 2);
    REQUIRE(view.subview(2)[0] == 3);
    REQUIRE(view.subview(4).empty());
    REQUIRE(view.subview(9).empty());
    REQUIRE(view.subview(1, 100).size() == 3);
}
