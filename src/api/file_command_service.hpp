#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace sf {

enum class FileCommand : std::uint8_t {
    WriteBuffDataFile,
    WriteChallengeDataFile,
    WritePerksFile,
    WriteSaveEverythingFile,
    WriteItemsModFile,
    WriteLegendaryModsFile,
    WriteVendorLogFile,
    WriteDPSMeterFile,
    WriteCampSearchConfigFile,
    WriteBlockFile,
    WriteCampDataFile,
    WriteCharacterDataFile,
};

enum class FileCommandSubmitStatus : std::uint8_t {
    Accepted,
    NotRunning,
    InvalidPayload,
    QueueFull,
};

// Serializes file commands on one persistent worker. Submitted payloads are
// copied before Submit returns and never alias Scaleform-owned storage.
class FileCommandService final {
public:
    static constexpr std::size_t kMaximumQueuedCommands = 64;
    static constexpr std::size_t kMaximumPayloadBytes = 1024U * 1024U;
    static constexpr std::size_t kMaximumQueuedPayloadBytes =
        4U * 1024U * 1024U;

    FileCommandService() noexcept = default;
    ~FileCommandService();

    FileCommandService(const FileCommandService&) = delete;
    FileCommandService& operator=(const FileCommandService&) = delete;

    // Start is idempotent while running. A stopped service may be restarted.
    [[nodiscard]] bool Start() noexcept;

    // Rejects new commands, drains the FIFO, and joins the worker. Stop is
    // idempotent and must not be called from the worker thread.
    void Stop() noexcept;

    [[nodiscard]] FileCommandSubmitStatus Submit(
        FileCommand command,
        std::string_view payload) noexcept;

    // Overwrites a file at a path relative to the executable directory. The
    // path is copied, and an empty payload truncates the file.
    [[nodiscard]] FileCommandSubmitStatus SubmitRelativeFileWrite(
        std::wstring_view relativePath,
        std::string_view payload) noexcept;

    // Never waits for the queue mutex and never allocates.
    [[nodiscard]] bool SubmitDiagnosticLine(std::string_view line) noexcept;

    // Waits until no file command or diagnostic line is queued or executing.
    void WaitUntilIdle() noexcept;

    // Returns true while a write to relativePath is queued or executing.
    [[nodiscard]] bool IsWriteQueued(std::wstring_view relativePath) noexcept;

private:
    enum class State : std::uint8_t {
        Stopped,
        Running,
        Stopping,
    };

    struct Job final {
        const char* label{};
        std::wstring relativePath;
        std::string payload;
        bool append{};
    };

    static constexpr std::size_t kMaximumQueuedDiagnosticLines = 64;
    static constexpr std::size_t kMaximumDiagnosticLineBytes = 1024;

    struct DiagnosticLine final {
        std::array<char, kMaximumDiagnosticLineBytes> bytes{};
        std::uint16_t length{};
    };

    [[nodiscard]] FileCommandSubmitStatus QueueJob(
        const char* label,
        std::wstring_view relativePath,
        bool append,
        std::string_view payload) noexcept;
    void WorkerMain() noexcept;
    void Execute(const Job& job) noexcept;

    std::mutex lifecycleMutex_;
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    // The job WorkerMain is executing, or nullptr. Set and cleared under mutex_.
    const Job* activeJob_{};
    std::array<DiagnosticLine, kMaximumQueuedDiagnosticLines>
        diagnosticLines_{};
    std::size_t queuedPayloadBytes_{};
    std::size_t diagnosticReadIndex_{};
    std::size_t diagnosticWriteIndex_{};
    std::size_t diagnosticLineCount_{};
    bool workActive_{};
    State state_{State::Stopped};
    std::thread worker_;
};

}  // namespace sf
