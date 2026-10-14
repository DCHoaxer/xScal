#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>

namespace sf::datamanager {

struct ResolvedNativeTimeDataRuntime final {
    config::NativeTimeDataProfile layout{};
    std::uintptr_t calendarSingletonGlobal{};
    std::uintptr_t tesGlobalVtable{};
};

struct NativeTimeData final {
    std::uint32_t hour{};
    std::uint32_t minute{};
};

[[nodiscard]] bool ResolveNativeTimeDataRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeTimeDataProfile& profile,
    ResolvedNativeTimeDataRuntime& runtime) noexcept;

[[nodiscard]] bool ReadNativeTimeData(
    const ResolvedNativeTimeDataRuntime& runtime,
    NativeTimeData& timeData) noexcept;

[[nodiscard]] bool BuildNativeTimeDataJson(
    const ResolvedNativeTimeDataRuntime& runtime,
    std::string& json) noexcept;

}
