#include "datamanager/time_data_snapshot.hpp"

#include "common/json.hpp"
#include "platform/native_access.hpp"

#include <cmath>
#include <cstddef>

namespace sf::datamanager {
namespace {

constexpr float kHoursPerDay = 24.0F;
constexpr float kMinutesPerHour = 60.0F;
constexpr std::uint32_t kMinutesPerDay = 24U * 60U;
constexpr std::size_t kMaximumTimeDataJsonBytes = 64;

[[nodiscard]] bool HasCompleteNativeTimeDataProfile(
    const config::NativeTimeDataProfile& profile) noexcept {
    return profile.calendarSingletonGlobal != 0 &&
        profile.tesGlobalVtable != 0;
}

}

bool ResolveNativeTimeDataRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeTimeDataProfile& profile,
    ResolvedNativeTimeDataRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr ||
        !HasCompleteNativeTimeDataProfile(profile)) {
        return false;
    }

    ResolvedNativeTimeDataRuntime resolved;
    resolved.layout = profile;
    if (!platform::ResolveRequiredRvas(
            moduleBase,
            {
                {profile.calendarSingletonGlobal,
                 &resolved.calendarSingletonGlobal},
                {profile.tesGlobalVtable, &resolved.tesGlobalVtable},
            }) ||
        !platform::AreReadableAddresses(
            virtualQuery, {resolved.calendarSingletonGlobal}) ||
        !platform::ValidateVtable(
            virtualQuery, moduleBase, profile.tesGlobalVtable)) {
        return false;
    }

    runtime = resolved;
    return true;
}

bool ReadNativeTimeData(
    const ResolvedNativeTimeDataRuntime& runtime,
    NativeTimeData& timeData) noexcept {
    timeData = {};
    if (runtime.calendarSingletonGlobal == 0 ||
        runtime.tesGlobalVtable == 0) {
        return false;
    }

    std::uintptr_t calendar{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.calendarSingletonGlobal),
            &calendar,
            sizeof(calendar)) ||
        calendar == 0) {
        return false;
    }

    std::uintptr_t gameHourAddress{};
    if (!platform::TryAddAddress(
            calendar,
            runtime.layout.calendarGameHourOffset,
            gameHourAddress)) {
        return false;
    }

    std::uintptr_t gameHour{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(gameHourAddress),
            &gameHour,
            sizeof(gameHour)) ||
        gameHour == 0) {
        return false;
    }

    std::uintptr_t actualVtable{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(gameHour),
            &actualVtable,
            sizeof(actualVtable)) ||
        actualVtable != runtime.tesGlobalVtable) {
        return false;
    }

    std::uintptr_t valueAddress{};
    float gameHourValue{};
    if (!platform::TryAddAddress(
            gameHour,
            runtime.layout.tesGlobalValueOffset,
            valueAddress) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(valueAddress),
            &gameHourValue,
            sizeof(gameHourValue)) ||
        !std::isfinite(gameHourValue) ||
        gameHourValue < 0.0F ||
        gameHourValue > kHoursPerDay) {
        return false;
    }

    std::uintptr_t currentCalendar{};
    std::uintptr_t currentGameHour{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.calendarSingletonGlobal),
            &currentCalendar,
            sizeof(currentCalendar)) ||
        currentCalendar != calendar ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(gameHourAddress),
            &currentGameHour,
            sizeof(currentGameHour)) ||
        currentGameHour != gameHour) {
        return false;
    }

    if (gameHourValue == kHoursPerDay) {
        gameHourValue = 0.0F;
    }
    const auto totalMinutes = static_cast<std::uint32_t>(
        gameHourValue * kMinutesPerHour);
    if (totalMinutes >= kMinutesPerDay) {
        return false;
    }

    timeData.hour = totalMinutes / 60U;
    timeData.minute = totalMinutes % 60U;
    return true;
}

bool BuildNativeTimeDataJson(
    const ResolvedNativeTimeDataRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    NativeTimeData timeData;
    if (!ReadNativeTimeData(runtime, timeData)) {
        return false;
    }

    try {
        json.reserve(40);
        json += R"({"TimeHour":)";
        if (!json::AppendInteger(
                timeData.hour, json, kMaximumTimeDataJsonBytes)) {
            json.clear();
            return false;
        }
        json += R"(,"TimeMin":)";
        if (!json::AppendInteger(
                timeData.minute, json, kMaximumTimeDataJsonBytes)) {
            json.clear();
            return false;
        }
        json.push_back('}');
    } catch (...) {
        json.clear();
        return false;
    }
    return json.size() <= kMaximumTimeDataJsonBytes;
}

}
