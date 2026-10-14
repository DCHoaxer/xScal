#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>

namespace sf::datamanager {

struct ResolvedNativeActiveEffectRuntime final {
    config::NativeActiveEffectProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t localPlayerMagicTargetVtable{};
    std::uintptr_t getActiveEffects{};
    std::uintptr_t releaseActiveEffectsReadLock{};
    std::uintptr_t isEligible{};
    std::uintptr_t isDisplayable{};
    std::uintptr_t getMagnitudeDisplayInfo{};
    std::uintptr_t formatEffectText{};
    std::uintptr_t deallocate{};
};

[[nodiscard]] bool ResolveNativeActiveEffectRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActiveEffectProfile& profile,
    ResolvedNativeActiveEffectRuntime& runtime) noexcept;

[[nodiscard]] bool ReadNativeActiveEffectSourcePlayer(
    const ResolvedNativeActiveEffectRuntime& runtime,
    std::uintptr_t& sourcePlayer) noexcept;

// Builds {"EffectsA":[...]} from the local player's active effects.
[[nodiscard]] bool BuildNativeActiveEffectJsonSnapshot(
    platform::VirtualQueryFn virtualQuery,
    const ResolvedNativeActiveEffectRuntime& runtime,
    std::string& json,
    bool* lockBusy = nullptr,
    std::uintptr_t* sourcePlayer = nullptr) noexcept;

}
