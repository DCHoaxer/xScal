#include "api/scaleform_movie_interface.hpp"

#include "common/ascii_case.hpp"
#include "hook/movie_root_hook.hpp"
#include "platform/diagnostics.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"
#include "scaleform/value.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>


namespace sf::movieapi {
namespace {

static_assert(sizeof(XScalMovieValue) == sizeof(ScaleformValue));
static_assert(alignof(XScalMovieValue) == alignof(ScaleformValue));

struct LookupHandler final {
    XScalMovieLookupFn handler{};
    void* userData{};
    std::atomic<bool> disabled{};
};

// A function slot holds either a version 1 function or a version 2 call.
enum class FunctionKind {
    Function,
    Call,
};

struct FunctionRegistration final {
    FunctionKind kind{};
    XScalMovieFunctionFn function{};
    XScalMovieCallFn call{};
    void* userData{};
    std::atomic<bool> disabled{};
};

struct AdvanceHandler final {
    XScalMovieAdvanceFn handler{};
    void* userData{};
    std::atomic<bool> disabled{};
};

constexpr std::size_t kNameCapacity = 128;
constexpr std::size_t kPathCapacity = 256;
constexpr std::size_t kMaxMovies = 64;
constexpr std::uint32_t kMaxFunctionDepth = 8;
// A movie that has no root1 yet is probed on every frame for this many frames,
// then on every 16th frame.
constexpr std::uint32_t kEveryFrameLoadProbes = 120;
constexpr std::uint32_t kSlowLoadProbeMask = 15;
// GFx type code of a method read from an object. IsObjectLikeScaleformValue
// stops at display objects.
constexpr std::uint8_t kClosureType = 11;

struct MovieHandler final {
    char swfName[kNameCapacity]{};
    bool matchesAllMovies{};
    XScalMovieEventFn loaded{};
    XScalMovieEventFn unloading{};
    void* userData{};
    std::atomic<bool> disabled{};
};

struct InvokeFilter final {
    char swfName[kNameCapacity]{};
    bool matchesAllMovies{};
    char methodPath[kPathCapacity]{};
    bool matchesAllMethods{};
    XScalMovieInvokeFilterFn filter{};
    void* userData{};
    std::atomic<bool> disabled{};
};

// MovieRoot with at least one watchAdvance() that was not undone. The
// AdvanceFrame hook reads movieRoot without the lock.
struct WatchedMovie final {
    std::atomic<void*> movieRoot{};
    std::uint32_t references{};
};

// What the host knows about one MovieRoot it has seen advance.
struct MovieRecord final {
    void* movieRoot{};
    std::uintptr_t identity{};
    char url[XSCAL_SCALEFORM_MOVIE_URL_CAPACITY]{};
    bool loaded{};
    // Bit n is set when movie handler n matched this movie and was notified.
    std::uint32_t notifiedHandlers{};
    // Movie handlers [0, checkedHandlers) were already matched against url.
    std::uint32_t checkedHandlers{};
    std::uint32_t frameAttempts{};
};

struct RetainedRef final {
    ScaleformValue value{};
    std::uintptr_t movieIdentity{};
    std::uint32_t generation{};
    bool used{};
};

struct PostedWork final {
    XScalMoviePostedFn function{};
    void* userData{};
    std::uintptr_t identity{};
};

std::mutex registrationMutex;
std::mutex watchMutex;
std::array<WatchedMovie, XSCAL_SCALEFORM_MOVIE_MAX_WATCHED_MOVIES> watchedMovies;
std::atomic<std::uint32_t> watchedMovieCount{};
std::array<AdvanceHandler, XSCAL_SCALEFORM_MOVIE_MAX_ADVANCE_HANDLERS> advanceHandlers;
std::atomic<std::uint32_t> advanceHandlerCount{};
std::atomic<bool> advanceAvailable{};
std::atomic<bool> invokeAvailable{};
std::array<LookupHandler, XSCAL_SCALEFORM_MOVIE_MAX_LOOKUP_HANDLERS> lookupHandlers;
std::atomic<std::uint32_t> lookupHandlerCount{};
std::array<FunctionRegistration, XSCAL_SCALEFORM_MOVIE_MAX_FUNCTIONS> functions;
std::atomic<std::uint32_t> functionCount{};
std::array<MovieHandler, XSCAL_SCALEFORM_MOVIE_MAX_MOVIE_HANDLERS> movieHandlers;
std::atomic<std::uint32_t> movieHandlerCount{};
std::array<InvokeFilter, XSCAL_SCALEFORM_MOVIE_MAX_INVOKE_FILTERS> invokeFilters;
std::atomic<std::uint32_t> invokeFilterCount{};

// Fixed table; records are never moved, so movieUrl() can return a pointer
// into one. Callbacks never run with the lock held.
std::mutex movieMutex;
std::array<MovieRecord, kMaxMovies> movies;

std::mutex refMutex;
std::array<RetainedRef, XSCAL_SCALEFORM_MOVIE_MAX_REFS> refs;
std::atomic<std::uint32_t> liveRefCount{};

// postedCount lets the AdvanceFrame hook skip the lock while the queue is empty.
std::mutex postMutex;
std::array<PostedWork, XSCAL_SCALEFORM_MOVIE_MAX_POSTED> posted;
std::uint32_t postedUsed{};
std::atomic<std::uint32_t> postedCount{};

// Number of movies destroyed at each MovieRoot address. MovieIdentity() puts
// it in the high 16 bits, which are zero in x64 user-mode addresses, so a
// movie created at a reused address gets a new identity.
std::mutex generationMutex;
std::unordered_map<void*, std::uint16_t> movieGenerations;

constexpr std::uintptr_t kIdentityAddressMask = (std::uintptr_t{1} << 48) - 1;

// Number of plugin functions and host-started plugin callbacks running on this
// thread. Game code that a plugin triggers can call GetVariable, AdvanceFrame
// or Invoke again; those calls start no host callbacks while the depth is not
// zero.
thread_local std::uint32_t pluginDepth{};

// Defined further down, next to the other per-frame entry points.
void NotifyAdvance(const MovieRootContext& context) noexcept;
[[nodiscard]] bool IsAdvanceWatched(void* movieRoot) noexcept;

class DepthScope final {
public:
    DepthScope() noexcept { ++pluginDepth; }
    ~DepthScope() { --pluginDepth; }
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;
};

using CreateFunctionRoutine = void(__fastcall*)(
    void* movieRoot, ScaleformValue* outValue, void* handler, void* userData);

[[nodiscard]] ScaleformValue& AsValue(XScalMovieValue* value) noexcept {
    return *reinterpret_cast<ScaleformValue*>(value);
}

[[nodiscard]] ScaleformValue& AsValue(const XScalMovieValue* value) noexcept {
    return *reinterpret_cast<ScaleformValue*>(
        const_cast<XScalMovieValue*>(value));
}

[[nodiscard]] const MovieRootContext* ContextOf(XScalMovie* movie) noexcept {
    return movie == nullptr ? nullptr : movie->context;
}

template <typename T>
[[nodiscard]] T ReadPayload(const ScaleformValue& value) noexcept {
    T result{};
    std::memcpy(
        &result,
        value.storage + layout::kValueInternalTwoOffset,
        sizeof(result));
    return result;
}

[[nodiscard]] std::uint8_t BaseType(const ScaleformValue& value) noexcept {
    return InspectScaleformValue(value).rawType & layout::kTypeMask;
}

[[nodiscard]] bool IsOwned(const ScaleformValue& value) noexcept {
    return (InspectScaleformValue(value).rawType &
            layout::kOwnedValueFlag) != 0;
}

void ClearOwnedFlag(ScaleformValue& value) noexcept {
    auto& typeByte = value.storage[layout::kValueTypeOffset];
    typeByte = static_cast<std::byte>(
        static_cast<std::uint8_t>(typeByte) &
        static_cast<std::uint8_t>(~layout::kOwnedValueFlag));
}

void CopyText(char* destination, std::size_t capacity, std::string_view text) noexcept {
    const auto length = (std::min)(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

[[nodiscard]] std::uintptr_t MakeIdentity(void* movieRoot) noexcept {
    std::uint16_t generation{};
    {
        std::scoped_lock lock{generationMutex};
        const auto found = movieGenerations.find(movieRoot);
        if (found != movieGenerations.end()) {
            generation = found->second;
        }
    }
    return reinterpret_cast<std::uintptr_t>(movieRoot) |
        (static_cast<std::uintptr_t>(generation) << 48);
}

[[nodiscard]] bool IsCurrentIdentity(std::uintptr_t identity) noexcept {
    const auto movieRoot = reinterpret_cast<void*>(identity & kIdentityAddressMask);
    return movieRoot != nullptr && MakeIdentity(movieRoot) == identity;
}

[[nodiscard]] bool IsCrash(DWORD code) noexcept {
    return code != EXCEPTION_BREAKPOINT && code != EXCEPTION_SINGLE_STEP;
}

// Returns false when the plugin raised a structured exception.
[[nodiscard]] bool CallLookupHandler(
    XScalMovieLookupFn handler,
    void* userData,
    XScalMovie* movie,
    const char* path) noexcept {
    __try {
        handler(userData, movie, path);
        return true;
    } __except (IsCrash(GetExceptionCode())
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Advance handlers, version 1 functions, movie events and posted work share
// this signature.
[[nodiscard]] bool CallMovieCallback(
    XScalMovieFunctionFn function,
    void* userData,
    XScalMovie* movie) noexcept {
    __try {
        function(userData, movie);
        return true;
    } __except (IsCrash(GetExceptionCode())
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

[[nodiscard]] bool CallPluginCall(
    XScalMovieCallFn function,
    void* userData,
    XScalMovie* movie,
    const XScalMovieCall* call) noexcept {
    __try {
        function(userData, movie, call);
        return true;
    } __except (IsCrash(GetExceptionCode())
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

[[nodiscard]] bool CallInvokeFilter(
    const InvokeFilter& entry,
    XScalMovie* movie,
    const char* methodPath,
    XScalMovieValue* arguments,
    std::uint32_t argumentCount,
    int32_t& verdict) noexcept {
    __try {
        verdict = entry.filter(
            entry.userData, movie, methodPath, arguments, argumentCount);
        return true;
    } __except (IsCrash(GetExceptionCode())
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Builds the plugin's view of one 0x30-byte value record from the game: a
// 0x40-byte value with the tail zeroed and the owned flag cleared, so release()
// does nothing. Strings become plain borrowed strings, because the owned flag
// also selects how the string pointer is read.
// With allowFallback, a record whose primary pair is undefined and whose
// fallback pair is not (see DecodePluginArgument) is read from the fallback.
void MakeBorrowedView(
    const std::byte* record,
    bool allowFallback,
    XScalMovieValue& view) noexcept {
    auto& value = AsValue(&view);
    value = {};
    std::memcpy(value.storage, record, layout::kFunctionArgumentSize);
    if (allowFallback) {
        const auto candidates = ReadArgumentCandidates(record);
        const auto primaryKind = BaseType(value);
        const auto fallbackKind = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(candidates.fallback.type) &
            layout::kTypeMask);
        if (primaryKind == 0 && fallbackKind != 0) {
            std::memcpy(
                value.storage + layout::kPrimaryArgumentTypeOffset,
                &candidates.fallback.type,
                sizeof(candidates.fallback.type));
            std::memcpy(
                value.storage + layout::kPrimaryArgumentPointerOffset,
                &candidates.fallback.payload,
                sizeof(candidates.fallback.payload));
        }
    }
    if (BaseType(value) == layout::kStringType) {
        std::string_view text;
        if (TryDecodeScaleformValueString(value, text)) {
            InitializeBorrowedScaleformString(value, text.data());
        } else {
            value = {};
        }
        return;
    }
    ClearOwnedFlag(value);
}

// Moves the plugin's result into the game's result value.
void CommitCallResult(const XScalMovieValue& result, void* rawResult) noexcept {
    if (rawResult == nullptr) {
        return;
    }
    const auto& value = AsValue(&result);
    const auto type = BaseType(value);
    if (IsOwned(value)) {
        // The game releases it from here on; the plugin must not.
        std::memcpy(rawResult, value.storage, layout::kValueSize);
        RelinkMovedScaleformValue(*static_cast<ScaleformValue*>(rawResult));
        return;
    }
    switch (type) {
        case layout::kBooleanType:
        case layout::kIntType:
        case layout::kUIntType:
        case layout::kNumberType:
            std::memcpy(rawResult, value.storage, layout::kValueSize);
            return;
        case layout::kStringType: {
            std::string_view text;
            if (TryDecodeScaleformValueString(value, text)) {
                (void)WriteScaleformStringResult(rawResult, text);
            }
            return;
        }
        default:
            return;
    }
}

// Scaleform function handlers are 0x10-byte objects (vtable, ref count,
// padding) and the creation call here passes no user data. All handlers
// share one vtable; the handler pointer passed as the first argument selects
// the function by its position in handlerObjects.
class PluginFunctionHandler final {
public:
    using HandlerVtableEntry = void(__fastcall*)();

    PluginFunctionHandler() noexcept;

private:
    const HandlerVtableEntry* vtable_;
    volatile std::int32_t referenceCount_{1};
    std::uint32_t referenceCountPadding_{};
};

static_assert(sizeof(PluginFunctionHandler) == 0x10);

void* __fastcall HandlerIdentity(void* handler) noexcept {
    return handler;
}

void __fastcall InvokeHandler(void* handler, const FunctionParams* params) noexcept;

PluginFunctionHandler::PluginFunctionHandler() noexcept {
    static const HandlerVtableEntry vtable[] = {
        reinterpret_cast<HandlerVtableEntry>(&HandlerIdentity),
        reinterpret_cast<HandlerVtableEntry>(&InvokeHandler),
    };
    vtable_ = vtable;
}

std::array<PluginFunctionHandler, XSCAL_SCALEFORM_MOVIE_MAX_FUNCTIONS>&
FunctionHandlers() noexcept {
    static std::array<PluginFunctionHandler, XSCAL_SCALEFORM_MOVIE_MAX_FUNCTIONS>
        handlerObjects;
    return handlerObjects;
}

// Position of handler in the handler array, or the array size when handler is
// not one of its elements.
[[nodiscard]] std::size_t FunctionIdOfHandler(const void* handler) noexcept {
    auto& handlerObjects = FunctionHandlers();
    const auto first = reinterpret_cast<std::uintptr_t>(handlerObjects.data());
    const auto address = reinterpret_cast<std::uintptr_t>(handler);
    if (address < first ||
        address >= first + sizeof(PluginFunctionHandler) * handlerObjects.size() ||
        (address - first) % sizeof(PluginFunctionHandler) != 0) {
        return handlerObjects.size();
    }
    return (address - first) / sizeof(PluginFunctionHandler);
}

struct FunctionCall final {
    FunctionRegistration* registration;
    std::uint32_t functionId;
    const FunctionParams* params;
};

// Runs one registerCall() function. Returns false when the plugin raised an
// exception.
[[nodiscard]] bool RunPluginCall(
    const FunctionCall& call,
    XScalMovie& movie) noexcept {
    const auto& params = *call.params;
    const auto argumentCount = params.argumentCount;
    if (argumentCount > XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS) {
        static std::atomic<bool> reported{};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            DiagnosticLogFormat(
                "Scaleform movie interface: call %u has %u arguments, more than the limit of %u; not calling the plugin",
                static_cast<unsigned int>(call.functionId),
                static_cast<unsigned int>(argumentCount),
                static_cast<unsigned int>(XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS));
        }
        return true;
    }
    std::array<XScalMovieValue, XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS> arguments{};
    for (std::uint32_t index = 0; index < argumentCount; ++index) {
        MakeBorrowedView(
            params.arguments + index * layout::kFunctionArgumentSize,
            true,
            arguments[index]);
    }
    XScalMovieValue thisObject{};
    if (params.thisObject != nullptr) {
        // The fallback pair is only read for argument records.
        MakeBorrowedView(
            reinterpret_cast<const std::byte*>(params.thisObject),
            false,
            thisObject);
    }
    XScalMovieValue result{};
    const XScalMovieCall pluginCall{
        &thisObject,
        argumentCount != 0 ? arguments.data() : nullptr,
        argumentCount,
        &result,
    };
    const bool completed = CallPluginCall(
        call.registration->call,
        call.registration->userData,
        &movie,
        &pluginCall);
    if (completed && params.result != nullptr) {
        CommitCallResult(result, params.result);
        return true;
    }
    // The game has no result slot, or the plugin crashed after moving an
    // owned value into the result: release the reference here.
    ReleaseValue(*movie.context, AsValue(&result));
    return completed;
}

bool RunFunction(const MovieRootContext& context, void* userData) noexcept {
    auto& call = *static_cast<FunctionCall*>(userData);
    XScalMovie movie{&context};
    bool completed{};
    {
        DepthScope depth;
        completed = call.registration->kind == FunctionKind::Call
            ? RunPluginCall(call, movie)
            : CallMovieCallback(
                  call.registration->function,
                  call.registration->userData,
                  &movie);
    }
    if (!completed) {
        call.registration->disabled.store(true, std::memory_order_relaxed);
        DiagnosticLogFormat(
            "Scaleform movie interface: plugin function %u raised an exception and was disabled",
            static_cast<unsigned int>(call.functionId));
    }
    return true;
}

void __fastcall InvokeHandler(void* handler, const FunctionParams* params) noexcept {
    const auto functionId = FunctionIdOfHandler(handler);
    // FunctionParams::movie is not the hooked MovieRoot (seen 0x3170 bytes
    // below it), so the movie root given as user data at creation is used
    // when the movie pointer is not recognized.
    void* createdIn{};
    if (params != nullptr) {
        std::memcpy(
            &createdIn,
            reinterpret_cast<const std::byte*>(params) +
                layout::kFunctionParamsUserDataOffset,
            sizeof(createdIn));
    }
    if (params == nullptr || pluginDepth >= kMaxFunctionDepth ||
        functionId >= functionCount.load(std::memory_order_acquire)) {
        return;
    }
    auto& registration = functions[functionId];
    if (registration.disabled.load(std::memory_order_relaxed)) {
        return;
    }
    FunctionCall call{
        &registration, static_cast<std::uint32_t>(functionId), params};
    if (VisitMovieRoot(params->movie, &RunFunction, &call) ||
        (createdIn != nullptr && VisitMovieRoot(createdIn, &RunFunction, &call))) {
        return;
    }
    static std::atomic<bool> reported{};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        DiagnosticLogFormatImmediate(
            "Scaleform movie interface: function %u called with an unrecognized movie %p (created in %p)",
            static_cast<unsigned int>(functionId),
            params->movie,
            createdIn);
    }
}

int32_t __cdecl AddLookupHandler(
    XScalMovieLookupFn handler,
    void* userData) noexcept {
    if (handler == nullptr) {
        return 0;
    }
    std::scoped_lock lock{registrationMutex};
    const auto index = lookupHandlerCount.load(std::memory_order_relaxed);
    if (index >= lookupHandlers.size()) {
        return 0;
    }
    lookupHandlers[index].handler = handler;
    lookupHandlers[index].userData = userData;
    lookupHandlers[index].disabled.store(false, std::memory_order_relaxed);
    lookupHandlerCount.store(index + 1, std::memory_order_release);
    return 1;
}

int32_t __cdecl AddAdvanceHandler(
    XScalMovieAdvanceFn handler,
    void* userData) noexcept {
    if (handler == nullptr || !advanceAvailable.load(std::memory_order_acquire)) {
        return 0;
    }
    std::scoped_lock lock{registrationMutex};
    const auto index = advanceHandlerCount.load(std::memory_order_relaxed);
    if (index >= advanceHandlers.size()) {
        return 0;
    }
    advanceHandlers[index].handler = handler;
    advanceHandlers[index].userData = userData;
    advanceHandlers[index].disabled.store(false, std::memory_order_relaxed);
    advanceHandlerCount.store(index + 1, std::memory_order_release);
    return 1;
}

// Shared by registerFunction() and registerCall().
int32_t AddFunction(
    FunctionKind kind,
    XScalMovieFunctionFn function,
    XScalMovieCallFn call,
    void* userData,
    uint32_t* functionId) noexcept {
    (void)FunctionHandlers();
    std::scoped_lock lock{registrationMutex};
    const auto index = functionCount.load(std::memory_order_relaxed);
    if (index >= functions.size()) {
        return 0;
    }
    functions[index].kind = kind;
    functions[index].function = function;
    functions[index].call = call;
    functions[index].userData = userData;
    functions[index].disabled.store(false, std::memory_order_relaxed);
    functionCount.store(index + 1, std::memory_order_release);
    *functionId = index;
    return 1;
}

int32_t __cdecl RegisterFunction(
    XScalMovieFunctionFn function,
    void* userData,
    uint32_t* functionId) noexcept {
    if (function == nullptr || functionId == nullptr) {
        return 0;
    }
    return AddFunction(FunctionKind::Function, function, nullptr, userData, functionId);
}

int32_t __cdecl RegisterCall(
    XScalMovieCallFn function,
    void* userData,
    uint32_t* functionId) noexcept {
    if (function == nullptr || functionId == nullptr) {
        return 0;
    }
    return AddFunction(FunctionKind::Call, nullptr, function, userData, functionId);
}

[[nodiscard]] WatchedMovie* FindWatch(void* movieRoot) noexcept {
    for (auto& entry : watchedMovies) {
        if (entry.movieRoot.load(std::memory_order_relaxed) == movieRoot) {
            return &entry;
        }
    }
    return nullptr;
}

void ClearWatch(WatchedMovie& entry) noexcept {
    entry.movieRoot.store(nullptr, std::memory_order_relaxed);
    entry.references = 0;
    watchedMovieCount.fetch_sub(1, std::memory_order_release);
}

int32_t __cdecl WatchAdvance(XScalMovie* movie) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr ||
        !advanceAvailable.load(std::memory_order_acquire)) {
        return 0;
    }
    std::scoped_lock lock{watchMutex};
    if (auto* entry = FindWatch(context->movieRoot); entry != nullptr) {
        ++entry->references;
        return 1;
    }
    auto* entry = FindWatch(nullptr);
    if (entry == nullptr) {
        return 0;
    }
    entry->references = 1;
    entry->movieRoot.store(context->movieRoot, std::memory_order_relaxed);
    watchedMovieCount.fetch_add(1, std::memory_order_release);
    return 1;
}

void __cdecl UnwatchAdvance(XScalMovie* movie) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr) {
        return;
    }
    std::scoped_lock lock{watchMutex};
    auto* entry = FindWatch(context->movieRoot);
    if (entry != nullptr && --entry->references == 0) {
        ClearWatch(*entry);
    }
}

uintptr_t __cdecl MovieIdentity(XScalMovie* movie) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr) {
        return 0;
    }
    return MakeIdentity(context->movieRoot);
}

int32_t __cdecl GetVariable(
    XScalMovie* movie,
    const char* path,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || path == nullptr || result == nullptr) {
        return 0;
    }
    return GetScaleformVariable(*context, path, AsValue(result)) ? 1 : 0;
}

int32_t __cdecl GetMember(
    XScalMovie* movie,
    const XScalMovieValue* object,
    const char* name,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || object == nullptr || name == nullptr ||
        result == nullptr) {
        return 0;
    }
    AsValue(result) = {};
    return GetScaleformMember(*context, AsValue(object), name, AsValue(result))
        ? 1
        : 0;
}

int32_t __cdecl SetMember(
    XScalMovie* movie,
    const XScalMovieValue* object,
    const char* name,
    const XScalMovieValue* value) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || object == nullptr || name == nullptr ||
        value == nullptr) {
        return 0;
    }
    return SetScaleformMember(*context, AsValue(object), name, AsValue(value))
        ? 1
        : 0;
}

