#include <cstddef>
#include <cstdint>

// Seed corpus: tests/fuzz/corpus/alp/ (ALP packets cut from the samples).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    (void)data;
    (void)size;
    return 0;
}
