#include "datamanager/pip_boy_status_snapshot.hpp"

#include "common/json.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace sf::datamanager {
namespace {

// Fallout76.esm form IDs of the ActorValueInfo forms (0x2BC + index).
constexpr std::uint32_t kHealthFormId = 0x2D4;
constexpr std::uint32_t kActionPointsFormId = 0x2D5;
constexpr std::uint32_t kRadsFormId = 0x2E1;
constexpr std::uint32_t kRadHealthMaxFormId = 0x2EE;
constexpr std::uint32_t kFoodFormId = 0x331;
constexpr std::uint32_t kWaterFormId = 0x332;
// PerceptionCondition, LeftAttackCondition, RightAttackCondition,
// LeftMobilityCondition, RightMobilityCondition.
constexpr std::array<std::uint32_t, 5> kLimbFormIds{
    0x36C, 0x36E, 0x36F, 0x370, 0x371};
// DamageResist, PoisonResist, FireResist, ElectricResist, FrostResist,
// MagicResist, RadResistIngestion, RadResistExposure, EnergyResist, with the
// Pip-Boy resistance group each one is added to.
constexpr std::array<std::uint32_t, 9> kResistFormIds{
    0x2E3, 0x2E4, 0x2E5, 0x2E6, 0x2E7, 0x2E8, 0x2E9, 0x2EA, 0x2EB};
constexpr std::array<PipBoyResistance, 9> kResistGroups{
    PipBoyResistance::Damage,
    PipBoyResistance::Poison,
    PipBoyResistance::Fire,
    PipBoyResistance::Energy,
    PipBoyResistance::Frost,
    PipBoyResistance::Energy,
    PipBoyResistance::Radiation,
    PipBoyResistance::Radiation,
    PipBoyResistance::Energy};
// The condition event builder treats |value| <= 0.0001 (rva 0x4110100) as a
// crippled limb, and Rads >= fLethalRads * 0.6 (rva 0x4155234) as irradiated.
constexpr float kCrippledConditionEpsilon = 0.0001F;
constexpr float kIrradiatedLethalRadsFraction = 0.6F;
// GetActorAbsoluteWeightLimit returns FLT_MAX when the game sets no limit.
constexpr float kNoWeightLimit = 3.0e38F;
constexpr std::size_t kMaximumStatusJsonBytes = 16 * 1024;

using GetActorValueModifierByType = float(__fastcall*)(
    void* actorValueOwner,
    std::int32_t modifierType,
    void* actorValueInfo);
using GetActorCarryWeightValues = void(__fastcall*)(
    void* actor,
    float* carriedWeight,
    float* maximumCarryWeight,
    float* absoluteWeightLimit);
using GetPlayerCapsCount = std::int32_t(__fastcall*)(void* player);
using IsActorGhoulRace = bool(__fastcall*)(void* actor);
// Calendar getters fall back to built-in defaults when a global is missing.

struct CarryWeightValues final {
    float carried{};
    float maximum{};
    float absoluteLimit{};
};

[[nodiscard]] bool CallGetActorValueModifierByType(
    std::uintptr_t function,
    std::uintptr_t owner,
    std::int32_t modifierType,
    std::uintptr_t actorValueInfo,
    float& modifier) noexcept {
    __try {
        modifier = reinterpret_cast<GetActorValueModifierByType>(function)(
            reinterpret_cast<void*>(owner),
            modifierType,
            reinterpret_cast<void*>(actorValueInfo));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        modifier = 0.0F;
        return false;
    }
    return std::isfinite(modifier);
}

[[nodiscard]] bool CallGetActorCarryWeightValues(
    std::uintptr_t function,
    std::uintptr_t player,
    CarryWeightValues& values) noexcept {
    __try {
        reinterpret_cast<GetActorCarryWeightValues>(function)(
            reinterpret_cast<void*>(player),
            &values.carried,
            &values.maximum,
            &values.absoluteLimit);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        values = {};
        return false;
    }
    return std::isfinite(values.carried) && std::isfinite(values.maximum);
}

[[nodiscard]] bool CallGetPlayerCapsCount(
    std::uintptr_t function,
    std::uintptr_t player,
    std::int32_t& caps) noexcept {
    __try {
        caps = reinterpret_cast<GetPlayerCapsCount>(function)(
            reinterpret_cast<void*>(player));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        caps = 0;
        return false;
    }
    return caps >= 0;
}

[[nodiscard]] bool CallIsActorGhoulRace(
    std::uintptr_t function,
    std::uintptr_t player,
    bool& ghoul) noexcept {
    __try {
        ghoul = reinterpret_cast<IsActorGhoulRace>(function)(
            reinterpret_cast<void*>(player));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        ghoul = false;
        return false;
    }
    return true;
}

struct CalendarValues final {
    std::uint32_t month{};
    std::int32_t day{};
    float hour{};
};

// The game writes these values into UI integers with cvttss2si.
[[nodiscard]] bool TruncateToInt32(
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

// Reads one calendar TESGlobal value; an empty slot leaves global at 0.
[[nodiscard]] bool ReadCalendarGlobal(
    std::uintptr_t calendar,
    std::size_t slotOffset,
    std::size_t valueOffset,
    std::uintptr_t& global,
    float& value) noexcept {
    global = 0;
    value = 0.0F;
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(calendar + slotOffset),
            &global,
            sizeof(global))) {
        return false;
    }
    return global == 0 ||
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(global + valueOffset),
            &value,
            sizeof(value));
}