int32_t __cdecl Invoke(
    XScalMovie* movie,
    const char* methodPath,
    const XScalMovieValue* arguments,
    uint32_t argumentCount,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || methodPath == nullptr ||
        argumentCount > XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS ||
        (argumentCount != 0 && arguments == nullptr)) {
        return 0;
    }
    const auto* scaleformArguments = argumentCount == 0
        ? nullptr
        : &AsValue(arguments);
    if (result != nullptr) {
        return InvokeScaleformMethod(
                   *context,
                   methodPath,
                   AsValue(result),
                   scaleformArguments,
                   argumentCount)
            ? 1
            : 0;
    }
    ScaleformValue ignored{};
    ScopedScaleformValue releaseIgnored{*context, ignored};
    return InvokeScaleformMethod(
               *context,
               methodPath,
               ignored,
               scaleformArguments,
               argumentCount)
        ? 1
        : 0;
}

int32_t __cdecl CreateFunction(
    XScalMovie* movie,
    uint32_t functionId,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || result == nullptr ||
        functionId >= functionCount.load(std::memory_order_acquire)) {
        return 0;
    }
    const auto createFunction = GetMovieRootRoutine<CreateFunctionRoutine>(
        context->movieRoot,
        layout::kCreateFunctionVtableOffset);
    if (createFunction == nullptr) {
        return 0;
    }
    auto& value = AsValue(result);
    value = {};
    // The movie root is the user data, so a call can find its movie even when
    // FunctionParams::movie is not the hooked MovieRoot.
    createFunction(
        context->movieRoot,
        &value,
        &FunctionHandlers()[functionId],
        context->movieRoot);
    if (IsObjectLikeScaleformValue(value)) {
        return 1;
    }
    ReleaseValue(*context, value);
    value = {};
    return 0;
}

