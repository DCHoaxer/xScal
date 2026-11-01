#include "runtime/runtime.hpp"

#include <libdeflate.h>
#include <yyjson.h>

#include "address/address_library.hpp"
#include "address/address_library_key.hpp"
#include "address/address_library_v3.hpp"
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
#include <string>
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

// Displays a message box indicating that the Address Library is missing or
// invalid. The dialog is the base for addlib.json messages: explanation (the
// block message, or the message of the build's event) replaces everything
// between the runtime line and the closing line; without one, the dialog
// keeps its own "What went wrong?" paragraph. The Always message comes
// before the closing line.
void ShowAddressLibraryError(
    const RuntimeProcessPlatform& platform,
    const config::RuntimeVersion& found,
    const address::v3::Message* explanation = nullptr,
    const address::v3::Message* always = nullptr) noexcept {
    const auto runtimeText = FormatRuntimeVersion(found);
    std::array<wchar_t, 64> runtime{};
    std::swprintf(runtime.data(), runtime.size(), L"%hs", runtimeText.data());
    UINT icon{};
    std::wstring serverText;
    if (explanation != nullptr && !address::v3::BuildMessageText(*explanation, {}, serverText, icon)) {
        serverText.clear();
    }
    std::wstring alwaysText;
    if (always != nullptr && !address::v3::BuildMessageText(*always, {}, alwaysText, icon)) {
        alwaysText.clear();
    }
    const wchar_t* const defaultText =
        L"What went wrong?\n"
        L"xScal tried to read the Address Library and immediately regretted being born. "
        L"Missing or invalid Address Library for this runtime.";
    try {
        const std::wstring message =
            L"Oh no. The Address Library has flatlined.\n\n"
            L"Fallout 76 runtime: " + std::wstring(runtime.data()) + L"\n\n" +
            (serverText.empty() ? std::wstring(defaultText) : serverText) + L"\n\n" +
            (alwaysText.empty() ? std::wstring() : alwaysText + L"\n\n") +
            L"xScal features are now taking an unpaid vacation for this session.";
        platform.messageBox(nullptr, message.c_str(), L"xScal", MB_OK | MB_ICONEXCLAMATION);
    } catch (...) {
        platform.messageBox(
            nullptr,
            L"Oh no. The Address Library has flatlined.\n\n"
            L"xScal features are now taking an unpaid vacation for this session.",
            L"xScal",
            MB_OK | MB_ICONEXCLAMATION);
    }
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

// Downloads addlib.json and stores each part that verifies and has a higher
// sequence than the cached one. status gets the newly accepted status, or
// the cached one when the download has nothing newer. False when there is
// no usable status.
[[nodiscard]] bool ReadServerStatus(
    const RuntimeProcessPlatform& platform,
    address::v3::Status& status) noexcept {
    namespace v3 = address::v3;
    std::string text;
    if (!platform.downloadAddressLibrary(v3::kAddlibPath.data(), text)) {
        DiagnosticLog("Address Library: addlib.json could not be downloaded");
        return false;
    }
    v3::RawFile file;
    const char* detail = nullptr;
    if (!v3::SplitFile(text, file, detail)) {
        DiagnosticLogFormat("Address Library: addlib.json rejected (%s)", detail);
        text.clear();
    }

    v3::Status cached;
    const bool hasCached = v3::LoadCachedStatus(cached);
    v3::Revocations cachedRevocations;
    const bool hasCachedRevocations = v3::LoadCachedRevocations(cachedRevocations);

    if (file.hasRevocations) {
        v3::Revocations fresh;
        const auto check = v3::CheckRevocationsPart(
            file.revocations,
            hasCachedRevocations ? cachedRevocations.sequence : 0,
            fresh,
            detail);
        if (check == v3::PartCheck::Accepted) {
            if (v3::StoreRevocationsPart(file.revocations)) {
                DiagnosticLogFormat(
                    "Address Library: revocations sequence %llu accepted, used from the next launch",
                    static_cast<unsigned long long>(fresh.sequence));
            } else {
                DiagnosticLogFormat(
                    "Address Library: v3-revocations.json could not be written (Windows error %lu)",
                    ::GetLastError());
            }
        } else if (check == v3::PartCheck::NotNewer) {
            DiagnosticLogFormat(
                "Address Library: revocations sequence %llu is not newer than the cached %llu",
                static_cast<unsigned long long>(fresh.sequence),
                static_cast<unsigned long long>(
                    hasCachedRevocations ? cachedRevocations.sequence : 0));
        } else {
            DiagnosticLogFormat("Address Library: revocations part rejected (%s)", detail);
        }
    } else if (file.revocationsDetail != nullptr && !text.empty()) {
        DiagnosticLogFormat(
            "Address Library: revocations part rejected (%s)", file.revocationsDetail);
    }

    if (file.hasStatus) {
        v3::Status fresh;
        const auto check = v3::CheckStatusPart(
            file.status, hasCached ? cached.sequence : 0, fresh, detail);
        if (check == v3::PartCheck::Accepted) {
            if (!v3::StoreStatusPart(file.status)) {
                DiagnosticLogFormat(
                    "Address Library: v3-status.json could not be written (Windows error %lu)",
                    ::GetLastError());
            }
            DiagnosticLogFormat(
                "Address Library: status sequence %llu accepted",
                static_cast<unsigned long long>(fresh.sequence));
            status = std::move(fresh);
            return true;
        }
        if (check == v3::PartCheck::NotNewer) {
            DiagnosticLogFormat(
                "Address Library: status sequence %llu is not newer than the cached %llu",
                static_cast<unsigned long long>(fresh.sequence),
                static_cast<unsigned long long>(hasCached ? cached.sequence : 0));
        } else {
            DiagnosticLogFormat("Address Library: status part rejected (%s)", detail);
        }
    } else if (!text.empty()) {
        DiagnosticLogFormat("Address Library: status part rejected (%s)", file.statusDetail);
    }
    if (!hasCached) {
        DiagnosticLog("Address Library: no accepted status, nothing to offer");
        return false;
    }
    status = std::move(cached);
    return true;
}

// Reads addlib.json and writes the database it names after the user agrees.
// With installed set, the database is offered only when its release and
// revision are newer than the installed file's and the user has not declined
// that file.
[[nodiscard]] AddressLibraryUpdateStatus OfferAddressLibraryDownload(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable,
    std::uintptr_t moduleBase,
    const InstalledFile* installed) noexcept {
    namespace v3 = address::v3;
    const auto runtimeText = FormatRuntimeVersion(runtime);
    v3::Status status;
    if (!ReadServerStatus(platform, status)) {
        DiagnosticLogFormat(
            "Address Library: no download available for runtime %s (%s)",
            runtimeText.data(),
            TargetKindName(target));
        return AddressLibraryUpdateStatus::Unavailable;
    }
    const v3::Build* const build = v3::FindBuild(status, target, runtime, executable);
    if (build == nullptr) {
        DiagnosticLogFormat(
            "Address Library: status %llu has no entry for runtime %s (%s), "
            "timestamp 0x%08X size 0x%08X",
            static_cast<unsigned long long>(status.sequence),
            runtimeText.data(),
            TargetKindName(target),
            executable.timeDateStamp,
            executable.imageSize);
        return AddressLibraryUpdateStatus::Unavailable;
    }

    // A file that the new status blocks is replaced by any database.
    if (installed != nullptr) {
        const auto* const blocked = v3::FindBlockedRevision(
            *build, installed->release, installed->revision);
        if (blocked != nullptr) {
            const auto* const blockMessage =
                v3::FindMessage(status.messages, blocked->messageId);
            DiagnosticLogFormat(
                "Address Library: installed revision %u (xScal %hhu.%hhu.%hhu) is blocked (message %s)",
                installed->revision,
                installed->release.major,
                installed->release.minor,
                installed->release.patch,
                blockMessage != nullptr ? blockMessage->id.c_str() : "no message");
            // The file stays in use until the game is restarted; the next
            // launch refuses it and shows the reason in the flatline dialog.
            installed = nullptr;
        }
    }
    if (build->status != v3::BuildStatus::Ok) {
        static constexpr const char* kNames[] = {"ok", "held", "unavailable", "retired"};
        DiagnosticLogFormat(
            "Address Library: runtime %s (%s) is %s, nothing to download",
            runtimeText.data(),
            TargetKindName(target),
            kNames[static_cast<int>(build->status)]);
        return AddressLibraryUpdateStatus::Unavailable;
    }
    constexpr address::format::XScalVersion runningXScal{
        config::kXScalVersion.major,
        config::kXScalVersion.minor,
        config::kXScalVersion.patch};
    const v3::DatabaseEntry* selected = v3::FindDatabaseFor(*build, runningXScal);
    if (selected == nullptr) {
        DiagnosticLogFormat(
            "Address Library: status ok for runtime %s (%s) but the entry has no database for this xScal version",
            runtimeText.data(),
            TargetKindName(target));
        return AddressLibraryUpdateStatus::Unavailable;
    }
    const v3::DatabaseEntry& entry = *selected;
    if (installed != nullptr) {
        if (!address::format::IsFileNewer(
                entry.release, entry.revision, installed->release, installed->revision)) {
            DiagnosticLogFormat(
                "Address Library: server has no newer file for runtime %s (%s)",
                runtimeText.data(),
                TargetKindName(target));
            return AddressLibraryUpdateStatus::Unavailable;
        }
        if (address::IsUpdateDeclined(target, runtime, entry.release, entry.revision)) {
            DiagnosticLogFormat(
                "Address Library: server file %hhu.%hhu.%hhu revision %u was declined earlier",
                entry.release.major,
                entry.release.minor,
                entry.release.patch,
                entry.revision);
            return AddressLibraryUpdateStatus::Unavailable;
        }
    }
    if (const auto* const blocked = v3::FindBlockedRevision(
            *build, entry.release, entry.revision)) {
        const auto* const blockMessage = v3::FindMessage(status.messages, blocked->messageId);
        DiagnosticLogFormat(
            "Address Library: server file %hhu.%hhu.%hhu revision %u is blocked (message %s)",
            entry.release.major,
            entry.release.minor,
            entry.release.patch,
            entry.revision,
            blockMessage != nullptr ? blockMessage->id.c_str() : "no message");
        return AddressLibraryUpdateStatus::Unavailable;
    }

    std::string contents;
    if (!platform.downloadAddressLibrary(entry.url.c_str(), contents)) {
        DiagnosticLogFormat(
            "Address Library: %s could not be downloaded", entry.url.c_str());
        return AddressLibraryUpdateStatus::Unavailable;
    }
    plugins::signing::Hash hash{};
    if (contents.size() != entry.size) {
        DiagnosticLogFormat(
            "Address Library: downloaded file has %zu bytes, the status says %u",
            contents.size(),
            entry.size);
        return AddressLibraryUpdateStatus::Unavailable;
    }
    if (plugins::signing::HashBytes(contents.data(), contents.size(), hash) < 0 ||
        hash != entry.sha256) {
        DiagnosticLog("Address Library: downloaded file SHA-256 does not match the status");
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
    if (address::format::EncodeXScalVersion(release) !=
            address::format::EncodeXScalVersion(entry.release) ||
        revision != entry.revision) {
        DiagnosticLogFormat(
            "Address Library: downloaded file is %hhu.%hhu.%hhu revision %u, "
            "the status says %hhu.%hhu.%hhu revision %u",
            release.major, release.minor, release.patch, revision,
            entry.release.major, entry.release.minor, entry.release.patch, entry.revision);
        return AddressLibraryUpdateStatus::Unavailable;
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
// per checkAgainHours of the build entry (24 without one), and right away after xScal itself was updated. A newly
// installed file is used after the next restart.
[[nodiscard]] AddressLibraryUpdateStatus UpdateInstalledAddressLibrary(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable,
    std::uintptr_t moduleBase,
    const InstalledFile& installed) noexcept {
    const std::uint64_t now = CurrentFileTime(platform);
    const std::uint32_t intervalHours =
        address::v3::CachedCheckHours(target, runtime, executable);
    if (!address::IsUpdateCheckDue(target, runtime, now, intervalHours)) {
        const auto runtimeText = FormatRuntimeVersion(runtime);
        DiagnosticLogFormat(
            "Address Library: update check skipped for runtime %s (%s), "
            "last check was less than %u hours ago",
            runtimeText.data(),
            TargetKindName(target),
            intervalHours);
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
    // The installed file is listed in blockedRevisions. blockedMessage is
    // set when the entry names a message.
    bool blocked;
    bool hasBlockedMessage;
    address::v3::Message blockedMessage;
};

// The messages of the cached status, which the daily check has just
// refreshed, that apply to this xScal version now.
struct StatusMessages final {
    // For the flatline dialog: the first of the build's messageIds, else the
    // first of missingMessageIds (also for builds the status does not list).
    bool hasEvent{};
    address::v3::Message event;
    // The first of the build's alwaysMessageIds, or of the status's list for
    // builds it does not list.
    bool hasAlways{};
    address::v3::Message always;
};

[[nodiscard]] StatusMessages ReadStatusMessages(
    const RuntimeProcessPlatform& platform,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const address::ExecutableIdentity& executable) noexcept {
    StatusMessages found;
    try {
        address::v3::Status status;
        if (!address::v3::LoadCachedStatus(status)) {
            return found;
        }
        const address::format::XScalVersion running{
            config::kXScalVersion.major, config::kXScalVersion.minor, config::kXScalVersion.patch};
        const auto now = CurrentFileTime(platform);
        const auto first = [&](const std::vector<std::string>& ids, address::v3::Message& message) {
            for (const auto& id : ids) {
                const auto* const candidate = address::v3::FindMessage(status.messages, id);
                if (candidate != nullptr && address::v3::IsMessageCurrent(*candidate, running, now)) {
                    message = *candidate;
                    return true;
                }
            }
            return false;
        };
        const auto* const build = address::v3::FindBuild(status, target, runtime, executable);
        found.hasEvent = (build != nullptr && first(build->messageIds, found.event)) ||
            first(status.missingMessageIds, found.event);
        found.hasAlways = first(build != nullptr ? build->alwaysMessageIds : status.alwaysMessageIds, found.always);
    } catch (...) {
        found = {};
    }
    return found;
}

// The update offers, the restart notice and the flatline dialog. Returns
// true when the flatline dialog was shown; it already carries the Always
// message then.
[[nodiscard]] bool RunAddressLibraryJob(BackgroundJob& job) noexcept {
    const bool fileMissing = job.status == address::LoadStatus::Missing;
    const bool fileUnusable =
        fileMissing ||
        job.status == address::LoadStatus::Invalid ||
        job.status == address::LoadStatus::BadSignature ||
        job.status == address::LoadStatus::WrongExecutable;
    if (job.autoUpdate) {
        auto updateStatus = AddressLibraryUpdateStatus::Unavailable;
        if (fileUnusable) {
            updateStatus = UpdateMissingAddressLibrary(
                job.platform, job.target, job.runtime, job.executable,
                job.moduleBase);
            if (updateStatus == AddressLibraryUpdateStatus::Declined && fileMissing) {
                return false;
            }
        } else if (job.status == address::LoadStatus::Loaded) {
            updateStatus = UpdateInstalledAddressLibrary(
                job.platform, job.target, job.runtime, job.executable,
                job.moduleBase, job.installed);
        }
        if (updateStatus == AddressLibraryUpdateStatus::Installed) {
            return false;
        }
    }
    if (!job.showError) {
        return false;
    }
    // One box: the flatline dialog, with the block reason or the event
    // message, and the Always message.
    const StatusMessages messages = ReadStatusMessages(job.platform, job.target, job.runtime, job.executable);
    ShowAddressLibraryError(
        job.platform,
        job.runtime,
        job.hasBlockedMessage ? &job.blockedMessage : (messages.hasEvent ? &messages.event : nullptr),
        messages.hasAlways ? &messages.always : nullptr);
    return true;
}

// Runs on the background thread: the error boxes, the download offers, the
// restart notice and the Always message.
void RunBackgroundJob(void* context) noexcept {
    std::unique_ptr<BackgroundJob> job{static_cast<BackgroundJob*>(context)};
    if (job->versionUnreadable) {
        ShowRuntimeVersionError(job->platform);
        return;
    }
    if (RunAddressLibraryJob(*job)) {
        return;
    }
    // Without the flatline dialog the Always message is a box of its own,
    // once per launch, after the daily check has refreshed the status.
    const StatusMessages messages = ReadStatusMessages(job->platform, job->target, job->runtime, job->executable);
    if (!messages.hasAlways) {
        return;
    }
    try {
        std::wstring text;
        UINT icon{};
        if (address::v3::BuildMessageText(messages.always, {}, text, icon)) {
            job->platform.messageBox(nullptr, text.c_str(), L"xScal", MB_OK | icon);
        }
    } catch (...) {
        DiagnosticLog("Address Library: Always message could not be shown (out of memory)");
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
        &address::DownloadAddressLibraryFile,
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
        if (gamepadFilterStatus == VtableHookStatus::Installed) {
            (void)gamepadInputFilter_.ForwardGamepads();
            if (profile.scePadBluetoothFlagOffset != 0) {
                (void)gamepadInputFilter_.EnableScePadBluetooth(
                    moduleBase + profile.scePadBluetoothFlagOffset);
            }
        }
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
    address::v3::ApplyCachedRevocations();
    auto addressStatus = activeAddressLibrary.Load(
        target->kind, found, executable, base);
    job->runtime = found;
    job->executable = executable;
    if (addressStatus == address::LoadStatus::Loaded) {
        // A blocked revision is treated like a missing file.
        address::v3::Status cachedStatus;
        const address::v3::Build* build = nullptr;
        if (address::v3::LoadCachedStatus(cachedStatus)) {
            build = address::v3::FindBuild(cachedStatus, target->kind, found, executable);
        }
        const auto release = activeAddressLibrary.ReleaseXScalVersion();
        const auto revision = activeAddressLibrary.Revision();
        const auto* const blocked = build != nullptr
            ? address::v3::FindBlockedRevision(*build, release, revision)
            : nullptr;
        if (blocked != nullptr) {
            const auto* const blockMessage =
                address::v3::FindMessage(cachedStatus.messages, blocked->messageId);
            DiagnosticLogFormat(
                "xScal Address Library version-%hu-%hu-%hu-%hu-%s.bin revision %u "
                "(xScal %hhu.%hhu.%hhu) is blocked (message %s)",
                found.major, found.minor, found.build, found.revision,
                TargetKindName(target->kind), revision,
                release.major, release.minor, release.patch,
                blockMessage != nullptr ? blockMessage->id.c_str() : "no message");
            job->blocked = true;
            if (blockMessage != nullptr) {
                job->blockedMessage = *blockMessage;
                job->hasBlockedMessage = true;
            }
            addressStatus = address::LoadStatus::Missing;
        }
    }
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
            statusText = job->blocked ? "revision blocked" : "file not found";
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
