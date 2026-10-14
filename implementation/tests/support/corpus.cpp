#include "corpus.hpp"

#include <sys/stat.h>

namespace atsc3::test {

std::string corpus(const std::string& relative) { return std::string(ATSC3_CORPUS_DIR) + "/" + relative; }
std::string schema(const std::string& relative) { return std::string(ATSC3_SCHEMA_DIR) + "/" + relative; }
bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace atsc3::test
