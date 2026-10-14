#include "api/internal/enemy_meter_data.hpp"

#include "common/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sf::enemy_meter {
namespace {

constexpr char kMeterPath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc";
constexpr char kMeterVisiblePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.visible";
constexpr char kEnemyBarVisiblePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBarEnemy_mc.visible";
constexpr char kFriendlyBarVisiblePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBarFriendly_mc.visible";
constexpr char kGenericBarVisiblePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBar_mc.visible";
constexpr char kNamePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc."
    "DisplayText_mc.DisplayText_tf.text";
constexpr char kLevelPath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc."
    "LevelText_mc.LevelText_tf.text";
constexpr char kEncounterVisiblePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc."
    "EncounterHolder_mc.visible";
constexpr char kEncounterLevelFramePath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc."
    "EncounterHolder_mc.Encounter_mc.currentFrame";
constexpr char kEnemyPercentPath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBarEnemy_mc.Percent";
constexpr char kFriendlyPercentPath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBarFriendly_mc.Percent";
constexpr char kGenericPercentPath[] =
    "root1.TopCenterGroup_mc.EnemyHealthMeter_mc.MeterBar_mc.Percent";
constexpr char kRolloverVisiblePath[] =
    "root1.CenterGroup_mc.RolloverWidget_mc.visible";
constexpr char kRolloverNamePath[] =
    "root1.CenterGroup_mc.RolloverWidget_mc.RolloverName_tf.text";

constexpr std::array<const char*, 6> kResistanceNames{
    "damage",
    "energy",
    "radiation",
    "fire",
    "poison",
    "cryo",
};

constexpr const char* kHostilityPaths[] = {
    "root1.CenterGroup_mc.HUDCrosshair_mc.CrosshairBase_mc.targetIsHostile",
    "root1.HUDCrosshair_mc.CrosshairBase_mc.targetIsHostile",
    "root1.Crosshair_mc.CrosshairBase_mc.targetIsHostile",
};

[[nodiscard]] bool IsAsciiSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void TrimAscii(std::string& value) {
    const auto first = std::find_if_not(
        value.begin(), value.end(), IsAsciiSpace);
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), IsAsciiSpace).base();
    if (first >= last) {
        value.clear();
        return;
    }
    value.assign(first, last);
}

[[nodiscard]] char AsciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

[[nodiscard]] bool EqualsAsciiInsensitive(
    std::string_view left,
    std::string_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](char lhs, char rhs) {
                return AsciiLower(lhs) == AsciiLower(rhs);
            });
}

[[nodiscard]] std::optional<std::uint32_t> ParseLevel(
    std::string value) {
    TrimAscii(value);
    if (value.empty() || value.front() == '_' || value.back() == '_') {
        return std::nullopt;
    }

    std::string digits;
    digits.reserve(value.size());
    for (char character : value) {
        if (character == '_') {
            continue;
        }
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        digits.push_back(character);
    }

    std::uint32_t level{};
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), level);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return level;
}

[[nodiscard]] const char* KindName(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::Enemy: return "enemy";
        case TargetKind::Friendly: return "friendly";
        case TargetKind::Neutral: return "neutral";
    }
    return "neutral";
}

[[nodiscard]] bool QueryUnsigned(
    const MovieRootContext& context,
    const char* path,
    std::uint32_t& result) noexcept {
    double value{};
    if (!GetScaleformNumber(context, path, value) ||
        value < 0.0 ||
        value > static_cast<double>(
            (std::numeric_limits<std::uint32_t>::max)()) ||
        std::trunc(value) != value) {
        return false;
    }
    result = static_cast<std::uint32_t>(value);
    return true;
}

void ReadEncounterType(
    const MovieRootContext& context,
    std::uint32_t& bossType) noexcept {
    bossType = 0;

    bool holderVisible{};
    if (!GetScaleformBoolean(
            context, kEncounterVisiblePath, holderVisible) ||
        !holderVisible) {
        return;
    }

    std::uint32_t levelFrame{};
    if (QueryUnsigned(
            context, kEncounterLevelFramePath, levelFrame) &&
        levelFrame >= 1 && levelFrame <= 3) {
        bossType = levelFrame;
    }
}

[[nodiscard]] bool ReadHostility(
    const MovieRootContext& context,
    bool& hostile) noexcept {
    for (const char* path : kHostilityPaths) {
        if (GetScaleformBoolean(context, path, hostile)) {
            return true;
        }
    }
    return false;
}

}

