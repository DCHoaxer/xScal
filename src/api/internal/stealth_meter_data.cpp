#include "api/internal/stealth_meter_data.hpp"

#include "common/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sf::stealth_meter {
namespace {

constexpr char kMeterPath[] =
    "root1.TopCenterGroup_mc.StealthMeter_mc";
constexpr char kMeterVisiblePath[] =
    "root1.TopCenterGroup_mc.StealthMeter_mc.visible";
constexpr char kStateFramePath[] =
    "root1.TopCenterGroup_mc.StealthMeter_mc."
    "Internal_mc.stealthTextStates.currentFrame";
constexpr char kTextPath[] =
    "root1.TopCenterGroup_mc.StealthMeter_mc."
    "Internal_mc.stealthTextStates.stealthTextAnimStates."
    "StealthTextInstance.text";
constexpr char kPercentPath[] =
    "root1.TopCenterGroup_mc.StealthMeter_mc.LastPercent";
constexpr std::uint32_t kHiddenLastFrame = 6;
constexpr std::uint32_t kDetectedLastFrame = 12;
constexpr std::uint32_t kCautionLastFrame = 73;
constexpr std::uint32_t kDangerLastFrame = 104;

[[nodiscard]] bool DecodeFrame(
    double value,
    std::uint32_t maximum,
    std::uint32_t& frame) noexcept {
    if (!std::isfinite(value) || value < 1.0 || value > maximum ||
        std::floor(value) != value) {
        return false;
    }
    frame = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] StealthState StateForFrame(std::uint32_t frame) noexcept {
    if (frame <= kHiddenLastFrame) {
        return StealthState::Hidden;
    }
    if (frame <= kDetectedLastFrame) {
        return StealthState::Detected;
    }
    if (frame <= kCautionLastFrame) {
        return StealthState::Caution;
    }
    return StealthState::Danger;
}

[[nodiscard]] const char* StateName(StealthState state) noexcept {
    switch (state) {
        case StealthState::Hidden: return "hidden";
        case StealthState::Detected: return "detected";
        case StealthState::Caution: return "caution";
        case StealthState::Danger: return "danger";
    }
    return "detected";
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

        bool visible{};
        double stateFrameValue{};
        double percent{};
        std::string text;
        if (!GetScaleformBoolean(context, kMeterVisiblePath, visible) ||
            !GetScaleformNumber(context, kStateFramePath, stateFrameValue) ||
            !GetScaleformNumber(context, kPercentPath, percent) ||
            !GetScaleformString(context, kTextPath, text)) {
            return ReadStatus::MeterUnavailable;
        }

        std::uint32_t stateFrame{};
        if (!DecodeFrame(
                stateFrameValue, kDangerLastFrame, stateFrame)) {
            return ReadStatus::MeterUnavailable;
        }

        snapshot.visible = visible;
        snapshot.state = StateForFrame(stateFrame);
        snapshot.text = std::move(text);
        // hudmenu.swf's SetStealthMeter stores the displayed/interpolated
        // percentage in LastPercent before the next update.
        snapshot.percent = std::clamp(percent, 0.0, 100.0);
        return ReadStatus::Meter;
    } catch (...) {
        snapshot = {};
        return ReadStatus::MeterUnavailable;
    }
}

bool BuildPayload(
    const Snapshot& snapshot,
    std::string& payload) noexcept {
    payload.clear();
    try {
        payload.reserve(snapshot.text.size() + 112);
        payload.append(
            snapshot.visible
                ? R"json({"data":{"visible":true,"state":")json"
                : R"json({"data":{"visible":false,"state":")json");
        payload.append(StateName(snapshot.state));
        payload.append(R"json(","text":)json");
        json::AppendString(
            snapshot.text, payload);
        payload.append(R"json(,"percent":)json");
        if (!json::AppendFloat(
                static_cast<float>(snapshot.percent), payload)) {
            payload.clear();
            return false;
        }
        payload.append("}}");
        return true;
    } catch (...) {
        payload.clear();
        return false;
    }
}

}
