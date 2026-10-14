#pragma once
#include <string>

namespace atsc3::test {

/// Absolute path of a capture in the corpus (ATSC3_CORPUS_DIR), e.g. corpus("alp-sample1.pcap").
std::string corpus(const std::string& relative);
/// Absolute path under the schema directory.
std::string schema(const std::string& relative);
/// True when the file exists; tests that need the corpus SKIP otherwise.
bool exists(const std::string& path);

}  // namespace atsc3::test