int32_t __cdecl CreateObject(
    XScalMovie* movie,
    const char* className,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || className == nullptr || result == nullptr) {
        return 0;
    }
    return CreateScaleformObject(*context, className, nullptr, 0, AsValue(result))
        ? 1
        : 0;
}

void __cdecl Release(XScalMovie* movie, XScalMovieValue* value) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || value == nullptr) {
        return;
    }
    ReleaseValue(*context, AsValue(value));
    AsValue(value) = {};
}

int32_t __cdecl ReadPrimitive(
    const XScalMovieValue* value,
    XScalScaleformValue* result) noexcept {
    if (value == nullptr || result == nullptr) {
        return 0;
    }
    *result = {};
    const auto& source = AsValue(value);
    switch (InspectScaleformValue(source).rawType & layout::kTypeMask) {
        case 0:
            result->type = XSCAL_SCALEFORM_VALUE_UNDEFINED;
            return 1;
        case layout::kBooleanType:
            result->type = XSCAL_SCALEFORM_VALUE_BOOLEAN;
            result->value.booleanValue =
                // One-byte boolean; the bytes above it are not cleared.
                (ReadPayload<std::uint64_t>(source) & 0xFF) != 0 ? 1 : 0;
            return 1;
        case layout::kIntType:
            result->type = XSCAL_SCALEFORM_VALUE_INT32;
            result->value.int32Value = ReadPayload<std::int32_t>(source);
            return 1;
        case layout::kUIntType:
            result->type = XSCAL_SCALEFORM_VALUE_UINT32;
            result->value.uint32Value = ReadPayload<std::uint32_t>(source);
            return 1;
        case layout::kNumberType:
            result->type = XSCAL_SCALEFORM_VALUE_NUMBER;
            result->value.numberValue = ReadPayload<double>(source);
            return 1;
        case layout::kStringType: {
            std::string_view text;
            if (!TryDecodeScaleformValueString(source, text) ||
                text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                return 0;
            }
            result->type = XSCAL_SCALEFORM_VALUE_STRING;
            result->stringLength = static_cast<std::uint32_t>(text.size());
            result->value.stringValue = text.data();
            return 1;
        }
        default:
            return 0;
    }
}

