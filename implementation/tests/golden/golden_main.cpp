#include <catch2/catch_test_macros.hpp>

#include "corpus.hpp"

// Expected values live in expectations/<capture>.json (spec Section 9 baselines).
TEST_CASE("corpus is reachable", "[golden]") {
    if (!atsc3::test::exists(atsc3::test::corpus("alp-sample1.pcap"))) {
        SKIP("corpus not available");
    }
    SUCCEED();
}
