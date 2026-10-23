#include "runtime/runtime.hpp"

#include <libdeflate.h>
#include <yyjson.h>

#include "address/address_library.hpp"
#include "address/address_library_key.hpp"
#include "api/internal/enemy_meter_data.hpp"
#include "api/mod_storage.hpp"
#include "api/scaleform_callbacks.hpp"
#include "api/scaleform_movie_interface.hpp"
#include "archive/archive_stream_hook.hpp"
#include "chat/chat_interface.hpp"
#include "config/config.h"
#include "platform/diagnostics.hpp"
#include "plugins/plugins.hpp"

#include <array>
#include <cstddef>
#include <cwchar>
#include <memory>
#include <new>
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
    DiagnosticLog("Uses libdeflate v" LIBDEFLATE_VERSION_STRING " by Eric Biggers");
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
    const config::RuntimeVersion& found) noexcept {
    const auto runtimeText = FormatRuntimeVersion(found);
    std::array<wchar_t, 1024> message{};
    std::swprintf(message.data(), message.size(),
        L"Oh no. The Address Library has flatlined.\n\n"
        L"Fallout 76 runtime: %hs\n\n"
        L"What went wrong?\n"
        L"xScal tried to read the Address Library and immediately regretted being born. "
        L"Missing or invalid Address Library for this runtime.\n\n"
        L"Wait for xscal to automatically download it if the new database is available:\n"
        L"If not, you can wait, or manually download the latest Address Library from NexusMods (if published):\n"
        L"https://www.nexusmods.com/fallout76/mods/4298/\n\n"
        L"Note that I am publishing the adress library manually, so it may take some time "
        L"after update for the new version to be available.\n\n"
        L"xScal features are now taking an unpaid vacation for this session.",
        runtimeText.data());
    platform.messageBox(
        nullptr,
        message.data(),
        L"xScal",
        MB_OK | MB_ICONEXCLAMATION);
}

// The file an installed database was published as.
struct InstalledFile final {
    address::format::XScalVersion release;
    std::uint32_t revision;
};

[[nodiscard]] std::uint64_t CurrentFileTime(
    const RuntimeProcessPlatform& platform) noexcept {
    FILETIME now{};
    platform.getSystemTimeAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
        now.dwLowDateTime;
}

// Logs why a database file was rejected, with the detail the status carries.
void LogAddressLibraryRejection(
    const address::Library& library,
    address::LoadStatus status,
    const address::ExecutableIdentity& executable) noexcept {
    if (status == address::LoadStatus::BadSignature) {
        DiagnosticLogFormat(
            "Address Library: signature check failed (%s)",
            plugins::signing::DescribeCheck(library.LastSignatureCheck()));
    } else if (status == address::LoadStatus::WrongExecutable) {
        const auto expected = library.FileExecutable();
        DiagnosticLogFormat(
            "Address Library: file is for another executable build "
            "(file timestamp 0x%08X size 0x%08X, running timestamp 0x%08X size 0x%08X)",
            expected.timeDateStamp,
            expected.imageSize,
            executable.timeDateStamp,
            executable.imageSize);
    }
}

