#include "datamanager/pip_boy_special_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/form_text.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>

namespace sf::datamanager {
namespace {

// Fallout76.esm form IDs of the SPECIAL ActorValueInfo forms. The game
// assigns 0x2BC + ActorValue index; Strength has index 6.
constexpr std::array<std::uint32_t, 7> kSpecialFormIds{
    0x2C2, 0x2C3, 0x2C4, 0x2C5, 0x2C6, 0x2C7, 0x2C8};
constexpr std::array<std::string_view, 7> kSpecialJsonKeys{
    R"({"S":)", R"(,"P":)", R"(,"E":)", R"(,"C":)",
    R"(,"I":)", R"(,"A":)", R"(,"L":)"};
constexpr std::size_t kMaximumNameLength = 64;
constexpr std::size_t kMaximumDescriptionLength = 4096;
constexpr std::size_t kMaximumSpecialJsonBytes = 64 * 1024;

using GetSpecialBonusValue = std::uint8_t(__fastcall*)(
    void* player,
    std::uint8_t specialIndex);

// The Pip-Boy shows SPECIAL as the getter's float truncated toward zero.
[[nodiscard]] bool ConvertSpecialValue(
    float value,
    std::int32_t& converted) noexcept {
    converted = 0;
    if (!std::isfinite(value) || value <= -2147483648.0F ||
        value >= 2147483648.0F) {
        return false;
    }
    converted = static_cast<std::int32_t>(std::trunc(value));
    return true;
}

[[nodiscard]] bool CallGetSpecialBonusValue(
    std::uintptr_t function,
    std::uintptr_t player,
    std::uint8_t specialIndex,
    std::uint8_t& bonus) noexcept {
    __try {
        bonus = reinterpret_cast<GetSpecialBonusValue>(function)(
            reinterpret_cast<void*>(player), specialIndex);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        bonus = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadSpecialText(
    const ResolvedPipBoySpecialRuntime& runtime,
    std::uintptr_t actorValueInfo,
    PipBoySpecialStat& stat) noexcept {
    const auto& layout = runtime.layout;
    std::uintptr_t fullName{};
    std::uintptr_t description{};
    return platform::TryAddAddress(
               actorValueInfo, layout.actorValueFullNameOffset, fullName) &&
        ReadFullName(
            fullName,
            layout.getFullNameVtableOffset,
            kMaximumNameLength,
            stat.name) &&
        platform::TryAddAddress(
            actorValueInfo, layout.actorValueDescriptionOffset, description) &&
        ReadDescription(
            description,
            layout.getDescriptionVtableOffset,
            reinterpret_cast<GameDeallocate>(runtime.deallocate),
            kMaximumDescriptionLength,
            stat.description);
}

[[nodiscard]] bool AppendSpecialDetail(
    const PipBoySpecialStat& stat,
    std::string& json) {
    json += R"({"name":)";
    if (!json::AppendString(stat.name, json, kMaximumSpecialJsonBytes)) {
        return false;
    }
    json += R"(,"value":)";
    if (!json::AppendInteger(stat.value, json, kMaximumSpecialJsonBytes)) {
        return false;
    }
    json += R"(,"bonus":)";
    if (!json::AppendInteger(stat.bonus, json, kMaximumSpecialJsonBytes)) {
        return false;
    }
    json += R"(,"description":)";
    if (!json::AppendString(
            stat.description, json, kMaximumSpecialJsonBytes)) {
        return false;
    }
    json.push_back('}');
    return true;
}

}

bool ResolvePipBoySpecialRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoySpecialProfile& special,
    ResolvedPipBoySpecialRuntime& runtime) noexcept {
    runtime = {};
    if (special.actorValueFullNameOffset == 0 ||
        special.getFullNameVtableOffset == 0 ||
        special.actorValueDescriptionOffset == 0 ||
        special.getDescriptionVtableOffset == 0) {
        return false;
    }

    ResolvedPipBoySpecialRuntime candidate{
        .layout = special,
    };
    if (!ResolveActorValueReader(
            virtualQuery, moduleBase, actorValues, candidate.reader) ||
        !platform::ResolveRequiredRvas(moduleBase, {
            {special.getSpecialBonusValue, &candidate.getSpecialBonusValue},
            {special.deallocate, &candidate.deallocate}}) ||
        !platform::AreExecutableAddresses(virtualQuery, {
            candidate.getSpecialBonusValue,
            candidate.deallocate})) {
        return false;
    }

    for (const auto registryOffset : special.registryOffsets) {
        std::uintptr_t slot{};
        if (registryOffset == 0 ||
            !platform::TryAddAddress(
                candidate.reader.actorValueRegistry, registryOffset, slot) ||
            !platform::AreReadableAddresses(virtualQuery, {slot})) {
            return false;
        }
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoySpecialStats(
    const ResolvedPipBoySpecialRuntime& runtime,
    PipBoySpecialStats& stats) noexcept {
    stats = {};
    std::uintptr_t owner{};
    if (!ReadLocalPlayerActorValueOwner(runtime.reader, owner)) {
        return false;
    }
    const auto player = owner - runtime.reader.layout.actorValueOwnerOffset;

    PipBoySpecialStats result;
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto& stat = result[index];
        std::uintptr_t actorValueInfo{};
        float value{};
        if (!ReadRegisteredActorValueInfo(
                runtime.reader,
                runtime.layout.registryOffsets[index],
                kSpecialFormIds[index],
                actorValueInfo) ||
            !ReadCurrentActorValue(
                runtime.reader, owner, actorValueInfo, value) ||
            !ConvertSpecialValue(value, stat.value) ||
            !CallGetSpecialBonusValue(
                runtime.getSpecialBonusValue,
                player,
                static_cast<std::uint8_t>(index),
                stat.bonus) ||
            !ReadSpecialText(runtime, actorValueInfo, stat)) {
            return false;
        }
    }

    stats = std::move(result);
    return true;
}

bool BuildPipBoySpecialJson(
    const ResolvedPipBoySpecialRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    PipBoySpecialStats stats;
    if (!ReadPipBoySpecialStats(runtime, stats)) {
        return false;
    }

    try {
        json.reserve(1024);
        for (std::size_t index = 0; index < stats.size(); ++index) {
            json += kSpecialJsonKeys[index];
            if (!json::AppendInteger(
                    stats[index].value, json, kMaximumSpecialJsonBytes)) {
                json.clear();
                return false;
            }
        }
        json += R"(,"details":[)";
        for (std::size_t index = 0; index < stats.size(); ++index) {
            if (index != 0) {
                json.push_back(',');
            }
            if (!AppendSpecialDetail(stats[index], json)) {
                json.clear();
                return false;
            }
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumSpecialJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
