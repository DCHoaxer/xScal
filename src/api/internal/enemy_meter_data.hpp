#pragma once

#include "config/target_profile.hpp"
#include "scaleform/value.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace sf::enemy_meter {

enum class TargetKind {
    Enemy,
    Friendly,
    Neutral,
};

// Index order is damage, energy, radiation, fire, poison, cryo. The JSON
// payload writes the entries in the same order.
struct Resistances final {
    std::array<float, 6> values{};
    std::array<bool, 6> immune{};
};

struct Snapshot final {
    std::string name;
    std::optional<std::uint32_t> level;
    std::optional<double> health;
    TargetKind kind{TargetKind::Neutral};
    std::uint32_t bossType{};
    std::optional<Resistances> resistances;
};

enum class ReadStatus {
    MeterUnavailable,
    NoTarget,
    Target,
};

[[nodiscard]] ReadStatus ReadSnapshot(
    const MovieRootContext& context,
    Snapshot& snapshot) noexcept;

[[nodiscard]] bool BuildPayload(
    const Snapshot& snapshot,
    std::string& payload) noexcept;

void ConfigureNativeRuntime(
    const config::NativeEnemyMeterProfile& profile,
    std::uintptr_t moduleBase) noexcept;

void ResetNativeRuntime() noexcept;

[[nodiscard]] bool ReadNativeResistances(
    std::optional<Resistances>& resistances) noexcept;

}
