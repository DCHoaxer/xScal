#pragma once

#include "scaleform/value.hpp"

#include <string>

namespace sf::stealth_meter {

enum class StealthState {
    Hidden,
    Detected,
    Caution,
    Danger,
};

struct Snapshot final {
    bool visible{};
    StealthState state{StealthState::Detected};
    std::string text;
    double percent{};
};

enum class ReadStatus {
    MeterUnavailable,
    Meter,
};

[[nodiscard]] ReadStatus ReadSnapshot(
    const MovieRootContext& context,
    Snapshot& snapshot) noexcept;

[[nodiscard]] bool BuildPayload(
    const Snapshot& snapshot,
    std::string& payload) noexcept;

}
