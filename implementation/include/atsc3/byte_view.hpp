#pragma once
#include <cstddef>
#include <cstdint>

namespace atsc3 {

/// Non-owning view over bytes (C++17 stand-in for std::span<const std::uint8_t>).
class ByteView {
public:
    constexpr ByteView() noexcept = default;
    constexpr ByteView(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    constexpr const std::uint8_t* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr std::uint8_t operator[](std::size_t i) const noexcept { return data_[i]; }
    constexpr const std::uint8_t* begin() const noexcept { return data_; }
    constexpr const std::uint8_t* end() const noexcept { return data_ + size_; }

    constexpr ByteView subview(std::size_t offset, std::size_t count) const noexcept {
        if (offset > size_) return {};
        const std::size_t n = (count > size_ - offset) ? size_ - offset : count;
        return {data_ + offset, n};
    }
    constexpr ByteView subview(std::size_t offset) const noexcept { return subview(offset, size_); }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace atsc3
