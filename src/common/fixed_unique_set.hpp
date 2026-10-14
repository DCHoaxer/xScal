#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace sf {

template <typename T, std::size_t Capacity>
class FixedUniqueSet final {
    static_assert(Capacity != 0);

public:
    [[nodiscard]] bool Contains(const T& value) const noexcept {
        return std::find(begin(), end(), value) != end();
    }

    // Inserts in encounter order. When full, evicts the oldest value.
    [[nodiscard]] bool Insert(
        const T& value,
        T* evicted = nullptr) noexcept {
        if (Contains(value)) {
            return false;
        }
        if (size_ == Capacity) {
            if (evicted != nullptr) {
                *evicted = values_.front();
            }
            std::move(values_.begin() + 1, values_.end(), values_.begin());
            values_.back() = value;
        } else {
            values_[size_++] = value;
        }
        return true;
    }

    [[nodiscard]] bool Erase(const T& value) noexcept {
        const auto found = std::find(begin(), end(), value);
        if (found == end()) {
            return false;
        }
        std::move(found + 1, end(), found);
        values_[--size_] = {};
        return true;
    }

    template <typename Predicate>
    void EraseIf(Predicate predicate) noexcept {
        auto destination = values_.begin();
        for (auto source = values_.begin(); source != end(); ++source) {
            if (!predicate(*source)) {
                *destination++ = std::move(*source);
            }
        }
        while (values_.begin() + size_ != destination) {
            values_[--size_] = {};
        }
    }

    [[nodiscard]] auto begin() noexcept { return values_.begin(); }
    [[nodiscard]] auto begin() const noexcept { return values_.begin(); }
    [[nodiscard]] auto end() noexcept { return values_.begin() + size_; }
    [[nodiscard]] auto end() const noexcept { return values_.begin() + size_; }

private:
    std::array<T, Capacity> values_{};
    std::size_t size_{};
};

}
