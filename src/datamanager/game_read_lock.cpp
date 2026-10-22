#include "datamanager/game_read_lock.hpp"

#include "platform/native_access.hpp"

#include <Windows.h>

namespace sf::datamanager {
namespace {

using TryAcquireReadLock = bool(__fastcall*)(void* lock);
using ReleaseReadLock = void(__fastcall*)(void* lock);

}

bool TryAcquireGameReadLock(
    std::uintptr_t tryAcquireReadLock,
    std::uintptr_t lock,
    bool& acquired) noexcept {
    __try {
        acquired = reinterpret_cast<TryAcquireReadLock>(tryAcquireReadLock)(
            reinterpret_cast<void*>(lock));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        acquired = false;
        return false;
    }
    return true;
}

GameReadLockGuard::~GameReadLockGuard() {
    reinterpret_cast<ReleaseReadLock>(release_)(reinterpret_cast<void*>(lock_));
}

}