int32_t __cdecl WritePrimitive(
    const XScalScaleformValue* value,
    XScalMovieValue* result) noexcept {
    if (value == nullptr || result == nullptr) {
        return 0;
    }
    auto& target = AsValue(result);
    switch (value->type) {
        case XSCAL_SCALEFORM_VALUE_UNDEFINED:
            target = {};
            return 1;
        case XSCAL_SCALEFORM_VALUE_BOOLEAN:
            InitializeScaleformBoolean(target, value->value.booleanValue != 0);
            return 1;
        case XSCAL_SCALEFORM_VALUE_INT32:
            InitializeScaleformValue(
                target, layout::kIntType, value->value.int32Value);
            return 1;
        case XSCAL_SCALEFORM_VALUE_UINT32:
            InitializeScaleformValue(
                target, layout::kUIntType, value->value.uint32Value);
            return 1;
        case XSCAL_SCALEFORM_VALUE_NUMBER:
            InitializeScaleformValue(
                target, layout::kNumberType, value->value.numberValue);
            return 1;
        case XSCAL_SCALEFORM_VALUE_STRING:
            if (value->value.stringValue == nullptr) {
                return 0;
            }
            InitializeBorrowedScaleformString(target, value->value.stringValue);
            return 1;
        default:
            return 0;
    }
}

int32_t __cdecl IsObject(const XScalMovieValue* value) noexcept {
    return value != nullptr && IsObjectLikeScaleformValue(AsValue(value))
        ? 1
        : 0;
}

uintptr_t __cdecl ObjectIdentity(const XScalMovieValue* value) noexcept {
    if (value == nullptr || !IsObjectLikeScaleformValue(AsValue(value))) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(InspectScaleformValue(AsValue(value)).data);
}

