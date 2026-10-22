#pragma once

#include <cstdint>

namespace sf::datamanager {

// Calls the game's BSReadWriteLock try-read function. Returns false when the
// call faults; acquired is false when a writer holds the lock.
[[nodiscard]] bool TryAcquireGameReadLock(
    std::uintptr_t tryAcquireReadLock,
    std::uintptr_t lock,
    bool& acquired) noexcept;

// Releases a BSReadWriteLock read hold when it goes out of scope.
class GameReadLockGuard final {
public:
    GameReadLockGuard(std::uintptr_t releaseReadLock, std::uintptr_t lock) noexcept
        : release_(releaseReadLock), lock_(lock) {}

    GameReadLockGuard(const GameReadLockGuard&) = delete;
    GameReadLockGuard& operator=(const GameReadLockGuard&) = delete;

    ~GameReadLockGuard();

private:
    std::uintptr_t release_{};
    std::uintptr_t lock_{};
};

}
