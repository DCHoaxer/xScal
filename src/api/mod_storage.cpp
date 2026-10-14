#include "api/mod_storage.hpp"

#include "api/file_command_service.hpp"
#include "common/json.hpp"
#include "platform/win32_file.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/value.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <string>
#include <utility>

namespace sf::modstorage {
namespace {

constexpr std::string_view kReservedNames[] = {
    "con", "prn", "aux", "nul",
    "com1", "com2", "com3", "com4", "com5",
    "com6", "com7", "com8", "com9",
    "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
    "lpt6", "lpt7", "lpt8", "lpt9",
};
constexpr std::string_view kStoragePrefix = "Data\\modsdata\\";
constexpr std::string_view kStorageSuffix = ".json";
constexpr std::size_t kMaximumNameLength = 64;
constexpr std::size_t kMaximumJsonDepth = 64;
// Bounds the in-memory cache. A save under a new name is rejected once the
// cache holds this many names; saves under cached names still succeed.
constexpr std::size_t kMaximumCachedDocuments = 256;

[[nodiscard]] bool IsAllowedNameCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') ||
        value == '.' || value == '_' || value == '-';
}

[[nodiscard]] bool IsValidJson(std::string_view text) noexcept {
    json::Document document;
    return json::ParseDocument(
        text,
        {json::NumberSyntax::Any,
         kMaximumDocumentSize,
         kMaximumJsonDepth,
         false},
        document);
}

// Storage names are validated ASCII, so each byte widens to one wchar_t.
void AppendAsciiToWide(std::wstring& target, std::string_view text) {
    for (const char value : text) {
        target.push_back(static_cast<wchar_t>(value));
    }
}

[[nodiscard]] std::wstring StorageRelativePath(std::string_view name) {
    std::wstring relative;
    relative.reserve(
        kStoragePrefix.size() + name.size() + kStorageSuffix.size());
    AppendAsciiToWide(relative, kStoragePrefix);
    AppendAsciiToWide(relative, name);
    AppendAsciiToWide(relative, kStorageSuffix);
    return relative;
}

[[nodiscard]] bool LoadDocument(
    std::string_view name,
    std::string& document) noexcept {
    try {
        const std::wstring relative = StorageRelativePath(name);
        platform::ExecutableRelativePath path;
        if (!platform::ResolveExecutableRelativePath(relative.c_str(), path) ||
            !platform::ReadBoundedFile(path, kMaximumDocumentSize, document) ||
            !IsValidJson(document)) {
            document.clear();
            return false;
        }
        return true;
    } catch (...) {
        document.clear();
        return false;
    }
}

[[nodiscard]] ScaleformCall MakeCall(const FunctionParams& params) noexcept {
    return {
        params.arguments,
        params.argumentCount,
        nullptr,
        params.movie,
        params.thisObject,
        params.argumentFrame,
    };
}

void InvokeOperation(
    FunctionHandler::Operation operation,
    const FunctionParams* params) noexcept {
    if (params == nullptr || params->result == nullptr) {
        return;
    }

    ScaleformResult result;
    result.SetBoolean(false);
    const ScaleformCall call = MakeCall(*params);
    auto& service = GlobalService();

    if (operation == FunctionHandler::Operation::Register) {
        std::string_view name;
        if (params->argumentCount == 1 &&
            TryGetScaleformStringArgument(call, 0, name)) {
            result.SetBoolean(service.Register(params->movie, name));
        }
    } else if (operation == FunctionHandler::Operation::Save) {
        if (params->argumentCount == 1) {
            std::string_view document;
            if (TryGetScaleformStringArgument(call, 0, document)) {
                result.SetBoolean(service.Save(params->movie, document));
            }
        } else if (params->argumentCount == 2) {
            std::string_view name;
            std::string_view document;
            if (TryGetScaleformStringArgument(call, 0, name) &&
                TryGetScaleformStringArgument(call, 1, document)) {
                result.SetBoolean(service.Save(name, document));
            }
        }
    } else {
        std::string document;
        if (params->argumentCount == 0 &&
            service.Load(params->movie, document)) {
            (void)result.SetString(document);
        } else if (params->argumentCount == 1) {
            std::string_view name;
            if (TryGetScaleformStringArgument(call, 0, name) &&
                service.Load(name, document)) {
                (void)result.SetString(document);
            }
        }
    }

    (void)CommitScaleformResult(*params, result);
}

}  // namespace

bool IsValidName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumNameLength ||
        !((name.front() >= 'a' && name.front() <= 'z') ||
          (name.front() >= '0' && name.front() <= '9')) ||
        name.back() == '.') {
        return false;
    }
    for (const char value : name) {
        if (!IsAllowedNameCharacter(value)) {
            return false;
        }
    }
    const auto deviceEnd = name.find('.');
    const auto deviceName = name.substr(0, deviceEnd);
    return std::find(
               std::begin(kReservedNames),
               std::end(kReservedNames),
               deviceName) == std::end(kReservedNames);
}