[[nodiscard]] MovieRecord* FindMovieRecord(void* movieRoot) noexcept {
    for (auto& record : movies) {
        if (record.movieRoot == movieRoot) {
            return &record;
        }
    }
    return nullptr;
}

// Returns the record for a movie, replacing a record left over from an
// earlier movie at the same address. Null when the table is full. Call with
// movieMutex held.
[[nodiscard]] MovieRecord* FindOrCreateMovieRecord(
    void* movieRoot,
    std::uintptr_t identity) noexcept {
    if (auto* record = FindMovieRecord(movieRoot); record != nullptr) {
        if (record->identity == identity) {
            return record;
        }
        *record = {};
        record->movieRoot = movieRoot;
        record->identity = identity;
        return record;
    }
    auto* record = FindMovieRecord(nullptr);
    if (record == nullptr) {
        static std::atomic<bool> reported{};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            DiagnosticLogFormat(
                "Scaleform movie interface: movie table is full (%u movies)",
                static_cast<unsigned int>(kMaxMovies));
        }
        return nullptr;
    }
    *record = {};
    record->movieRoot = movieRoot;
    record->identity = identity;
    return record;
}

// Copies the URL of a movie into url. Writes "" for a movie without a record.
void CopyMovieUrl(
    void* movieRoot,
    char (&url)[XSCAL_SCALEFORM_MOVIE_URL_CAPACITY]) noexcept {
    std::scoped_lock lock{movieMutex};
    const auto* record = FindMovieRecord(movieRoot);
    CopyText(url, sizeof(url), record == nullptr ? std::string_view{} : record->url);
}

int32_t __cdecl RegisterMovieHandlers(
    const char* swfName,
    XScalMovieEventFn loaded,
    XScalMovieEventFn unloading,
    void* userData) noexcept {
    if ((loaded == nullptr && unloading == nullptr) ||
        !advanceAvailable.load(std::memory_order_acquire) ||
        (swfName != nullptr && std::strlen(swfName) >= kNameCapacity)) {
        return 0;
    }
    std::scoped_lock lock{registrationMutex};
    const auto index = movieHandlerCount.load(std::memory_order_relaxed);
    if (index >= movieHandlers.size()) {
        return 0;
    }
    auto& entry = movieHandlers[index];
    entry.matchesAllMovies = swfName == nullptr;
    CopyText(entry.swfName, sizeof(entry.swfName), swfName == nullptr ? "" : swfName);
    entry.loaded = loaded;
    entry.unloading = unloading;
    entry.userData = userData;
    entry.disabled.store(false, std::memory_order_relaxed);
    movieHandlerCount.store(index + 1, std::memory_order_release);
    return 1;
}

const char* __cdecl MovieUrl(XScalMovie* movie) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr) {
        return "";
    }
    std::scoped_lock lock{movieMutex};
    const auto* record = FindMovieRecord(context->movieRoot);
    return record == nullptr ? "" : record->url;
}

int32_t __cdecl InvokeMember(
    XScalMovie* movie,
    const XScalMovieValue* object,
    const char* name,
    const XScalMovieValue* arguments,
    uint32_t argumentCount,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || object == nullptr || name == nullptr ||
        argumentCount > XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS ||
        (argumentCount != 0 && arguments == nullptr)) {
        return 0;
    }
    const auto* scaleformArguments = argumentCount == 0
        ? nullptr
        : &AsValue(arguments);
    if (result != nullptr) {
        return InvokeScaleformMember(
                   *context,
                   AsValue(object),
                   name,
                   scaleformArguments,
                   argumentCount,
                   &AsValue(result))
            ? 1
            : 0;
    }
    ScaleformValue ignored{};
    ScopedScaleformValue releaseIgnored{*context, ignored};
    return InvokeScaleformMember(
               *context,
               AsValue(object),
               name,
               scaleformArguments,
               argumentCount,
               &ignored)
        ? 1
        : 0;
}

int32_t __cdecl MoveValue(
    XScalMovie* movie,
    XScalMovieValue* destination,
    XScalMovieValue* source) noexcept {
    if (ContextOf(movie) == nullptr || destination == nullptr || source == nullptr) {
        return 0;
    }
    if (destination == source) {
        return 1;
    }
    auto& target = AsValue(destination);
    target = AsValue(source);
    RelinkMovedScaleformValue(target);
    AsValue(source) = {};
    return 1;
}

uint32_t __cdecl ValueType(const XScalMovieValue* value) noexcept {
    if (value == nullptr) {
        return 0;
    }
    return InspectScaleformValue(AsValue(value)).rawType & 0x0F;
}

constexpr XScalMovieRef MakeRef(std::size_t index, std::uint32_t generation) noexcept {
    return (static_cast<XScalMovieRef>(generation) << 32) | (index + 1);
}

// Returns the slot for ref if it is in use, has the same generation and was
// retained by this movie, otherwise null. Call with refMutex held.
[[nodiscard]] RetainedRef* FindRef(
    XScalMovieRef ref,
    std::uintptr_t movieIdentity) noexcept {
    const auto position = static_cast<std::uint32_t>(ref);
    if (position == 0 || position > refs.size()) {
        return nullptr;
    }
    auto& slot = refs[position - 1];
    if (!slot.used || slot.generation != static_cast<std::uint32_t>(ref >> 32) ||
        slot.movieIdentity != movieIdentity) {
        return nullptr;
    }
    return &slot;
}

XScalMovieRef __cdecl Retain(XScalMovie* movie, XScalMovieValue* value) noexcept {
    const auto* context = ContextOf(movie);
    // Refs are released from the destroy hook. The AdvanceFrame hook is
    // installed only after it, so advanceAvailable also means the destroy hook
    // is active.
    if (context == nullptr || context->movieRoot == nullptr || value == nullptr ||
        !advanceAvailable.load(std::memory_order_acquire)) {
        return 0;
    }
    auto& source = AsValue(value);
    const auto info = InspectScaleformValue(source);
    const auto type = BaseType(source);
    const bool ownedObject = IsOwned(source) &&
        type >= layout::kObjectType && type <= kClosureType &&
        info.objectInterface != nullptr && info.data != nullptr;
    const bool primitive = !IsOwned(source) && type <= layout::kNumberType;
    if (!ownedObject && !primitive) {
        return 0;
    }
    const auto identity = MakeIdentity(context->movieRoot);
    std::scoped_lock lock{refMutex};
    for (std::size_t index = 0; index < refs.size(); ++index) {
        auto& slot = refs[index];
        if (slot.used) {
            continue;
        }
        slot.value = source;
        RelinkMovedScaleformValue(slot.value);
        slot.movieIdentity = identity;
        ++slot.generation;
        slot.used = true;
        liveRefCount.fetch_add(1, std::memory_order_relaxed);
        source = {};
        return MakeRef(index, slot.generation);
    }
    return 0;
}

int32_t __cdecl LoadRef(
    XScalMovie* movie,
    XScalMovieRef ref,
    XScalMovieValue* result) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr || result == nullptr) {
        return 0;
    }
    const auto identity = MakeIdentity(context->movieRoot);
    std::scoped_lock lock{refMutex};
    const auto* slot = FindRef(ref, identity);
    if (slot == nullptr) {
        return 0;
    }
    auto& target = AsValue(result);
    target = slot->value;
    ClearOwnedFlag(target);
    return 1;
}