// Downloads the server's database for this runtime and writes it after the
// user agrees. With installed set, the download is offered only when its
// release and revision are newer than the installed file's and the user has
// not declined that file.
[[nodiscard]] AddressLibraryUpdateStatus OfferAddressLibraryDownload(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable,
    std::uintptr_t moduleBase,
    const InstalledFile* installed) noexcept {
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
    const auto parseStatus = downloaded.Parse(
        contents, target, runtime, executable, moduleBase);
    if (parseStatus != address::LoadStatus::Loaded) {
        DiagnosticLogFormat(
            "Address Library: downloaded file is invalid for runtime %s (%s)",
            runtimeText.data(),
            TargetKindName(target));
        LogAddressLibraryRejection(downloaded, parseStatus, executable);
        return AddressLibraryUpdateStatus::Unavailable;
    }
    const auto release = downloaded.ReleaseXScalVersion();
    const auto revision = downloaded.Revision();
    if (installed != nullptr) {
        if (!address::format::IsFileNewer(
                release, revision, installed->release, installed->revision)) {
            DiagnosticLogFormat(
                "Address Library: server has no newer file for runtime %s (%s)",
                runtimeText.data(),
                TargetKindName(target));
            return AddressLibraryUpdateStatus::Unavailable;
        }
        if (address::IsUpdateDeclined(target, runtime, release, revision)) {
            DiagnosticLogFormat(
                "Address Library: server file %hhu.%hhu.%hhu revision %u was declined earlier",
                release.major,
                release.minor,
                release.patch,
                revision);
            return AddressLibraryUpdateStatus::Unavailable;
        }
    }

    std::array<wchar_t, 160> versions{};
    if (installed != nullptr) {
        std::swprintf(
            versions.data(),
            versions.size(),
            L"Installed file: xScal %hhu.%hhu.%hhu, revision %u\n"
            L"New file: xScal %hhu.%hhu.%hhu, revision %u\n\n",
            installed->release.major,
            installed->release.minor,
            installed->release.patch,
            installed->revision,
            release.major,
            release.minor,
            release.patch,
            revision);
    }
    std::array<wchar_t, 1024> message{};
    std::swprintf(
        message.data(),
        message.size(),
        L"A new xScal Address Library was found for Fallout 76 "
        L"%hs (%hs).\n\n"
        L"%ls"
        L"The new file is used after the next restart of Fallout 76.\n\n"
        L"Do you want to install it now?",
        runtimeText.data(),
        TargetKindName(target),
        versions.data());
    if (platform.messageBox(
            nullptr,
            message.data(),
            L"xScal Address Library",
            MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        DiagnosticLog("Address Library: installation declined");
        if (installed != nullptr &&
            !address::StoreDeclinedUpdate(target, runtime, release, revision)) {
            DiagnosticLogFormat(
                "Address Library: decline marker could not be written "
                "(Windows error %lu)",
                ::GetLastError());
        }
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
    return AddressLibraryUpdateStatus::Installed;
}

void ShowRestartNotice(const RuntimeProcessPlatform& platform) noexcept {
    platform.messageBox(
        nullptr,
        L"The new xScal Address Library was installed.\n\n"
        L"Restart Fallout 76 manually to activate xScal for this game version.",
        L"xScal Address Library",
        MB_OK | MB_ICONINFORMATION);
}

// Offers the server's database when none is installed for this runtime or
// the installed file cannot be used.
[[nodiscard]] AddressLibraryUpdateStatus UpdateMissingAddressLibrary(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable,
    std::uintptr_t moduleBase) noexcept {
    const auto status = OfferAddressLibraryDownload(
        platform,
        target,
        runtime,
        executable,
        moduleBase,
        nullptr);
    if (status == AddressLibraryUpdateStatus::Installed) {
        ShowRestartNotice(platform);
    }
    return status;
}

// Asks the server for a newer database than the installed one, at most once
// per 24 hours, and right away after xScal itself was updated. A newly
// installed file is used after the next restart.
[[nodiscard]] AddressLibraryUpdateStatus UpdateInstalledAddressLibrary(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable,
    std::uintptr_t moduleBase,
    const InstalledFile& installed) noexcept {
    const std::uint64_t now = CurrentFileTime(platform);
    if (!address::IsUpdateCheckDue(target, runtime, now)) {
        const auto runtimeText = FormatRuntimeVersion(runtime);
        DiagnosticLogFormat(
            "Address Library: update check skipped for runtime %s (%s), "
            "last check was less than 24 hours ago",
            runtimeText.data(),
            TargetKindName(target));
        return AddressLibraryUpdateStatus::Unavailable;
    }

    const auto status = OfferAddressLibraryDownload(
        platform,
        target,
        runtime,
        executable,
        moduleBase,
        &installed);
    if (!address::StoreUpdateCheck(target, runtime, now)) {
        DiagnosticLogFormat(
            "Address Library: update check marker could not be written "
            "(Windows error %lu)",
            ::GetLastError());
    }
    if (status == AddressLibraryUpdateStatus::Installed) {
        ShowRestartNotice(platform);
    }
    return status;
}

// Everything the background thread needs, copied at startup. The thread
// does not touch activeAddressLibrary.
struct BackgroundJob final {
    RuntimeProcessPlatform platform;
    TargetKind target;
    config::RuntimeVersion runtime;
    address::ExecutableIdentity executable;
    std::uintptr_t moduleBase;
    bool autoUpdate;
    bool versionUnreadable;
    address::LoadStatus status;
    InstalledFile installed;
    // False when the file loaded and has every ID.
    bool showError;
};

// Runs on the background thread: the error boxes, the download offers and
// the restart notice.
void RunBackgroundJob(void* context) noexcept {
    std::unique_ptr<BackgroundJob> job{static_cast<BackgroundJob*>(context)};
    if (job->versionUnreadable) {
        ShowRuntimeVersionError(job->platform);
        return;
    }
    const bool fileMissing = job->status == address::LoadStatus::Missing;
    const bool fileUnusable =
        fileMissing ||
        job->status == address::LoadStatus::Invalid ||
        job->status == address::LoadStatus::BadSignature ||
        job->status == address::LoadStatus::WrongExecutable;
    if (job->autoUpdate) {
        auto updateStatus = AddressLibraryUpdateStatus::Unavailable;
        if (fileUnusable) {
            updateStatus = UpdateMissingAddressLibrary(
                job->platform, job->target, job->runtime, job->executable,
                job->moduleBase);
            if (updateStatus == AddressLibraryUpdateStatus::Declined && fileMissing) {
                return;
            }
        } else if (job->status == address::LoadStatus::Loaded) {
            updateStatus = UpdateInstalledAddressLibrary(
                job->platform, job->target, job->runtime, job->executable,
                job->moduleBase, job->installed);
        }
        if (updateStatus == AddressLibraryUpdateStatus::Installed) {
            return;
        }
    }
    if (job->showError) {
        ShowAddressLibraryError(job->platform, job->runtime);
    }
}

// Starts the background thread for job. Takes ownership of job.
void StartBackgroundJob(
    const RuntimeProcessPlatform& platform,
    std::unique_ptr<BackgroundJob> job) noexcept {
    if (!platform.runInBackground(&RunBackgroundJob, job.get())) {
        DiagnosticLog("Address Library: update thread could not be started");
        return;
    }
    (void)job.release();
}

bool RunOnNewThread(void (*work)(void* context) noexcept, void* context) noexcept {
    struct Start final {
        void (*work)(void*) noexcept;
        void* context;
    };
    auto* const start = new (std::nothrow) Start{work, context};
    if (start == nullptr) {
        return false;
    }
    const HANDLE thread = ::CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
            std::unique_ptr<Start> start{static_cast<Start*>(parameter)};
            start->work(start->context);
            return 0;
        },
        start, 0, nullptr);
    if (thread == nullptr) {
        delete start;
        return false;
    }
    (void)::CloseHandle(thread);
    return true;
}
}