ReadStatus ReadSnapshot(
    const MovieRootContext& context,
    Snapshot& snapshot) noexcept {
    snapshot = {};
    try {
        if (!HasScaleformObject(context, kMeterPath)) {
            return ReadStatus::MeterUnavailable;
        }

        bool meterVisible{};
        const bool hasMeterVisible =
            GetScaleformBoolean(context, kMeterVisiblePath, meterVisible);
        bool rolloverVisible{};
        const bool hasRolloverVisible =
            GetScaleformBoolean(
                context, kRolloverVisiblePath, rolloverVisible);
        if ((!hasMeterVisible || !meterVisible) &&
            (!hasRolloverVisible || !rolloverVisible)) {
            return ReadStatus::NoTarget;
        }

        std::string name;
        if (!GetScaleformString(context, kNamePath, name)) {
            return ReadStatus::NoTarget;
        }
        TrimAscii(name);
        if (name.empty()) {
            return ReadStatus::NoTarget;
        }

        if (!hasMeterVisible || !meterVisible) {
            std::string rolloverName;
            if (!GetScaleformString(
                    context, kRolloverNamePath, rolloverName)) {
                return ReadStatus::NoTarget;
            }
            TrimAscii(rolloverName);
            if (!EqualsAsciiInsensitive(name, rolloverName)) {
                return ReadStatus::NoTarget;
            }
        }

        bool enemyBarVisible{};
        const bool hasEnemyBarVisible =
            GetScaleformBoolean(
                context, kEnemyBarVisiblePath, enemyBarVisible);
        bool friendlyBarVisible{};
        const bool hasFriendlyBarVisible =
            GetScaleformBoolean(
                context, kFriendlyBarVisiblePath, friendlyBarVisible);
        bool genericBarVisible{};
        const bool hasGenericBarVisible =
            GetScaleformBoolean(
                context, kGenericBarVisiblePath, genericBarVisible);

        TargetKind kind{};
        if (hasFriendlyBarVisible && friendlyBarVisible) {
            kind = TargetKind::Friendly;
        } else if (hasGenericBarVisible && genericBarVisible) {
            bool hostile{};
            const bool hasHostility = ReadHostility(context, hostile);
            kind = hasHostility && !hostile
                ? TargetKind::Friendly
                : TargetKind::Neutral;
        } else if (hasEnemyBarVisible && enemyBarVisible) {
            kind = TargetKind::Enemy;
        } else {
            bool hostile{};
            if (!ReadHostility(context, hostile)) {
                return ReadStatus::NoTarget;
            }
            kind = hostile ? TargetKind::Enemy : TargetKind::Friendly;
        }

        std::string levelText;
        std::optional<std::uint32_t> level;
        if (GetScaleformString(context, kLevelPath, levelText)) {
            level = ParseLevel(std::move(levelText));
        }

        std::uint32_t bossType{};
        ReadEncounterType(context, bossType);

        double percent{};
        bool hasPercent{};
        if (kind == TargetKind::Enemy) {
            hasPercent = GetScaleformNumber(
                context, kEnemyPercentPath, percent);
        } else if (kind == TargetKind::Friendly) {
            hasPercent = GetScaleformNumber(
                context, kGenericPercentPath, percent);
            if (!hasPercent) {
                hasPercent =
                    GetScaleformNumber(
                        context, kFriendlyPercentPath, percent);
            }
        } else {
            hasPercent = GetScaleformNumber(
                context, kGenericPercentPath, percent);
        }

        snapshot.name = std::move(name);
        snapshot.level = level;
        snapshot.kind = kind;
        snapshot.bossType = bossType;
        if (hasPercent) {
            snapshot.health = std::clamp(percent, 0.0, 1.0);
        }
        return ReadStatus::Target;
    } catch (...) {
        snapshot = {};
        return ReadStatus::NoTarget;
    }
}

bool BuildPayload(
    const Snapshot& snapshot,
    std::string& payload) noexcept {
    payload.clear();
    try {
        payload.reserve(snapshot.name.size() + 280);
        payload.append(R"json({"data":{"name":)json");
        json::AppendString(
            snapshot.name, payload);
        payload.append(R"json(,"level":)json");
        if (snapshot.level.has_value()) {
            payload.append(std::to_string(*snapshot.level));
        } else {
            payload.append("null");
        }
        payload.append(R"json(,"health":)json");
        // std::clamp passes NaN through, so a NaN health is written as null.
        // MSVC 14.29's double overload pulls roughly 120 KiB of formatting
        // tables into the DLL. HUD health is clamped to [0, 1], so the value
        // goes through the float serializer.
        if (snapshot.health.has_value() &&
            std::isfinite(*snapshot.health)) {
            if (!json::AppendFloat(
                    static_cast<float>(*snapshot.health), payload)) {
                return false;
            }
        } else {
            payload.append("null");
        }
        payload.append(
            snapshot.kind == TargetKind::Enemy
                ? R"json(,"hostile":true)json"
                : R"json(,"hostile":false)json");
        payload.append(
            snapshot.kind == TargetKind::Friendly
                ? R"json(,"friendly":true,"kind":")json"
                : R"json(,"friendly":false,"kind":")json");
        payload.append(KindName(snapshot.kind));
        payload.append(R"json(","bosstype":)json");
        payload.append(std::to_string(snapshot.bossType));
        if (snapshot.resistances.has_value()) {
            const auto& resistances = *snapshot.resistances;
            payload.append(R"json(,"resistances":{)json");
            for (std::size_t index = 0; index < kResistanceNames.size();
                 ++index) {
                if (index != 0) {
                    payload.push_back(',');
                }
                payload.push_back('"');
                payload.append(kResistanceNames[index]);
                payload.append(R"json(":)json");
                if (!json::AppendFloat(resistances.values[index], payload)) {
                    return false;
                }
            }
            if (std::find(
                    resistances.immune.begin(),
                    resistances.immune.end(),
                    true) != resistances.immune.end()) {
                payload.append(R"json(,"immune":{)json");
                for (std::size_t index = 0; index < kResistanceNames.size();
                     ++index) {
                    if (index != 0) {
                        payload.push_back(',');
                    }
                    payload.push_back('"');
                    payload.append(kResistanceNames[index]);
                    payload.append(R"json(":)json");
                    payload.append(
                        resistances.immune[index] ? "true" : "false");
                }
                payload.push_back('}');
            }
            payload.push_back('}');
        }
        payload.append("}}");
        return true;
    } catch (...) {
        payload.clear();
        return false;
    }
}

}
