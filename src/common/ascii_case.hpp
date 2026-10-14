#pragma once

#include <cstddef>
#include <string_view>

namespace sf {

template <typename Char>
[[nodiscard]] constexpr Char AsciiLower(Char value) noexcept {
    return value >= static_cast<Char>('A') && value <= static_cast<Char>('Z')
        ? static_cast<Char>(value + ('a' - 'A'))
        : value;
}

// Compares only ASCII letters case-insensitively. Other characters must match
// exactly.
template <typename Char>
[[nodiscard]] constexpr bool EqualsAsciiIgnoreCase(
    std::basic_string_view<Char> left,
    std::basic_string_view<Char> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) {
            return false;
        }
    }
    return true;
}

}
