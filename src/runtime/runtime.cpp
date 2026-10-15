#include "runtime/runtime.hpp"

#include <yyjson.h>

#include "address/address_library.hpp"
#include "api/internal/enemy_meter_data.hpp"
#include "api/mod_storage.hpp"
#include "api/scaleform_callbacks.hpp"
#include "api/scaleform_movie_interface.hpp"
#include "chat/chat_interface.hpp"
#include "config/config.h"
#include "platform/diagnostics.hpp"
#include "plugins/plugins.hpp"

#include <array>
#include <cstddef>
#include <cwchar>
#include <vector>

namespace sf {
namespace {

address::Library activeAddressLibrary;

// Formats a runtime version as "major.minor.build.revision" for log and message text.
[[nodiscard]] std::array<char, 24> FormatRuntimeVersion(
    const config::RuntimeVersion& version) noexcept {
    std::array<char, 24> text{};
    std::snprintf(
        text.data(),
        text.size(),
        "%hu.%hu.%hu.%hu",
        version.major,
        version.minor,
        version.build,
        version.revision);
    return text;
}

enum class AddressLibraryUpdateStatus {
    Unavailable,
    Declined,
    Installed,
};

// Reads the version of a Windows PE file from its file version info.
[[nodiscard]] bool ReadSystemRuntimeVersion(const wchar_t* path, config::RuntimeVersion& version) noexcept {
    if (path == nullptr || path[0] == L'\0') return false;
    try {
        DWORD ignored{};
        const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
        if (size == 0) return false;
        std::vector<std::byte> info(size);
        if (::GetFileVersionInfoW(path, 0, size, info.data()) == FALSE) return false;
        void* raw{};
        UINT rawSize{};
        if (::VerQueryValueW(info.data(), L"\\", &raw, &rawSize) == FALSE ||
            raw == nullptr || rawSize < sizeof(VS_FIXEDFILEINFO)) return false;
        const auto* fixed = static_cast<const VS_FIXEDFILEINFO*>(raw);
        version = {HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                   HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS)};
        return true;
    } catch (...) { return false; }
}

// Logs the basic runtime configuration to the diagnostic log.
void LogRuntimeConfiguration(
    const TargetDefinition& target,
    std::uintptr_t base,
    const config::RuntimeVersion* version,
    const TargetProfile* profile = nullptr) noexcept {
    DiagnosticLogFormat(
        "xScal v%u.%u.%u minimal init by @DCHoaxer95",
        static_cast<unsigned int>(config::kXScalVersion.major),
        static_cast<unsigned int>(config::kXScalVersion.minor),
        static_cast<unsigned int>(config::kXScalVersion.patch));
    DiagnosticLog("Uses yyjson v" YYJSON_VERSION_STRING " by @ibireme");
    if (version != nullptr) {
        DiagnosticLogFormat(
            "imagebase = %p runtime = %s",
            reinterpret_cast<void*>(base),
            FormatRuntimeVersion(*version).data());
    } else {
        DiagnosticLogFormat("imagebase = %p runtime = unknown",
            reinterpret_cast<void*>(base));
    }
    DiagnosticLogFormat("platform: %s", TargetKindName(target.kind));
    if (profile != nullptr) {
        DiagnosticLogFormat(
            "Address Library: loaded (%zu entries, RVA setMember=0x%llX)",
            activeAddressLibrary.EntryCount(),
            static_cast<unsigned long long>(profile->setMemberOffset));
    }
    if (config::GetRuntimeConfig().loadedFromIni) {
        DiagnosticLog("xScal config: loaded xscal.ini");
    }
}

// Displays a message box indicating that the runtime version could not be read.
void ShowRuntimeVersionError(
    const RuntimeProcessPlatform& platform) noexcept {
    platform.messageBox(
        nullptr,
        L"xScal could not read the Fallout 76 runtime version.\n\n"
        L"A matching Address Library cannot be selected.\n\n"
        L"xScal features will be unavailable for this session.",
        L"xScal",
        MB_OK | MB_ICONEXCLAMATION);
}

// Displays a message box indicating that the Address Library is missing or invalid. 
void ShowAddressLibraryError(
    const RuntimeProcessPlatform& platform,
    const config::RuntimeVersion& found,
    const wchar_t* problem) noexcept {
    const auto runtimeText = FormatRuntimeVersion(found);
    std::array<wchar_t, 1024> message{};
    std::swprintf(message.data(), message.size(),
        L"%ls\n\n"
        L"Fallout 76 runtime: %hs\n\n"
        L"Missing or invalid Address Library for this runtime.\n\n"
        L"Wait for xscal to automatically download it if the new database is available:\n"
        L"If not, you can wait, or manually download the latest Address Library from NexusMods (if published):\n"
        L"https://www.nexusmods.com/fallout76/mods/4298/\n"
        L"Note that I am publishing the adress library manually, so it may take some time after update for the new version to be available.\n\n"
        L"xScal features will be unavailable for this session.",
        problem,
        runtimeText.data());
    platform.messageBox(nullptr, message.data(), L"xScal", MB_OK | MB_ICONEXCLAMATION);
}

// Updates the Address Library for the given runtime if a new version is available.
[[nodiscard]] AddressLibraryUpdateStatus UpdateMissingAddressLibrary(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uintptr_t moduleBase) noexcept {
    const auto runtimeText = FormatRuntimeVersion(runtime);
    std::string contents;
    if (!platform.downloadAddressLibrary(target, runtime, contents)) {
        DiagnosticLogFormat(
            "Address Library: no download available for runtime %s (%s)",
            runtimeText.data(),
            TargetKindName(target));
        return AddressLibraryUpdateStatus::Unavailable;
    }

    address::Library downloaded;
    if (!downloaded.Parse(contents, target, runtime, moduleBase)) {
        DiagnosticLogFormat(
            "Address Library: downloaded file is invalid for runtime %s (%s)",
            runtimeText.data(),
            TargetKindName(target));
        return AddressLibraryUpdateStatus::Unavailable;
    }

    std::array<wchar_t, 768> message{};
    std::swprintf(
        message.data(),
        message.size(),
        L"A new xScal Address Library was found for Fallout 76 "
        L"%hs (%hs).\n\n"
        L"It is required for xScal to work with this version of the game.\n\n"
        L"Do you want to install it now?",
        runtimeText.data(),
        TargetKindName(target));
    if (platform.messageBox(
            nullptr,
            message.data(),
            L"xScal Address Library",
            MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        DiagnosticLog("Address Library: installation declined");
        return AddressLibraryUpdateStatus::Declined;
    }
    if (!address::StoreAddressLibrary(target, runtime, contents)) {
        DiagnosticLogFormat(
            "Address Library: downloaded file could not be installed "
            "(Windows error %lu)",
            ::GetLastError());
        return AddressLibraryUpdateStatus::Unavailable;
    }
    DiagnosticLogFormat(
        "Address Library: installed runtime %s (%s)",
        runtimeText.data(),
        TargetKindName(target));
    platform.messageBox(
        nullptr,
        L"The new xScal Address Library was installed.\n\n"
        L"Restart Fallout 76 manually to activate xScal for this game version.",
        L"xScal Address Library",
        MB_OK | MB_ICONINFORMATION);
    return AddressLibraryUpdateStatus::Installed;
}
}


RuntimeProcessPlatform SystemRuntimeProcessPlatform() noexcept {
    return {
        &::GetModuleFileNameW, &::GetModuleHandleW,
        &ReadSystemRuntimeVersion, &::MessageBoxW,
        &address::DownloadAddressLibrary,
    };
}

Runtime::Runtime(VtableHookPlatform platform) noexcept
    : statsEffectsProvider_(platform.virtualQuery),
      timeDataProvider_(platform.virtualQuery),
      playerMapPositionProvider_(platform.virtualQuery),
      callbackRouter_(
          fileCommandService_,
          inputState_,
          statsEffectsProvider_,
          timeDataProvider_,
          playerMapPositionProvider_),
      controller_(callbackRouter_, platform),
      gamepadInputFilter_(platform),
      xscalPriority_(
          modules::SystemXScalPriorityWindowsApi(),
          config::GetRuntimeConfig().xscalPriority) {}

VtableHookStatus Runtime::Initialize(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept
{
    std::scoped_lock lifecycleLock{lifecycleMutex_};
    if (moduleBase == 0) return VtableHookStatus::InvalidArgument;
    if (initialized_) {
        return VtableHookStatus::AlreadyActive;
    }

    if (!fileCommandService_.Start()) {
        DiagnosticLog("xScal FileCommandService: worker start failed");
        return VtableHookStatus::AllocationFailed;
    }
    sf::modstorage::GlobalService().AttachFileWriter(&fileCommandService_);
    if (config::GetRuntimeConfig().chat.enabled && !chatSession_.Start()) {
        DiagnosticLog("xScal chat: initialization failed");
        sf::modstorage::GlobalService().AttachFileWriter(nullptr);
        fileCommandService_.Stop();
        return VtableHookStatus::AllocationFailed;
    }
    chat::SetChatInterfaceSession(
        config::GetRuntimeConfig().chat.enabled ? &chatSession_ : nullptr);

    callbackRouter_.SetTargetKind(profile.kind);
    const auto bridgeStatus = controller_.Install(
        profile, moduleBase, config::GetRuntimeConfig().scaleform);
    if (bridgeStatus == VtableHookStatus::Installed) {
        initialized_ = true;
        const auto gamepadFilterStatus = gamepadInputFilter_.Install(moduleBase);
        DiagnosticLogFormat(
            "xScal GamepadInputFilter: %s",
            VtableHookStatusName(gamepadFilterStatus));
        enemy_meter::ConfigureNativeRuntime(
            profile.dataManager.enemyMeter, moduleBase);
        const bool statsEffectsInstalled =
            profile.pipBoyStatsEffectsEnabled &&
            statsEffectsProvider_.Install(profile, moduleBase);
        const bool timeDataInstalled =
            timeDataProvider_.Install(profile, moduleBase);
        const bool playerMapPositionInstalled =
            playerMapPositionProvider_.Install(profile, moduleBase);
        DiagnosticLogFormat(
            "xScal Providers: {StatsEffectsProvider: %s, TimeDataProvider: %s, PlayerMapPositionProvider: %s}",
            statsEffectsInstalled ? "Enabled" : "Disabled",
            timeDataInstalled ? "Enabled" : "Disabled",
            playerMapPositionInstalled ? "Enabled" : "Disabled");
        DiagnosticLogFormat(
            "xScal Modules: {xScalPriority: %s}",
            config::GetRuntimeConfig().xscalPriority.enabled
                ? "Enabled"
                : "Disabled");
        xscalPriority_.Load();
    } else {
        chat::SetChatInterfaceSession(nullptr);
        chatSession_.Stop();
        sf::modstorage::GlobalService().AttachFileWriter(nullptr);
        fileCommandService_.Stop();
    }
    return bridgeStatus;
}

VtableHookStatus InitializeCurrentProcess(
    Runtime& runtime,
    RuntimeProcessPlatform platform) noexcept {
    // validate platform
    if (platform.getModuleFileName == nullptr || platform.getModuleHandle == nullptr ||
        platform.readRuntimeVersion == nullptr || platform.messageBox == nullptr ||
        platform.downloadAddressLibrary == nullptr) {
        return VtableHookStatus::InvalidPlatform;
    }

    // exe
    std::array<wchar_t, 32768> path{};
    const DWORD length = platform.getModuleFileName(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return VtableHookStatus::InvalidArgument;

    // gets basename
    const std::wstring_view fullPath{path.data(), length};
    const std::size_t separator = fullPath.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos ? fullPath : fullPath.substr(separator + 1);

    // detect platform of the executable
    const TargetDefinition* target = SelectTarget(name);
    if (target == nullptr) return VtableHookStatus::InvalidArgument;

    // return mh of the process
    const HMODULE module = platform.getModuleHandle(nullptr);
    if (module == nullptr) return VtableHookStatus::InvalidArgument;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    config::RuntimeVersion found{};

    if (!platform.readRuntimeVersion(path.data(), found)) {
        LogRuntimeConfiguration(*target, base, nullptr);
        ShowRuntimeVersionError(platform);
        return VtableHookStatus::RuntimeVersionUnavailable;
    }
    plugins::SetAddressLibrary(nullptr);
    auto addressStatus = activeAddressLibrary.Load(
        target->kind, found, base);
    if (addressStatus == address::LoadStatus::Missing &&
        config::GetRuntimeConfig().addressLibrary.autoUpdate) {
        const auto updateStatus = UpdateMissingAddressLibrary(
            platform, target->kind, found, base);
        if (updateStatus == AddressLibraryUpdateStatus::Declined) {
            return VtableHookStatus::AddressLibraryMissing;
        }
        if (updateStatus == AddressLibraryUpdateStatus::Installed) {
            return VtableHookStatus::AddressLibraryRestartRequired;
        }
    }
    if (addressStatus != address::LoadStatus::Loaded) {
        LogRuntimeConfiguration(*target, base, &found);
        const char* statusText = "invalid database";
        const wchar_t* problem =
            L"The xScal Address Library file is invalid or corrupt.";
        VtableHookStatus failure = VtableHookStatus::AddressLibraryInvalid;
        std::array<wchar_t, 192> versionProblem{};
        if (addressStatus == address::LoadStatus::Missing) {
            statusText = "file not found";
            problem = L"No xScal Address Library is installed for this runtime.";
            failure = VtableHookStatus::AddressLibraryMissing;
        } else if (addressStatus == address::LoadStatus::ReadFailed) {
            statusText = "file could not be read";
            problem = L"The xScal Address Library file could not be read.";
        } else if (addressStatus == address::LoadStatus::RequiresNewerXScal) {
            statusText = "newer xScal version required";
            problem = versionProblem.data();
            failure = VtableHookStatus::AddressLibraryRequiresNewerXScal;
            const auto required = activeAddressLibrary.RequiredXScalVersion();
            DiagnosticLogFormat(
                "Address Library requires xScal %hhu.%hhu.%hhu or newer",
                required.major,
                required.minor,
                required.patch);
            std::swprintf(
                versionProblem.data(),
                versionProblem.size(),
                L"This Address Library requires xScal %hhu.%hhu.%hhu or newer. "
                L"Update xScal before using this file.",
                required.major,
                required.minor,
                required.patch);
        }
        const auto foundText = FormatRuntimeVersion(found);
        DiagnosticLogFormat(
            "Address Library: %s for runtime %s (%s)",
            statusText,
            foundText.data(),
            TargetKindName(target->kind));
        ShowAddressLibraryError(platform, found, problem);
        return failure;
    }

    TargetProfile profile{};
    const auto* const addressInterface = activeAddressLibrary.Interface();
    if (addressInterface == nullptr ||
        !ReadTargetProfile(*target, found, *addressInterface, profile)) {
        LogRuntimeConfiguration(*target, base, &found);
        DiagnosticLog("Address Library: required xScal IDs are missing");
        ShowAddressLibraryError(
            platform,
            found,
            L"The xScal Address Library is missing required xScal IDs.");
        return VtableHookStatus::AddressLibraryIncomplete;
    }
    LogRuntimeConfiguration(*target, base, &found, &profile);
    plugins::SetAddressLibrary(addressInterface);
    const auto status = runtime.Initialize(profile, base);
    if (status == VtableHookStatus::Installed) {
        plugins::SetTargetKind(profile.kind);
        plugins::SetScaleformMovieInterface(
            movieapi::Interface(), movieapi::InterfaceV1());
    }
    return status;
}
}
