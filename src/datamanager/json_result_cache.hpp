#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace sf::datamanager {

// The result of a Game.* provider request.
enum class GameProviderRequestStatus {
    Delivered,
    SnapshotUnavailable,
    Inactive,
};

// Holds the last JSON a provider delivered. A mod that polls every frame gets
// the stored copy for kFreshnessMilliseconds instead of a new game read.
class JsonResultCache final {
public:
    using TickFn = std::uint64_t(*)() noexcept;

    static constexpr std::uint64_t kFreshnessMilliseconds = 50;

    // A null tick source uses GetTickCount64.
    explicit JsonResultCache(TickFn tick = nullptr) noexcept;

    JsonResultCache(const JsonResultCache&) = delete;
    JsonResultCache& operator=(const JsonResultCache&) = delete;

    // Copies the stored JSON when it was stored less than
    // kFreshnessMilliseconds ago.
    [[nodiscard]] bool TryCopyFresh(std::string& json) noexcept;
    void Store(const std::string& json) noexcept;
    void Clear() noexcept;

private:
    TickFn tick_{};
    std::mutex mutex_;
    std::string json_;
    std::uint64_t storedTick_{};
    bool stored_{};
};

}