// Converts the values as the game's month, day and hour getters do: the
// month truncated to 16 bits, the day truncated to an int, the hour as
// stored. Those getters return a fixed default for an empty slot; xScal
// reports no date instead, so present is false then.
[[nodiscard]] bool ReadCalendarValues(
    const config::NativePipBoyStatusProfile& layout,
    std::uintptr_t calendar,
    bool& present,
    CalendarValues& values) noexcept {
    present = false;
    values = {};
    std::uintptr_t monthGlobal{};
    std::uintptr_t dayGlobal{};
    std::uintptr_t hourGlobal{};
    float month{};
    float day{};
    float hour{};
    if (!ReadCalendarGlobal(
            calendar, layout.calendarMonthOffset, layout.globalValueOffset,
            monthGlobal, month) ||
        !ReadCalendarGlobal(
            calendar, layout.calendarDayOffset, layout.globalValueOffset,
            dayGlobal, day) ||
        !ReadCalendarGlobal(
            calendar, layout.calendarHourOffset, layout.globalValueOffset,
            hourGlobal, hour)) {
        return false;
    }
    if (monthGlobal == 0 || dayGlobal == 0 || hourGlobal == 0) {
        return true;
    }
    std::int32_t wholeMonth{};
    std::int32_t wholeDay{};
    if (!TruncateToInt32(month, wholeMonth) ||
        !TruncateToInt32(day, wholeDay) ||
        !std::isfinite(hour) || hour < 0.0F || hour >= 2147483648.0F) {
        return false;
    }
    values.month = static_cast<std::uint16_t>(wholeMonth);
    values.day = wholeDay;
    values.hour = hour;
    present = true;
    return true;
}

[[nodiscard]] bool ReadCurrentRegisteredValue(
    const ResolvedActorValueReader& reader,
    std::uintptr_t owner,
    std::size_t registryOffset,
    std::uint32_t formId,
    std::uintptr_t& actorValueInfo,
    float& value) noexcept {
    return ReadRegisteredActorValueInfo(
               reader, registryOffset, formId, actorValueInfo) &&
        ReadCurrentActorValue(reader, owner, actorValueInfo, value);
}

[[nodiscard]] bool ReadCurrentRegisteredValue(
    const ResolvedActorValueReader& reader,
    std::uintptr_t owner,
    std::size_t registryOffset,
    std::uint32_t formId,
    float& value) noexcept {
    std::uintptr_t actorValueInfo{};
    return ReadCurrentRegisteredValue(
        reader, owner, registryOffset, formId, actorValueInfo, value);
}

