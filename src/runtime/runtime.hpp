#pragma once

#include "address/address_library.hpp"
#include "api/file_command_service.hpp"
#include "api/scaleform_callbacks.hpp"
#include "chat/chat_session.hpp"
#include "config/target_profile.hpp"
#include "datamanager/pip_boy_providers.hpp"
#include "datamanager/pip_boy_stats_effects_provider.hpp"
#include "datamanager/player_map_position_provider.hpp"
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
    // path is the request path on dcma.pl, e.g. "/addlib/v3/addlib.json".
    using DownloadAddressLibraryFn = bool(*)(
        const char* path,
        std::string& contents) noexcept;

    using ReadExecutableIdentityFn = bool(*)(
        std::uintptr_t moduleBase,
        address::ExecutableIdentity& identity) noexcept;

    // Starts work(context) on a new thread that nothing waits for. Returns
    // false when the thread could not be started.
    using RunInBackgroundFn = bool(*)(
        void (*work)(void* context) noexcept,
        void* context) noexcept;

    decltype(&::GetModuleFileNameW) getModuleFileName{};
    decltype(&::GetModuleHandleW) getModuleHandle{};
    ReadRuntimeVersionFn readRuntimeVersion{};
    decltype(&::MessageBoxW) messageBox{};
    DownloadAddressLibraryFn downloadAddressLibrary{};
    ReadExecutableIdentityFn readExecutableIdentity{};
    decltype(&::GetSystemTimeAsFileTime) getSystemTimeAsFileTime{};
    RunInBackgroundFn runInBackground{};
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
    datamanager::PlayerMapPositionProvider playerMapPositionProvider_;
    datamanager::PipBoyProviders pipBoyProviders_;
    ScaleformCallbackRouter callbackRouter_;
    MovieRootHookController controller_;
    GamepadInputFilterController gamepadInputFilter_;
    modules::XScalPriority xscalPriority_;
};

[[nodiscard]] VtableHookStatus InitializeCurrentProcess(
    Runtime& runtime,
    RuntimeProcessPlatform platform = SystemRuntimeProcessPlatform()) noexcept;

}
