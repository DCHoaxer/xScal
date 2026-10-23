#pragma once

#include "config/target_profile.hpp"
#include "datamanager/actor_value_reader.hpp"
#include "datamanager/pip_boy_inventory_snapshot.hpp"
#include "platform/windows_memory.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyStatusRuntime final {
    ResolvedActorValueReader reader{};
    config::NativePipBoyStatusProfile layout{};
    std::uintptr_t getActorValueModifierByType{};
    std::uintptr_t getActorCarryWeightValues{};
    std::uintptr_t getPlayerCapsCount{};
    std::uintptr_t isActorGhoulRace{};
    std::uintptr_t gameCalendarGlobal{};
    std::uintptr_t gameYearSettingValue{};
    std::uintptr_t lethalRadsSettingValue{};
};

enum class PipBoyLimb : std::size_t {
    Head,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg,
    Count,
};

// Resistance groups in the order the Pip-Boy STAT page fills them.
enum class PipBoyResistance : std::size_t {
    Damage,
    Poison,
    Fire,
    // ElectricResist + MagicResist + EnergyResist.
    Energy,
    Frost,
    // RadResistIngestion + RadResistExposure.
    Radiation,
    Count,
};

// The calendar values the Pip-Boy menu caches each frame.
struct PipBoyGameDate final {
    std::int32_t year{};
    std::int32_t month{};
    std::int32_t day{};
    std::int32_t hour{};
    std::int32_t minute{};
};

// Values as the Pip-Boy header shows them. Integer fields are truncated the
// same way the game writes them into its UI integers.
struct PipBoyStatus final {
    float health{};
    float maximumHealth{};
    std::int32_t actionPoints{};
    std::int32_t maximumActionPoints{};
    float rads{};
    float hunger{};
    float thirst{};
    std::int32_t carriedWeight{};
    std::int32_t maximumCarryWeight{};
    // Absent when the game reports no absolute limit.
    bool hasAbsoluteWeightLimit{};
    std::int32_t absoluteWeightLimit{};
    std::int32_t caps{};
    // Current limb condition values; 0 means crippled.
    std::array<float, static_cast<std::size_t>(PipBoyLimb::Count)> limbs{};
    std::array<bool, static_cast<std::size_t>(PipBoyLimb::Count)> crippled{};
    bool irradiated{};
    bool ghoul{};
    std::array<std::int32_t, static_cast<std::size_t>(PipBoyResistance::Count)>
        resistances{};
    // False while the game has no calendar.
    bool hasDate{};
    PipBoyGameDate date{};
};

[[nodiscard]] bool ResolvePipBoyStatusRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativeActorValueProfile& actorValues,
    const config::NativePipBoyStatusProfile& status,
    ResolvedPipBoyStatusRuntime& runtime) noexcept;

[[nodiscard]] bool ReadPipBoyStatus(
    const ResolvedPipBoyStatusRuntime& runtime,
    PipBoyStatus& status) noexcept;

// weaponDamage holds the equipped weapons' damage rows; null writes
// "weaponDamage":null.
[[nodiscard]] bool BuildPipBoyStatusJson(
    const ResolvedPipBoyStatusRuntime& runtime,
    std::string& json,
    const std::vector<PipBoyDamageRow>* weaponDamage = nullptr) noexcept;

}