// Reads the calendar the way the menu frame update caches it: month and the
// sign-extended low byte of the day, whole hours, and minutes rounded from
// the hour fraction.
[[nodiscard]] bool ReadGameDate(
    const ResolvedPipBoyStatusRuntime& runtime,
    bool& hasDate,
    PipBoyGameDate& date) noexcept {
    hasDate = false;
    date = {};
    std::uintptr_t calendar{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.gameCalendarGlobal),
            &calendar,
            sizeof(calendar))) {
        return false;
    }
    if (calendar == 0) {
        return true;
    }

    bool present{};
    CalendarValues values;
    if (!ReadCalendarValues(runtime.layout, calendar, present, values)) {
        return false;
    }
    if (!present) {
        return true;
    }
    PipBoyGameDate result;
    if (
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.gameYearSettingValue),
            &result.year,
            sizeof(result.year))) {
        return false;
    }
    result.month = static_cast<std::int32_t>(values.month);
    result.day = static_cast<std::int8_t>(values.day);
    const auto wholeHours = static_cast<std::uint32_t>(values.hour);
    result.hour = static_cast<std::int32_t>(wholeHours);
    const float minutes =
        (values.hour - static_cast<float>(wholeHours)) * 60.0F;
    const auto wholeMinutes = static_cast<std::int32_t>(minutes);
    result.minute = wholeMinutes +
        (minutes - static_cast<float>(wholeMinutes) >= 0.5F ? 1 : 0);
    date = result;
    hasDate = true;
    return true;
}

[[nodiscard]] bool ReadLimbsAndResistances(
    const ResolvedPipBoyStatusRuntime& runtime,
    std::uintptr_t owner,
    PipBoyStatus& status) noexcept {
    const auto& layout = runtime.layout;
    for (std::size_t index = 0; index < status.limbs.size(); ++index) {
        if (!ReadCurrentRegisteredValue(
                runtime.reader,
                owner,
                layout.limbConditionRegistryOffsets[index],
                kLimbFormIds[index],
                status.limbs[index]) ||
            !std::isfinite(status.limbs[index])) {
            return false;
        }
        status.crippled[index] =
            std::fabs(status.limbs[index]) <= kCrippledConditionEpsilon;
    }

    std::array<float, static_cast<std::size_t>(PipBoyResistance::Count)>
        sums{};
    for (std::size_t index = 0; index < kResistFormIds.size(); ++index) {
        float value{};
        if (!ReadCurrentRegisteredValue(
                runtime.reader,
                owner,
                layout.resistRegistryOffsets[index],
                kResistFormIds[index],
                value)) {
            return false;
        }
        sums[static_cast<std::size_t>(kResistGroups[index])] += value;
    }
    for (std::size_t index = 0; index < sums.size(); ++index) {
        if (!TruncateToInt32(sums[index], status.resistances[index])) {
            return false;
        }
    }
    return true;
}

}

constexpr std::array kPipBoyStatusRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, getActorValueModifierByType),
        offsetof(ResolvedPipBoyStatusRuntime, getActorValueModifierByType),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, getActorCarryWeightValues),
        offsetof(ResolvedPipBoyStatusRuntime, getActorCarryWeightValues),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, getPlayerCapsCount),
        offsetof(ResolvedPipBoyStatusRuntime, getPlayerCapsCount),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, isActorGhoulRace),
        offsetof(ResolvedPipBoyStatusRuntime, isActorGhoulRace),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, gameCalendarGlobal),
        offsetof(ResolvedPipBoyStatusRuntime, gameCalendarGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, gameYearSettingValue),
        offsetof(ResolvedPipBoyStatusRuntime, gameYearSettingValue),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyStatusProfile, lethalRadsSettingValue),
        offsetof(ResolvedPipBoyStatusRuntime, lethalRadsSettingValue),
        platform::AddressCheck::Readable),
};

bool ResolvePipBoyStatusRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoyStatusProfile& status,
    ResolvedPipBoyStatusRuntime& runtime) noexcept {
    runtime = {};
    if (status.getActorValueModifierByType == 0 ||
        status.getActorCarryWeightValues == 0 ||
        status.getPlayerCapsCount == 0 ||
        status.damageModifierType == 0 ||
        status.damageModifierType > 2 ||
        status.calendarMonthOffset == 0 ||
        status.calendarDayOffset == 0 ||
        status.calendarHourOffset == 0 ||
        status.globalValueOffset == 0) {
        return false;
    }

    ResolvedPipBoyStatusRuntime candidate{
        .layout = status,
    };
    if (!ResolveActorValueReader(
            virtualQuery, moduleBase, actorValues, candidate.reader) ||
        !platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &status,
            &candidate,
            kPipBoyStatusRuntimeBindings)) {
        return false;
    }

    const auto isReadableRegistrySlot =
        [&virtualQuery, &candidate](std::size_t registryOffset) {
            std::uintptr_t slot{};
            return registryOffset != 0 &&
                platform::TryAddAddress(
                    candidate.reader.actorValueRegistry, registryOffset, slot) &&
                platform::AreReadableAddresses(virtualQuery, {slot});
        };
    for (const auto registryOffset : {
             status.healthRegistryOffset,
             status.radHealthMaxRegistryOffset,
             status.actionPointsRegistryOffset,
             status.radsRegistryOffset,
             status.foodRegistryOffset,
             status.waterRegistryOffset}) {
        if (!isReadableRegistrySlot(registryOffset)) {
            return false;
        }
    }
    for (const auto registryOffset : status.limbConditionRegistryOffsets) {
        if (!isReadableRegistrySlot(registryOffset)) {
            return false;
        }
    }
    for (const auto registryOffset : status.resistRegistryOffsets) {
        if (!isReadableRegistrySlot(registryOffset)) {
            return false;
        }
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoyStatus(
    const ResolvedPipBoyStatusRuntime& runtime,
    PipBoyStatus& status) noexcept {
    status = {};
    const auto& reader = runtime.reader;
    const auto& layout = runtime.layout;
    std::uintptr_t owner{};
    if (!ReadLocalPlayerActorValueOwner(reader, owner)) {
        return false;
    }
    const auto player = owner - reader.layout.actorValueOwnerOffset;

    PipBoyStatus result;
    std::uintptr_t actionPointsInfo{};
    float actionPoints{};
    float actionPointsDamage{};
    CarryWeightValues carryWeight;
    if (!ReadCurrentRegisteredValue(
            reader, owner, layout.healthRegistryOffset, kHealthFormId,
            result.health) ||
        // The Pip-Boy shows RadHealthMax, which radiation lowers, as the
        // maximum, not the Health base or permanent value.
        !ReadCurrentRegisteredValue(
            reader, owner, layout.radHealthMaxRegistryOffset,
            kRadHealthMaxFormId, result.maximumHealth) ||
        !ReadCurrentRegisteredValue(
            reader, owner, layout.actionPointsRegistryOffset,
            kActionPointsFormId, actionPointsInfo, actionPoints) ||
        // Maximum AP is the current value with the damage modifier removed.
        !CallGetActorValueModifierByType(
            runtime.getActorValueModifierByType,
            owner,
            static_cast<std::int32_t>(layout.damageModifierType),
            actionPointsInfo,
            actionPointsDamage) ||
        !TruncateToInt32(actionPoints, result.actionPoints) ||
        !TruncateToInt32(
            actionPoints - actionPointsDamage, result.maximumActionPoints) ||
        !ReadCurrentRegisteredValue(
            reader, owner, layout.radsRegistryOffset, kRadsFormId,
            result.rads) ||
        !ReadCurrentRegisteredValue(
            reader, owner, layout.foodRegistryOffset, kFoodFormId,
            result.hunger) ||
        !ReadCurrentRegisteredValue(
            reader, owner, layout.waterRegistryOffset, kWaterFormId,
            result.thirst) ||
        !CallGetActorCarryWeightValues(
            runtime.getActorCarryWeightValues, player, carryWeight) ||
        !TruncateToInt32(carryWeight.carried, result.carriedWeight) ||
        !TruncateToInt32(carryWeight.maximum, result.maximumCarryWeight) ||
        !CallGetPlayerCapsCount(
            runtime.getPlayerCapsCount, player, result.caps) ||
        !ReadLimbsAndResistances(runtime, owner, result) ||
        !CallIsActorGhoulRace(runtime.isActorGhoulRace, player, result.ghoul) ||
        !ReadGameDate(runtime, result.hasDate, result.date)) {
        return false;
    }

    float lethalRads{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.lethalRadsSettingValue),
            &lethalRads,
            sizeof(lethalRads)) ||
        !std::isfinite(lethalRads)) {
        return false;
    }
    result.irradiated =
        result.rads >= lethalRads * kIrradiatedLethalRadsFraction;

    result.hasAbsoluteWeightLimit =
        std::isfinite(carryWeight.absoluteLimit) &&
        carryWeight.absoluteLimit < kNoWeightLimit &&
        TruncateToInt32(
            carryWeight.absoluteLimit, result.absoluteWeightLimit);
    if (!result.hasAbsoluteWeightLimit) {
        result.absoluteWeightLimit = 0;
    }

    status = result;
    return true;
}