bool IsValidJsonDocument(std::string_view document) noexcept {
    return !document.empty() && document.size() <= kMaximumDocumentSize &&
        IsValidJson(document);
}

void Service::AttachFileWriter(FileCommandService* writer) noexcept {
    std::scoped_lock lock{mutex_};
    fileWriter_ = writer;
}

bool Service::Register(void* movie, std::string_view name) noexcept {
    if (movie == nullptr || !IsValidName(name)) {
        return false;
    }
    try {
        std::scoped_lock lock{mutex_};
        const auto found = registrations_.find(movie);
        if (found != registrations_.end()) {
            return found->second == name;
        }
        registrations_.emplace(movie, name);
        return true;
    } catch (...) {
        return false;
    }
}

bool Service::QueueWriteLocked(
    std::string_view name,
    std::string_view document) {
    if (fileWriter_ == nullptr) {
        return false;
    }
    std::string key{name};
    if (documents_.size() >= kMaximumCachedDocuments &&
        !documents_.contains(key)) {
        return false;
    }
    const std::wstring relative = StorageRelativePath(name);
    if (fileWriter_->SubmitRelativeFileWrite(relative, document) !=
        FileCommandSubmitStatus::Accepted) {
        return false;
    }
    documents_.insert_or_assign(std::move(key), std::string{document});
    return true;
}

bool Service::LoadLocked(std::string_view name, std::string& document) {
    // The saved copy is newer than the file only until its write finishes.
    // After that the file is read again, so edits made outside the game load.
    const auto cached = documents_.find(std::string{name});
    if (cached != documents_.end() && fileWriter_ != nullptr &&
        fileWriter_->IsWriteQueued(StorageRelativePath(name))) {
        document = cached->second;
        return true;
    }
    return LoadDocument(name, document);
}

bool Service::Save(void* movie, std::string_view document) noexcept {
    if (!IsValidJsonDocument(document)) {
        return false;
    }
    try {
        std::scoped_lock lock{mutex_};
        const auto found = registrations_.find(movie);
        if (found == registrations_.end()) {
            return false;
        }
        return QueueWriteLocked(found->second, document);
    } catch (...) {
        return false;
    }
}

bool Service::Save(
    std::string_view name,
    std::string_view document) noexcept {
    if (!IsValidName(name) || !IsValidJsonDocument(document)) {
        return false;
    }
    try {
        std::scoped_lock lock{mutex_};
        return QueueWriteLocked(name, document);
    } catch (...) {
        return false;
    }
}

bool Service::Load(void* movie, std::string& document) noexcept {
    document.clear();
    try {
        std::scoped_lock lock{mutex_};
        const auto found = registrations_.find(movie);
        if (found == registrations_.end()) {
            return false;
        }
        return LoadLocked(found->second, document);
    } catch (...) {
        document.clear();
        return false;
    }
}

bool Service::Load(
    std::string_view name,
    std::string& document) noexcept {
    document.clear();
    if (!IsValidName(name)) {
        return false;
    }
    try {
        std::scoped_lock lock{mutex_};
        return LoadLocked(name, document);
    } catch (...) {
        document.clear();
        return false;
    }
}

void Service::ForgetMovie(void* movie) noexcept {
    std::scoped_lock lock{mutex_};
    registrations_.erase(movie);
}

void Service::Clear() noexcept {
    std::scoped_lock lock{mutex_};
    registrations_.clear();
    documents_.clear();
}

Service& GlobalService() noexcept {
    static Service service;
    return service;
}

void* __fastcall FunctionHandler::Identity(void* handler) noexcept {
    return handler;
}

template <FunctionHandler::Operation kOperation>
void __fastcall FunctionHandler::Invoke(
    void*,
    const FunctionParams* params) noexcept {
    InvokeOperation(kOperation, params);
}

template <FunctionHandler::Operation kOperation>
const FunctionHandler::HandlerVtableEntry*
FunctionHandler::VtableFor() noexcept {
    static const HandlerVtableEntry vtable[] = {
        reinterpret_cast<HandlerVtableEntry>(&Identity),
        reinterpret_cast<HandlerVtableEntry>(&Invoke<kOperation>),
    };
    return vtable;
}

FunctionHandler::FunctionHandler(Operation operation) noexcept
    : vtable_{nullptr}, referenceCount_{1}, referenceCountPadding_{0} {
    switch (operation) {
        case Operation::Register:
            vtable_ = VtableFor<Operation::Register>();
            break;
        case Operation::Save:
            vtable_ = VtableFor<Operation::Save>();
            break;
        case Operation::Load:
            vtable_ = VtableFor<Operation::Load>();
            break;
    }
}

FunctionHandlers::FunctionHandlers() noexcept
    : register_{FunctionHandler::Operation::Register},
      save_{FunctionHandler::Operation::Save},
      load_{FunctionHandler::Operation::Load} {}

FunctionHandlers& GlobalFunctionHandlers() noexcept {
    static FunctionHandlers handlers;
    return handlers;
}

static_assert(sizeof(FunctionHandler) == 0x10);

}  // namespace sf::modstorage
