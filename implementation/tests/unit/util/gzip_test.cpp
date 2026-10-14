#include <catch2/catch_test_macros.hpp>

#include "atsc3/util/gzip.hpp"
#include "packets.hpp"

using namespace atsc3;
using namespace atsc3::test;

TEST_CASE("gzip round trip, raw deflate fallback and limits", "[util][gzip]") {
    Bytes text(5000);
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<std::uint8_t>('A' + (i % 26));
    const Bytes gz = util::gzipCompress(ByteView(text.data(), text.size()));
    REQUIRE(gz.size() > 10);
    CHECK(util::isGzip(ByteView(gz.data(), gz.size())));
    const auto back = util::gunzip(ByteView(gz.data(), gz.size()));
    REQUIRE(back);
    CHECK(*back == text);
    // raw deflate = gzip body without the 10-byte header and 8-byte trailer
    const Bytes raw(gz.begin() + 10, gz.end() - 8);
    const auto back2 = util::gunzip(ByteView(raw.data(), raw.size()));
    REQUIRE(back2);
    CHECK(*back2 == text);
    CHECK_FALSE(util::gunzip(ByteView(gz.data(), gz.size()), 100));       // output limit
    CHECK_FALSE(util::gunzip(ByteView(gz.data(), gz.size() / 2)));         // truncated
    const Bytes junk{1, 2, 3, 4, 5};
    CHECK_FALSE(util::gunzip(ByteView(junk.data(), junk.size())));
}
