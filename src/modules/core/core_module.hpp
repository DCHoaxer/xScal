#pragma once

#include <cstdint>

namespace sf {

struct MovieRootContext;

namespace modules {

// One instance belongs to each cached MovieRoot on the calling thread.
class CoreModule final {
public:
    void Update(const MovieRootContext& context) noexcept;

private:
    [[nodiscard]] static bool AddVersionText(
        const MovieRootContext& context) noexcept;

    std::uint8_t versionTextAttempts_{};
    bool versionTextAdded_{};
};

}  // namespace modules
}  // namespace sf