void __cdecl DropRef(XScalMovie* movie, XScalMovieRef ref) noexcept {
    const auto* context = ContextOf(movie);
    if (context == nullptr || context->movieRoot == nullptr) {
        return;
    }
    const auto identity = MakeIdentity(context->movieRoot);
    ScaleformValue value{};
    {
        std::scoped_lock lock{refMutex};
        auto* slot = FindRef(ref, identity);
        if (slot == nullptr) {
            return;
        }
        value = slot->value;
        slot->value = {};
        slot->used = false;
        liveRefCount.fetch_sub(1, std::memory_order_relaxed);
    }
    ReleaseValue(*context, value);
}

// Frees every ref retained by one movie. The values are released through
// context when it is not null; otherwise they are only forgotten.
void DropRefsOfMovie(
    std::uintptr_t movieIdentity,
    const MovieRootContext* context) noexcept {
    if (liveRefCount.load(std::memory_order_relaxed) == 0) {
        return;
    }
    for (;;) {
        ScaleformValue value{};
        {
            std::scoped_lock lock{refMutex};
            RetainedRef* found{};
            for (auto& slot : refs) {
                if (slot.used && slot.movieIdentity == movieIdentity) {
                    found = &slot;
                    break;
                }
            }
            if (found == nullptr) {
                return;
            }
            value = found->value;
            found->value = {};
            found->used = false;
            liveRefCount.fetch_sub(1, std::memory_order_relaxed);
        }
        if (context != nullptr) {
            ReleaseValue(*context, value);
        }
    }
}

int32_t __cdecl Post(
    uintptr_t movieIdentity,
    XScalMoviePostedFn function,
    void* userData) noexcept {
    // Posted work runs from the AdvanceFrame hook.
    if (function == nullptr || !advanceAvailable.load(std::memory_order_acquire) ||
        (movieIdentity != 0 && !IsCurrentIdentity(movieIdentity))) {
        return 0;
    }
    std::scoped_lock lock{postMutex};
    if (postedUsed >= posted.size()) {
        return 0;
    }
    posted[postedUsed++] = {function, userData, movieIdentity};
    postedCount.store(postedUsed, std::memory_order_release);
    return 1;
}

// Removes the posted work of a movie that is being destroyed.
void DropPostedWork(std::uintptr_t movieIdentity) noexcept {
    if (postedCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lock{postMutex};
    std::uint32_t kept{};
    for (std::uint32_t index = 0; index < postedUsed; ++index) {
        if (posted[index].identity != movieIdentity) {
            posted[kept++] = posted[index];
        }
    }
    postedUsed = kept;
    postedCount.store(kept, std::memory_order_release);
}

// Runs the work queued for this movie, and work queued for any movie, in the
// order it was posted. The queue lock is not held while plugin code runs.
void RunPostedWork(const MovieRootContext& context, std::uintptr_t identity) noexcept {
    if (postedCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::array<PostedWork, XSCAL_SCALEFORM_MOVIE_MAX_POSTED> due;
    std::uint32_t dueCount{};
    {
        std::scoped_lock lock{postMutex};
        std::uint32_t kept{};
        for (std::uint32_t index = 0; index < postedUsed; ++index) {
            const auto workIdentity = posted[index].identity;
            if (workIdentity == 0 || workIdentity == identity) {
                due[dueCount++] = posted[index];
            } else if (IsCurrentIdentity(workIdentity)) {
                posted[kept++] = posted[index];
            }
            // Otherwise the movie was destroyed after Post() checked it; the
            // destroy hook's DropPostedWork() ran before the entry was added.
        }
        postedUsed = kept;
        postedCount.store(kept, std::memory_order_release);
    }
    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < dueCount; ++index) {
        bool completed{};
        {
            DepthScope depth;
            completed = CallMovieCallback(due[index].function, due[index].userData, &movie);
        }
        if (!completed) {
            DiagnosticLogFormat(
                "Scaleform movie interface: posted work raised an exception");
        }
    }
}

int32_t __cdecl AddInvokeFilter(
    const char* swfName,
    const char* methodPath,
    XScalMovieInvokeFilterFn filter,
    void* userData) noexcept {
    if (filter == nullptr || !invokeAvailable.load(std::memory_order_acquire) ||
        (swfName != nullptr && std::strlen(swfName) >= kNameCapacity) ||
        (methodPath != nullptr && std::strlen(methodPath) >= kPathCapacity)) {
        return 0;
    }
    std::scoped_lock lock{registrationMutex};
    const auto index = invokeFilterCount.load(std::memory_order_relaxed);
    if (index >= invokeFilters.size()) {
        return 0;
    }
    auto& entry = invokeFilters[index];
    entry.matchesAllMovies = swfName == nullptr;
    CopyText(entry.swfName, sizeof(entry.swfName), swfName == nullptr ? "" : swfName);
    entry.matchesAllMethods = methodPath == nullptr;
    CopyText(
        entry.methodPath,
        sizeof(entry.methodPath),
        methodPath == nullptr ? "" : methodPath);
    entry.filter = filter;
    entry.userData = userData;
    entry.disabled.store(false, std::memory_order_relaxed);
    invokeFilterCount.store(index + 1, std::memory_order_release);
    return 1;
}

// The other functions read through the original GetVariable, which skips
// xScal's router. This one looks up objectPath.__SFCodeObj.call through the
// hooked GetVariable instead: the router attaches call() to that code object
// for a .__SFCodeObj.call path, the same as when game code looks it up.
int32_t __cdecl AttachCodeObject(
    XScalMovie* movie,
    const char* objectPath) noexcept {
    constexpr std::string_view kRootPrefix{"root1."};
    constexpr std::string_view kCallSuffix{".__SFCodeObj.call"};
    const auto* context = ContextOf(movie);
    if (context == nullptr || objectPath == nullptr) {
        return 0;
    }
    const std::string_view object{objectPath};
    char path[kNameCapacity + kCallSuffix.size() + 1]{};
    if (!object.starts_with(kRootPrefix) || object.size() >= kNameCapacity) {
        return 0;
    }
    std::memcpy(path, object.data(), object.size());
    std::memcpy(path + object.size(), kCallSuffix.data(), kCallSuffix.size());

    ScaleformValue call{};
    const bool found =
        HookedMovieRootGetVariable(context->movieRoot, &call, path) &&
        IsObjectLikeScaleformValue(call);
    ReleaseValue(*context, call);
    return found ? 1 : 0;
}

// Plugins asking for version 2 get this table through a V2 pointer; the
// structures share their layout up to moveValue.
constexpr XScalScaleformMovieV3 kInterface{
    sizeof(XScalScaleformMovieV3),
    XSCAL_SCALEFORM_MOVIE_API_VERSION,
    &AddLookupHandler,
    &RegisterFunction,
    &MovieIdentity,
    &GetVariable,
    &GetMember,
    &SetMember,
    &Invoke,
    &CreateFunction,
    &CreateObject,
    &Release,
    &ReadPrimitive,
    &WritePrimitive,
    &IsObject,
    &ObjectIdentity,
    &AddAdvanceHandler,
    &WatchAdvance,
    &UnwatchAdvance,
    &RegisterCall,
    &MovieUrl,
    &RegisterMovieHandlers,
    &InvokeMember,
    &ValueType,
    &Retain,
    &LoadRef,
    &DropRef,
    &Post,
    &AddInvokeFilter,
    &MoveValue,
    &AttachCodeObject,
};

}  // namespace

