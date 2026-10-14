#include "api/file_command_service.hpp"

#include "platform/diagnostics.hpp"
#include "platform/win32_file.hpp"

#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace sf {
namespace {

constexpr const char kRelativeFileLabel[] = "modstorage";

struct CommandTarget final {
    const char* callbackName;
    const wchar_t* relativePath;
    bool append;
};

// Indexed by FileCommand. Append targets reject empty payloads; overwrite
// targets accept them and truncate the file.
constexpr CommandTarget kCommandTargets[] = {
    {"writeBuffDataFile", L"Data\\BuffData.ini", false},
    {"writeChallengeDataFile", L"Data\\ChallengeData.ini", false},
    {"writePerksFile", L"Data\\perkloadoutmanager.ini", false},
    {"writeSaveEverythingFile", L"Data\\saveeverything.ini", false},
    {"writeItemsModFile", L"Data\\itemsmod.ini", false},
    {"writeLegendaryModsFile", L"Data\\LegendaryMods.ini", false},
    {"writeVendorLogFile", L"Data\\vendorlog.txt", true},
    {"writeDPSMeterFile", L"Data\\DPSMeter.txt", false},
    {"writeCampSearchConfigFile", L"Data\\CAMPSearch.ini", false},
    {"writeBlockFile", L"Data\\configuration\\blocklist.ini", false},
    {"writeCampDataFile", L"Data\\CampData.ini", false},
    {"writeCharacterDataFile", L"Data\\CharacterData.ini", false},
};
static_assert(
    std::size(kCommandTargets) ==
    static_cast<std::size_t>(FileCommand::WriteCharacterDataFile) + 1);

[[nodiscard]] const CommandTarget* FindCommandTarget(
    FileCommand command) noexcept {
    const auto index = static_cast<std::size_t>(command);
    if (index >= std::size(kCommandTargets)) {
        return nullptr;
    }
    return &kCommandTargets[index];
}

// Rejects oversized content, NUL, and control characters other than CR, LF,
// and TAB.
[[nodiscard]] bool IsValidFileContent(std::string_view content) noexcept {
    if (content.size() > FileCommandService::kMaximumPayloadBytes) {
        return false;
    }
    for (const unsigned char character : content) {
        if (character == '\0') {
            return false;
        }
        if (character < 0x20 && character != '\r' && character != '\n' &&
            character != '\t') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsValidPayload(
    const CommandTarget& target,
    std::string_view payload) noexcept {
    if (target.append && payload.empty()) {
        return false;
    }
    return IsValidFileContent(payload);
}

[[nodiscard]] bool WriteRelativeFile(
    const char* callbackName,
    const wchar_t* relativePath,
    std::string_view data) noexcept {
    platform::ExecutableRelativePath targetPath;
    if (!platform::ResolveExecutableRelativePath(relativePath, targetPath)) {
        return false;
    }

    if (!platform::WriteFileAtomically(targetPath, data)) {
        return false;
    }
    DiagnosticLogFormat(
        "Scaleform write %s: completed (bytes=%zu)",
        callbackName,
        data.size());
    return true;
}

[[nodiscard]] bool AppendRelativeFile(
    const char* callbackName,
    const wchar_t* relativePath,
    std::string_view data) noexcept {
    platform::ExecutableRelativePath targetPath;
    if (!platform::ResolveExecutableRelativePath(relativePath, targetPath)) {
        return false;
    }

    try {
        std::string combined;
        const auto presence = platform::QueryFilePresence(targetPath);
        if (presence == platform::FilePresence::Error) {
            return false;
        }
        if (presence == platform::FilePresence::Present &&
            !platform::ReadBoundedFile(
                targetPath,
                (std::numeric_limits<std::size_t>::max)(),
                combined)) {
            return false;
        }
        if (data.size() > combined.max_size() - combined.size()) {
            return false;
        }
        combined.append(data);

        if (!platform::WriteFileAtomically(targetPath, combined)) {
            return false;
        }
        DiagnosticLogFormat(
            "Scaleform append %s: completed (added=%zu, total=%zu)",
            callbackName,
            data.size(),
            combined.size());
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

FileCommandService::~FileCommandService() {
    Stop();
}

bool FileCommandService::Start() noexcept {
    std::scoped_lock lifecycleLock{lifecycleMutex_};
    std::unique_lock lock{mutex_};
    if (state_ == State::Running) {
        return true;
    }
    if (state_ == State::Stopping || worker_.joinable()) {
        return false;
    }

    state_ = State::Running;
    try {
        worker_ = std::thread(&FileCommandService::WorkerMain, this);
    } catch (...) {
        state_ = State::Stopped;
        return false;
    }
    lock.unlock();
    SetDiagnosticFileService(this);
    workAvailable_.notify_one();
    return true;
}

void FileCommandService::Stop() noexcept {
    std::scoped_lock lifecycleLock{lifecycleMutex_};
    {
        std::scoped_lock lock{mutex_};
        if (state_ == State::Running) {
            state_ = State::Stopping;
        }
    }
    workAvailable_.notify_all();

    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            return;
        }
        worker_.join();
    }

    ClearDiagnosticFileService(this);

    {
        std::scoped_lock lock{mutex_};
        state_ = State::Stopped;
    }
    idle_.notify_all();
}

FileCommandSubmitStatus FileCommandService::Submit(
    FileCommand command,
    std::string_view payload) noexcept {
    const CommandTarget* target = FindCommandTarget(command);
    if (target == nullptr || !IsValidPayload(*target, payload)) {
        return FileCommandSubmitStatus::InvalidPayload;
    }
    return QueueJob(
        target->callbackName,
        target->relativePath,
        target->append,
        payload);
}

FileCommandSubmitStatus FileCommandService::SubmitRelativeFileWrite(
    std::wstring_view relativePath,
    std::string_view payload) noexcept {
    if (!IsValidFileContent(payload)) {
        return FileCommandSubmitStatus::InvalidPayload;
    }
    return QueueJob(kRelativeFileLabel, relativePath, false, payload);
}

FileCommandSubmitStatus FileCommandService::QueueJob(
    const char* label,
    std::wstring_view relativePath,
    bool append,
    std::string_view payload) noexcept {
    Job job;
    job.label = label;
    job.append = append;
    try {
        job.relativePath.assign(relativePath);
        job.payload.assign(payload);
    } catch (...) {
        return FileCommandSubmitStatus::QueueFull;
    }

    {
        std::scoped_lock lock{mutex_};
        if (state_ != State::Running) {
            return FileCommandSubmitStatus::NotRunning;
        }
        if (jobs_.size() >= kMaximumQueuedCommands ||
            job.payload.size() >
                kMaximumQueuedPayloadBytes - queuedPayloadBytes_) {
            return FileCommandSubmitStatus::QueueFull;
        }
        const std::size_t payloadSize = job.payload.size();
        try {
            jobs_.push_back(std::move(job));
        } catch (...) {
            return FileCommandSubmitStatus::QueueFull;
        }
        queuedPayloadBytes_ += payloadSize;
    }

    workAvailable_.notify_one();
    return FileCommandSubmitStatus::Accepted;
}

bool FileCommandService::SubmitDiagnosticLine(
    std::string_view line) noexcept {
    if (line.empty() || line.size() > kMaximumDiagnosticLineBytes) {
        return false;
    }

    std::unique_lock lock{mutex_, std::defer_lock};
    if (!lock.try_lock()) {
        return false;
    }
    if (state_ != State::Running ||
        diagnosticLineCount_ == kMaximumQueuedDiagnosticLines) {
        return false;
    }

    DiagnosticLine& target = diagnosticLines_[diagnosticWriteIndex_];
    std::memcpy(target.bytes.data(), line.data(), line.size());
    target.length = static_cast<std::uint16_t>(line.size());
    diagnosticWriteIndex_ =
        (diagnosticWriteIndex_ + 1) % kMaximumQueuedDiagnosticLines;
    ++diagnosticLineCount_;
    lock.unlock();
    workAvailable_.notify_one();
    return true;
}

void FileCommandService::WaitUntilIdle() noexcept {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] {
        return jobs_.empty() && diagnosticLineCount_ == 0 && !workActive_;
    });
}

bool FileCommandService::IsWriteQueued(
    std::wstring_view relativePath) noexcept {
    std::scoped_lock lock{mutex_};
    if (activeJob_ != nullptr && activeJob_->relativePath == relativePath) {
        return true;
    }
    for (const Job& job : jobs_) {
        if (job.relativePath == relativePath) {
            return true;
        }
    }
    return false;
}

void FileCommandService::WorkerMain() noexcept {
    for (;;) {
        Job job;
        DiagnosticLine diagnosticLine;
        bool executeFileCommand{};
        {
            std::unique_lock lock{mutex_};
            workAvailable_.wait(lock, [this] {
                return !jobs_.empty() || diagnosticLineCount_ != 0 ||
                    state_ != State::Running;
            });
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                queuedPayloadBytes_ -= job.payload.size();
                activeJob_ = &job;
                executeFileCommand = true;
            } else if (diagnosticLineCount_ != 0) {
                diagnosticLine = diagnosticLines_[diagnosticReadIndex_];
                diagnosticReadIndex_ =
                    (diagnosticReadIndex_ + 1) %
                    kMaximumQueuedDiagnosticLines;
                --diagnosticLineCount_;
            } else {
                if (state_ != State::Running) {
                    break;
                }
                continue;
            }
            workActive_ = true;
        }

        if (executeFileCommand) {
            Execute(job);
        } else {
            WriteDiagnosticLineToFile(std::string_view{
                diagnosticLine.bytes.data(),
                diagnosticLine.length});
        }

        {
            std::scoped_lock lock{mutex_};
            workActive_ = false;
            activeJob_ = nullptr;
            if (jobs_.empty() && diagnosticLineCount_ == 0) {
                idle_.notify_all();
            }
        }
    }

    {
        std::scoped_lock lock{mutex_};
        workActive_ = false;
    }
    idle_.notify_all();
}

void FileCommandService::Execute(const Job& job) noexcept {
    if (job.append) {
        (void)AppendRelativeFile(
            job.label, job.relativePath.c_str(), job.payload);
    } else {
        (void)WriteRelativeFile(
            job.label, job.relativePath.c_str(), job.payload);
    }
}

}  // namespace sf
