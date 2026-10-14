#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sf {

class FileCommandService;
struct FunctionParams;

namespace modstorage {

inline constexpr std::size_t kMaximumDocumentSize = 40 * 1024;

[[nodiscard]] bool IsValidName(std::string_view name) noexcept;
[[nodiscard]] bool IsValidJsonDocument(std::string_view document) noexcept;

// Saves queue the file write on a FileCommandService worker and return once
// the write is queued, so the calling thread does not wait for disk I/O.
// While a saved document's write is queued or executing, Load returns it from
// memory. Otherwise Load reads the file, so edits made outside the game load.
class Service final {
public:
    // The writer must outlive its attachment. Saves fail while no writer is
    // attached or while the attached writer is not running.
    void AttachFileWriter(FileCommandService* writer) noexcept;

    [[nodiscard]] bool Register(void* movie, std::string_view name) noexcept;
    [[nodiscard]] bool Save(void* movie, std::string_view document) noexcept;
    // Named overloads are intentionally shared: any movie can read or write
    // the document of any valid name, whether or not it registered that name.
    // This lets movies share one document through a common name.
    [[nodiscard]] bool Save(
        std::string_view name,
        std::string_view document) noexcept;
    [[nodiscard]] bool Load(void* movie, std::string& document) noexcept;
    // See the note on the named Save overload.
    [[nodiscard]] bool Load(
        std::string_view name,
        std::string& document) noexcept;

    void ForgetMovie(void* movie) noexcept;
    void Clear() noexcept;

private:
    [[nodiscard]] bool QueueWriteLocked(
        std::string_view name,
        std::string_view document);
    [[nodiscard]] bool LoadLocked(std::string_view name, std::string& document);

    std::mutex mutex_;
    FileCommandService* fileWriter_{};
    std::unordered_map<void*, std::string> registrations_;
    // Latest saved document per storage name, capped at 256 names. Load uses an
    // entry only while its file write is queued. Entries are not evicted until
    // Clear().
    std::unordered_map<std::string, std::string> documents_;
};

[[nodiscard]] Service& GlobalService() noexcept;

// Scaleform handler objects are exactly 0x10 bytes (vtable, refcount, padding),
// so each operation needs its own vtable instead of a stored operation field.
class FunctionHandler final {
public:
    enum class Operation {
        Register,
        Save,
        Load,
    };

    explicit FunctionHandler(Operation operation) noexcept;

    [[nodiscard]] void* AsScaleformHandler() noexcept { return this; }

private:
    using HandlerVtableEntry = void(__fastcall*)();

    template <Operation kOperation>
    [[nodiscard]] static const HandlerVtableEntry* VtableFor() noexcept;
    [[nodiscard]] static void* __fastcall Identity(void* handler) noexcept;
    template <Operation kOperation>
    static void __fastcall Invoke(
        void* handler,
        const FunctionParams* params) noexcept;

    const HandlerVtableEntry* vtable_;
    volatile std::int32_t referenceCount_;
    std::uint32_t referenceCountPadding_;
};

class FunctionHandlers final {
public:
    FunctionHandlers() noexcept;

    [[nodiscard]] FunctionHandler& Register() noexcept { return register_; }
    [[nodiscard]] FunctionHandler& Save() noexcept { return save_; }
    [[nodiscard]] FunctionHandler& Load() noexcept { return load_; }

private:
    FunctionHandler register_;
    FunctionHandler save_;
    FunctionHandler load_;
};

[[nodiscard]] FunctionHandlers& GlobalFunctionHandlers() noexcept;

}  // namespace modstorage
}  // namespace sf