const XScalScaleformMovieV2* Interface() noexcept {
    return reinterpret_cast<const XScalScaleformMovieV2*>(&kInterface);
}

const XScalScaleformMovieV1* InterfaceV1() noexcept {
    static constexpr XScalScaleformMovieV1 kInterfaceV1{
        sizeof(XScalScaleformMovieV1),
        1,
        kInterface.addLookupHandler,
        kInterface.registerFunction,
        kInterface.movieIdentity,
        kInterface.getVariable,
        kInterface.getMember,
        kInterface.setMember,
        kInterface.invoke,
        kInterface.createFunction,
        kInterface.createObject,
        kInterface.release,
        kInterface.readPrimitive,
        kInterface.writePrimitive,
        kInterface.isObject,
        kInterface.objectIdentity,
        kInterface.addAdvanceHandler,
        kInterface.watchAdvance,
        kInterface.unwatchAdvance,
    };
    return &kInterfaceV1;
}

namespace {

// Calls movie handlers whose swfName matches a loaded movie and that were not
// told about it yet.
void NotifyMovieLoaded(const MovieRootContext& context, std::uintptr_t identity) noexcept {
    const auto count = movieHandlerCount.load(std::memory_order_acquire);
    if (count == 0) {
        return;
    }
    char url[XSCAL_SCALEFORM_MOVIE_URL_CAPACITY];
    std::uint32_t newlyMatched{};
    {
        std::scoped_lock lock{movieMutex};
        auto* record = FindOrCreateMovieRecord(context.movieRoot, identity);
        if (record == nullptr || !record->loaded ||
            record->checkedHandlers == count) {
            return;
        }
        CopyText(url, sizeof(url), record->url);
        const auto firstUnchecked = record->checkedHandlers;
        record->checkedHandlers = count;
        for (std::uint32_t index = firstUnchecked; index < count; ++index) {
            const auto bit = std::uint32_t{1} << index;
            const auto& handler = movieHandlers[index];
            if ((record->notifiedHandlers & bit) == 0 &&
                MatchesSwfName(handler.matchesAllMovies ? nullptr : handler.swfName, url)) {
                record->notifiedHandlers |= bit;
                newlyMatched |= bit;
            }
        }
    }
    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& handler = movieHandlers[index];
        if ((newlyMatched & (std::uint32_t{1} << index)) == 0 ||
            handler.loaded == nullptr ||
            handler.disabled.load(std::memory_order_relaxed)) {
            continue;
        }
        bool completed{};
        {
            DepthScope depth;
            completed = CallMovieCallback(handler.loaded, handler.userData, &movie);
        }
        if (!completed) {
            handler.disabled.store(true, std::memory_order_relaxed);
            DiagnosticLogFormat(
                "Scaleform movie interface: plugin movie handler %u raised an exception and was disabled",
                static_cast<unsigned int>(index));
        }
    }
}

void NotifyMovieUnloading(const MovieRootContext& context, std::uintptr_t identity) noexcept {
    std::uint32_t notified{};
    {
        std::scoped_lock lock{movieMutex};
        const auto* record = FindMovieRecord(context.movieRoot);
        if (record == nullptr || record->identity != identity || !record->loaded) {
            return;
        }
        notified = record->notifiedHandlers;
    }
    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < movieHandlers.size(); ++index) {
        auto& handler = movieHandlers[index];
        if ((notified & (std::uint32_t{1} << index)) == 0 ||
            handler.unloading == nullptr ||
            handler.disabled.load(std::memory_order_relaxed)) {
            continue;
        }
        bool completed{};
        {
            DepthScope depth;
            completed = CallMovieCallback(handler.unloading, handler.userData, &movie);
        }
        if (!completed) {
            handler.disabled.store(true, std::memory_order_relaxed);
            DiagnosticLogFormat(
                "Scaleform movie interface: plugin movie handler %u raised an exception and was disabled",
                static_cast<unsigned int>(index));
        }
    }
}

// Reads root1.loaderInfo.url once root1 exists. A movie whose URL cannot be
// read counts as loaded with an empty URL.
void DetectMovieLoad(const MovieRootContext& context, std::uintptr_t identity) noexcept {
    {
        std::scoped_lock lock{movieMutex};
        auto* record = FindOrCreateMovieRecord(context.movieRoot, identity);
        if (record == nullptr || record->loaded) {
            return;
        }
        const auto attempt = record->frameAttempts++;
        if (attempt >= kEveryFrameLoadProbes && (attempt & kSlowLoadProbeMask) != 0) {
            return;
        }
    }
    if (!HasScaleformObject(context, "root1")) {
        return;
    }
    std::string url;
    if (!GetScaleformString(context, "root1.loaderInfo.url", url)) {
        url.clear();
    }
    std::scoped_lock lock{movieMutex};
    auto* record = FindOrCreateMovieRecord(context.movieRoot, identity);
    if (record == nullptr) {
        return;
    }
    CopyText(record->url, sizeof(record->url), url);
    record->loaded = true;
}

bool AdvanceMovieVisitor(const MovieRootContext& context, void*) noexcept {
    const auto identity = MakeIdentity(context.movieRoot);
    RunPostedWork(context, identity);
    DetectMovieLoad(context, identity);
    NotifyMovieLoaded(context, identity);
    if (IsAdvanceWatched(context.movieRoot)) {
        NotifyAdvance(context);
    }
    return true;
}

bool ReleaseMovieVisitor(const MovieRootContext& context, void* userData) noexcept {
    const auto identity = *static_cast<const std::uintptr_t*>(userData);
    // Delivered at any depth: a plugin callback can make the game close the
    // menu, and a plugin that saw loaded() relies on unloading().
    NotifyMovieUnloading(context, identity);
    DropRefsOfMovie(identity, &context);
    return true;
}

struct InvokeQuery final {
    const char* methodPath;
    const std::byte* arguments;
    std::uint32_t argumentCount;
    bool allowed;
};

bool InvokeFilterVisitor(const MovieRootContext& context, void* userData) noexcept {
    auto& query = *static_cast<InvokeQuery*>(userData);
    if (query.argumentCount > XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS) {
        return true;
    }
    char url[XSCAL_SCALEFORM_MOVIE_URL_CAPACITY];
    CopyMovieUrl(context.movieRoot, url);

    const auto count = invokeFilterCount.load(std::memory_order_acquire);
    const auto matches = [&](const InvokeFilter& entry) {
        return !entry.disabled.load(std::memory_order_relaxed) &&
            MatchesSwfName(entry.matchesAllMovies ? nullptr : entry.swfName, url) &&
            (entry.matchesAllMethods ||
             std::strcmp(entry.methodPath, query.methodPath) == 0);
    };
    bool anyMatch{};
    for (std::uint32_t index = 0; index < count && !anyMatch; ++index) {
        anyMatch = matches(invokeFilters[index]);
    }
    if (!anyMatch) {
        return true;
    }

    std::array<XScalMovieValue, XSCAL_SCALEFORM_MOVIE_MAX_CALL_ARGUMENTS> views{};
    for (std::uint32_t index = 0; index < query.argumentCount; ++index) {
        MakeBorrowedView(
            query.arguments + index * layout::kValueArrayStride, false, views[index]);
    }
    const auto before = views;

    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& entry = invokeFilters[index];
        if (!matches(entry)) {
            continue;
        }
        int32_t verdict{1};
        bool completed{};
        {
            DepthScope depth;
            completed = CallInvokeFilter(
                entry,
                &movie,
                query.methodPath,
                query.argumentCount != 0 ? views.data() : nullptr,
                query.argumentCount,
                verdict);
        }
        if (!completed) {
            entry.disabled.store(true, std::memory_order_relaxed);
            DiagnosticLogFormat(
                "Scaleform movie interface: plugin invoke filter %u raised an exception and was disabled",
                static_cast<unsigned int>(index));
            continue;
        }
        if (verdict == 0) {
            query.allowed = false;
        }
    }

    // Plugins may replace only a primitive argument that is not a string. The
    // game's array holds 0x30-byte records.
    auto* gameRecords = const_cast<std::byte*>(query.arguments);
    for (std::uint32_t index = 0; index < query.argumentCount; ++index) {
        const auto& original = AsValue(&before[index]);
        const auto& changed = AsValue(&views[index]);
        if (std::memcmp(original.storage, changed.storage, layout::kValueSize) == 0 ||
            BaseType(original) > layout::kNumberType || IsOwned(original) ||
            BaseType(changed) > layout::kNumberType || IsOwned(changed)) {
            continue;
        }
        std::memcpy(
            gameRecords + index * layout::kValueArrayStride,
            changed.storage,
            layout::kValueArrayStride);
    }
    return true;
}

}  // namespace

