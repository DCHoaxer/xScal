#pragma once

#include "api/file_command_service.hpp"
#include "api/scaleform_callbacks.hpp"
#include "chat/chat_session.hpp"
#include "config/target_profile.hpp"
#include "datamanager/pip_boy_stats_effects_provider.hpp"
#include "datamanager/time_data_provider.hpp"
#include "hook/movie_root_hook.hpp"
#include "input/input_state.hpp"
#include "input/xinput_hook.hpp"
#include "modules/xscal_priority/xscal_priority.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace sf {

struct RuntimeProcessPlatform final {
    using ReadRuntimeVersionFn = bool(*)(
        const wchar_t* executablePath,
        config::RuntimeVersion& version) noexcept;
    using DownloadAddressLibraryFn = bool(*)(
        TargetKind target,
        const config::RuntimeVersion& runtime,
        std::string& contents) noexcept;

    decltype(&::GetModuleFileNameW) getModuleFileName{};
    decltype(&::GetModuleHandleW) getModuleHandle{};
    ReadRuntimeVersionFn readRuntimeVersion{};
    decltype(&::MessageBoxW) messageBox{};
    DownloadAddressLibraryFn downloadAddressLibrary{};
};
[[nodiscard]] RuntimeProcessPlatform SystemRuntimeProcessPlatform() noexcept;

class Runtime final {
public:
    explicit Runtime(
        VtableHookPlatform platform = SystemVtableHookPlatform()) noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] VtableHookStatus Initialize(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;

private:
    std::mutex lifecycleMutex_;
    bool initialized_{};
    FileCommandService fileCommandService_;
    chat::ChatSession chatSession_;
    InputState inputState_;
    datamanager::PipBoyStatsEffectsProvider statsEffectsProvider_;
    datamanager::TimeDataProvider timeDataProvider_;
    ScaleformCallbackRouter callbackRouter_;
    MovieRootHookController controller_;
    GamepadInputFilterController gamepadInputFilter_;
    modules::XScalPriority xscalPriority_;
};

[[nodiscard]] VtableHookStatus InitializeCurrentProcess(
    Runtime& runtime,
    RuntimeProcessPlatform platform = SystemRuntimeProcessPlatform()) noexcept;

}