RuntimeProcessPlatform SystemRuntimeProcessPlatform() noexcept {
    return {
        &::GetModuleFileNameW, &::GetModuleHandleW,
        &ReadSystemRuntimeVersion, &::MessageBoxW,
        &address::DownloadAddressLibrary,
        &address::ReadExecutableIdentity,
        &::GetSystemTimeAsFileTime,
        &RunOnNewThread,
    };
}

Runtime::Runtime(VtableHookPlatform platform) noexcept
    : statsEffectsProvider_(platform.virtualQuery),
      playerMapPositionProvider_(platform.virtualQuery),
      pipBoyProviders_(platform.virtualQuery),
      callbackRouter_(
          fileCommandService_,
          inputState_,
          statsEffectsProvider_,
          playerMapPositionProvider_,
          pipBoyProviders_),
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
        const bool playerMapPositionInstalled =
            playerMapPositionProvider_.Install(profile, moduleBase);
        pipBoyProviders_.Install(profile, moduleBase);
        DiagnosticLogFormat(
            "xScal Providers: {StatsEffectsProvider: %s, TimeDataProvider: %s, PlayerMapPositionProvider: %s, PipBoySpecialProvider: %s, PipBoyStatusProvider: %s, PipBoyPlayerInfoProvider: %s, PipBoyPerksProvider: %s, PipBoyQuestsProvider: %s, PipBoyInventoryProvider: %s, PipBoyCollectionsProvider: %s}",
            statsEffectsInstalled ? "Enabled" : "Disabled",
            pipBoyProviders_.time.IsActive() ? "Enabled" : "Disabled",
            playerMapPositionInstalled ? "Enabled" : "Disabled",
            pipBoyProviders_.special.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.status.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.playerInfo.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.perks.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.quests.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.inventory.IsActive() ? "Enabled" : "Disabled",
            pipBoyProviders_.collections.IsActive() ? "Enabled" : "Disabled");
        (void)archive::InstallArchiveStreamHook(profile.archiveStream, moduleBase);
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
        platform.downloadAddressLibrary == nullptr ||
        platform.readExecutableIdentity == nullptr ||
        platform.getSystemTimeAsFileTime == nullptr ||
        platform.runInBackground == nullptr) {
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

    auto job = std::make_unique<BackgroundJob>();
    job->platform = platform;
    job->target = target->kind;
    job->moduleBase = base;
    job->autoUpdate = config::GetRuntimeConfig().addressLibrary.autoUpdate;

    if (!platform.readRuntimeVersion(path.data(), found)) {
        LogRuntimeConfiguration(*target, base, nullptr);
        job->versionUnreadable = true;
        StartBackgroundJob(platform, std::move(job));
        return VtableHookStatus::RuntimeVersionUnavailable;
    }
    address::ExecutableIdentity executable{};
    if (!platform.readExecutableIdentity(base, executable)) {
        LogRuntimeConfiguration(*target, base, &found);
        DiagnosticLog("Address Library: could not read the executable's PE headers");
        return VtableHookStatus::InvalidArgument;
    }
    plugins::SetAddressLibrary(nullptr);
    const auto addressStatus = activeAddressLibrary.Load(
        target->kind, found, executable, base);
    job->runtime = found;
    job->executable = executable;
    job->status = addressStatus;
    if (addressStatus == address::LoadStatus::Loaded) {
        job->installed = {
            activeAddressLibrary.ReleaseXScalVersion(),
            activeAddressLibrary.Revision()};
    }
    if (addressStatus != address::LoadStatus::Loaded) {
        LogRuntimeConfiguration(*target, base, &found);
        const char* statusText = "invalid database";
        if (addressStatus == address::LoadStatus::Invalid &&
            !address::kHasLibraryKey) {
            DiagnosticLog(
                "Address Library: this build has no Address Library key, so it cannot read any database");
        }
        VtableHookStatus failure = VtableHookStatus::AddressLibraryInvalid;
        if (addressStatus == address::LoadStatus::Missing) {
            statusText = "file not found";
            failure = VtableHookStatus::AddressLibraryMissing;
        } else if (addressStatus == address::LoadStatus::BadSignature) {
            statusText = "signature check failed";
            LogAddressLibraryRejection(activeAddressLibrary, addressStatus, executable);
        } else if (addressStatus == address::LoadStatus::WrongExecutable) {
            statusText = "file is for another executable build";
            LogAddressLibraryRejection(activeAddressLibrary, addressStatus, executable);
        } else if (addressStatus == address::LoadStatus::ReadFailed) {
            statusText = "file could not be read";
        } else if (addressStatus == address::LoadStatus::RequiresNewerXScal) {
            statusText = "newer xScal version required";
            failure = VtableHookStatus::AddressLibraryRequiresNewerXScal;
            const auto required = activeAddressLibrary.RequiredXScalVersion();
            DiagnosticLogFormat(
                "Address Library requires xScal %hhu.%hhu.%hhu or newer",
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
        job->showError = true;
        StartBackgroundJob(platform, std::move(job));
        return failure;
    }

    TargetProfile profile{};
    const auto* const addressInterface = activeAddressLibrary.Interface();
    if (addressInterface == nullptr ||
        !ReadTargetProfile(*target, found, *addressInterface, profile)) {
        LogRuntimeConfiguration(*target, base, &found);
        DiagnosticLog("Address Library: required xScal IDs are missing");
        job->showError = true;
        StartBackgroundJob(platform, std::move(job));
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
    StartBackgroundJob(platform, std::move(job));
    return status;
}
}
