#include "datamanager/json_result_cache.hpp"

#include <Windows.h>

namespace sf::datamanager {
namespace {

[[nodiscard]] std::uint64_t SystemTick() noexcept {
    return static_cast<std::uint64_t>(::GetTickCount64());
}

}

JsonResultCache::JsonResultCache(TickFn tick) noexcept
    : tick_(tick != nullptr ? tick : &SystemTick) {}

bool JsonResultCache::TryCopyFresh(std::string& json) noexcept {
    std::scoped_lock lock{mutex_};
    if (!stored_ || tick_() - storedTick_ >= kFreshnessMilliseconds) {
        return false;
    }
    try {
        json = json_;
    } catch (...) {
        json.clear();
        return false;
    }
    return true;
}

void JsonResultCache::Store(const std::string& json) noexcept {
    std::scoped_lock lock{mutex_};
    try {
        json_ = json;
    } catch (...) {
        json_.clear();
        stored_ = false;
        return;
    }
    storedTick_ = tick_();
    stored_ = true;
}

void JsonResultCache::Clear() noexcept {
    std::scoped_lock lock{mutex_};
    json_.clear();
    stored_ = false;
}

}