bool MatchesSwfName(const char* swfName, const char* url) noexcept {
    if (swfName == nullptr) {
        return true;
    }
    std::string_view fileName{url == nullptr ? "" : url};
    const auto separator = fileName.find_last_of("/\\");
    if (separator != std::string_view::npos) {
        fileName.remove_prefix(separator + 1);
    }
    return EqualsAsciiIgnoreCase(std::string_view{swfName}, fileName);
}

void NotifyLookup(
    const MovieRootContext& context,
    const char* path) noexcept {
    if (pluginDepth != 0 || path == nullptr) {
        return;
    }
    const auto count = lookupHandlerCount.load(std::memory_order_acquire);
    if (count == 0) {
        return;
    }
    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& entry = lookupHandlers[index];
        if (entry.disabled.load(std::memory_order_relaxed)) {
            continue;
        }
        bool completed{};
        {
            DepthScope depth;
            completed =
                CallLookupHandler(entry.handler, entry.userData, &movie, path);
        }
        if (!completed) {
            entry.disabled.store(true, std::memory_order_relaxed);
            DiagnosticLogFormat(
                "Scaleform movie interface: plugin lookup handler %u raised an exception and was disabled",
                static_cast<unsigned int>(index));
        }
    }
}

namespace {

// Passes one MovieRoot::AdvanceFrame call to the plugin advance handlers.
void NotifyAdvance(const MovieRootContext& context) noexcept {
    if (pluginDepth != 0) {
        return;
    }
    const auto count = advanceHandlerCount.load(std::memory_order_acquire);
    XScalMovie movie{&context};
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& entry = advanceHandlers[index];
        if (entry.disabled.load(std::memory_order_relaxed)) {
            continue;
        }
        bool completed{};
        {
            DepthScope depth;
            completed = CallMovieCallback(entry.handler, entry.userData, &movie);
        }
        if (!completed) {
            entry.disabled.store(true, std::memory_order_relaxed);
            DiagnosticLogFormat(
                "Scaleform movie interface: plugin advance handler %u raised an exception and was disabled",
                static_cast<unsigned int>(index));
        }
    }
}

bool IsAdvanceWatched(void* movieRoot) noexcept {
    if (watchedMovieCount.load(std::memory_order_acquire) == 0) {
        return false;
    }
    for (const auto& entry : watchedMovies) {
        if (entry.movieRoot.load(std::memory_order_relaxed) == movieRoot) {
            return true;
        }
    }
    return false;
}

}  // namespace

void AdvanceMovie(void* movieRoot) noexcept {
    // Invoke filters read the URL of the movie, so they need load detection too.
    if (pluginDepth != 0 ||
        (movieHandlerCount.load(std::memory_order_acquire) == 0 &&
         invokeFilterCount.load(std::memory_order_acquire) == 0 &&
         postedCount.load(std::memory_order_acquire) == 0 &&
         watchedMovieCount.load(std::memory_order_acquire) == 0)) {
        return;
    }
    (void)VisitMovieRoot(movieRoot, &AdvanceMovieVisitor, nullptr);
}

void ReleaseMovie(void* movieRoot) noexcept {
    if (movieRoot == nullptr) {
        return;
    }
    const auto identity = MakeIdentity(movieRoot);
    bool loaded{};
    {
        std::scoped_lock lock{movieMutex};
        const auto* record = FindMovieRecord(movieRoot);
        loaded = record != nullptr && record->loaded;
    }
    if (loaded || liveRefCount.load(std::memory_order_relaxed) != 0) {
        auto visitedIdentity = identity;
        (void)VisitMovieRoot(movieRoot, &ReleaseMovieVisitor, &visitedIdentity);
    }
    // Anything the visit could not reach is forgotten without a release call.
    DropRefsOfMovie(identity, nullptr);
    DropPostedWork(identity);
    std::scoped_lock lock{movieMutex};
    if (auto* record = FindMovieRecord(movieRoot); record != nullptr) {
        *record = {};
    }
}

bool AllowInvoke(
    void* movieRoot,
    const char* methodPath,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept {
    if (pluginDepth != 0 || methodPath == nullptr ||
        invokeFilterCount.load(std::memory_order_acquire) == 0 ||
        (argumentCount != 0 && arguments == nullptr)) {
        return true;
    }
    InvokeQuery query{
        methodPath,
        reinterpret_cast<const std::byte*>(arguments),
        argumentCount,
        true};
    (void)VisitMovieRoot(movieRoot, &InvokeFilterVisitor, &query);
    return query.allowed;
}

void ForgetMovie(void* movieRoot) noexcept {
    if (movieRoot == nullptr) {
        return;
    }
    try {
        std::scoped_lock lock{generationMutex};
        ++movieGenerations[movieRoot];
    } catch (...) {
    }
    if (watchedMovieCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::scoped_lock lock{watchMutex};
    if (auto* entry = FindWatch(movieRoot); entry != nullptr) {
        ClearWatch(*entry);
    }
}

void SetAdvanceAvailable(bool available) noexcept {
    advanceAvailable.store(available, std::memory_order_release);
}

void SetInvokeAvailable(bool available) noexcept {
    invokeAvailable.store(available, std::memory_order_release);
}

void ResetForTests() noexcept {
    std::scoped_lock lock{registrationMutex};
    advanceHandlerCount.store(0, std::memory_order_release);
    lookupHandlerCount.store(0, std::memory_order_release);
    functionCount.store(0, std::memory_order_release);
    movieHandlerCount.store(0, std::memory_order_release);
    invokeFilterCount.store(0, std::memory_order_release);
    {
        std::scoped_lock movieLock{movieMutex};
        movies.fill({});
    }
    {
        std::scoped_lock refLock{refMutex};
        refs.fill({});
        liveRefCount.store(0, std::memory_order_relaxed);
    }
    {
        std::scoped_lock postLock{postMutex};
        postedUsed = 0;
        postedCount.store(0, std::memory_order_release);
    }
    pluginDepth = 0;
    std::scoped_lock watchLock{watchMutex};
    for (auto& entry : watchedMovies) {
        entry.movieRoot.store(nullptr, std::memory_order_relaxed);
        entry.references = 0;
    }
    watchedMovieCount.store(0, std::memory_order_release);
}

}  // namespace sf::movieapi