bool BuildPipBoyStatusJson(
    const ResolvedPipBoyStatusRuntime& runtime,
    std::string& json,
    const std::vector<PipBoyDamageRow>* weaponDamage) noexcept {
    json.clear();
    PipBoyStatus status;
    if (!ReadPipBoyStatus(runtime, status)) {
        return false;
    }

    try {
        json.reserve(768);
        const auto appendFloat = [&json](std::string_view prefix, float value) {
            json += prefix;
            return json::AppendFloat(value, json);
        };
        const auto appendInteger =
            [&json](std::string_view prefix, std::int64_t value) {
                json += prefix;
                return json::AppendInteger(
                    value, json, kMaximumStatusJsonBytes);
            };
        bool written =
            appendFloat(R"({"HP":{"cur":)", status.health) &&
            appendFloat(R"(,"max":)", status.maximumHealth) &&
            appendInteger(R"(},"AP":{"cur":)", status.actionPoints) &&
            appendInteger(R"(,"max":)", status.maximumActionPoints) &&
            appendFloat(R"(},"rads":)", status.rads) &&
            appendFloat(R"(,"hunger":)", status.hunger) &&
            appendFloat(R"(,"thirst":)", status.thirst) &&
            appendInteger(R"(,"carryWeight":{"cur":)", status.carriedWeight) &&
            appendInteger(R"(,"max":)", status.maximumCarryWeight);
        if (written && status.hasAbsoluteWeightLimit) {
            written = appendInteger(R"(,"limit":)", status.absoluteWeightLimit);
        } else if (written) {
            json += R"(,"limit":null)";
        }
        written = written && appendInteger(R"(},"caps":)", status.caps);

        constexpr std::array<std::string_view, 5> kLimbKeys{
            R"(,"limbs":{"head":)", R"(,"leftArm":)", R"(,"rightArm":)",
            R"(,"leftLeg":)", R"(,"rightLeg":)"};
        for (std::size_t index = 0; written && index < kLimbKeys.size();
             ++index) {
            written = appendFloat(kLimbKeys[index], status.limbs[index]);
        }
        constexpr std::array<std::string_view, 5> kCrippledKeys{
            R"(},"flags":{"headCrippled":)", R"(,"leftArmCrippled":)",
            R"(,"rightArmCrippled":)", R"(,"leftLegCrippled":)",
            R"(,"rightLegCrippled":)"};
        const auto appendBool = [&json](std::string_view prefix, bool value) {
            json += prefix;
            json += value ? "true" : "false";
        };
        if (written) {
            for (std::size_t index = 0; index < kCrippledKeys.size(); ++index) {
                appendBool(kCrippledKeys[index], status.crippled[index]);
            }
            appendBool(R"(,"irradiated":)", status.irradiated);
            appendBool(R"(,"ghoul":)", status.ghoul);
        }
        constexpr std::array<std::string_view, 6> kResistKeys{
            R"(},"resist":{"damage":)", R"(,"poison":)", R"(,"fire":)",
            R"(,"energy":)", R"(,"cryo":)", R"(,"radiation":)"};
        for (std::size_t index = 0; written && index < kResistKeys.size();
             ++index) {
            written =
                appendInteger(kResistKeys[index], status.resistances[index]);
        }
        if (written && status.hasDate) {
            written =
                appendInteger(R"(},"date":{"year":)", status.date.year) &&
                appendInteger(R"(,"month":)", status.date.month) &&
                appendInteger(R"(,"day":)", status.date.day) &&
                appendInteger(R"(,"hour":)", status.date.hour) &&
                appendInteger(R"(,"minute":)", status.date.minute);
            json.push_back('}');
        } else if (written) {
            json += R"(},"date":null)";
        }
        if (written && weaponDamage != nullptr) {
            json += R"(,"weaponDamage":)";
            written = AppendDamageRowsJson(
                *weaponDamage, kMaximumStatusJsonBytes, json);
        } else if (written) {
            json += R"(,"weaponDamage":null)";
        }
        if (written) {
            json.push_back('}');
        }
        if (!written) {
            json.clear();
            return false;
        }
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumStatusJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
