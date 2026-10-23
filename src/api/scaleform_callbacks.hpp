#pragma once

#include "config/target_profile.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

struct XScalScaleformValue;

namespace sf {

class FileCommandService;
class InputState;
struct ScaleformCall;

namespace datamanager {
class PipBoyStatsEffectsProvider;
class PlayerMapPositionProvider;
struct PipBoyProviders;
}

using PluginScaleformDispatch = bool(*)(
    std::string_view name,
    const XScalScaleformValue* arguments,
    std::uint32_t argumentCount,
    XScalScaleformValue& result) noexcept;

class ScaleformCallbackRouter final {
public:
    static constexpr std::size_t kMaximumNameLength = 64;

    ScaleformCallbackRouter(
        FileCommandService& fileCommands,
        InputState& input,
        datamanager::PipBoyStatsEffectsProvider& statsEffects,
        datamanager::PlayerMapPositionProvider& playerMapPosition,
        datamanager::PipBoyProviders& pipBoy,
        PluginScaleformDispatch pluginDispatch = nullptr) noexcept;

    void SetTargetKind(TargetKind targetKind) noexcept;
    [[nodiscard]] bool Dispatch(
        std::string_view name,
        const ScaleformCall& call) const noexcept;

private:
    FileCommandService& fileCommands_;
    InputState& input_;
    datamanager::PipBoyStatsEffectsProvider& statsEffects_;
    datamanager::PlayerMapPositionProvider& playerMapPosition_;
    datamanager::PipBoyProviders& pipBoy_;
    std::atomic<TargetKind> targetKind_{TargetKind::Steam};
    PluginScaleformDispatch pluginDispatch_;
};

}
