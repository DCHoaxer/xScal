#include "hook/movie_root_router.hpp"

#include "api/scaleform_movie_interface.hpp"
#include "modules/core/core_module.hpp"
#include "platform/diagnostics.hpp"
#include "platform/windows_memory.hpp"
#include "scaleform/memory_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string_view>
#include <utility>

namespace sf::routing {
namespace {

[[nodiscard]] bool HasRootPrefix(std::string_view path) noexcept {
    return path.starts_with("root1.") || path.starts_with("root.") || path.starts_with("_root.");
}

}

bool IsObjectAlias(std::string_view path) noexcept {
    constexpr std::string_view suffix = ".__SFCodeObj";
    if (path == "__SFCodeObj" ||
        path == "root1.__SFCodeObj" ||
        path == "root1.FilterHolder_mc.Menu_mc.__SFCodeObj" ||
        path == "root1.Menu_mc.__SFCodeObj") {
        return true;
    }
    return HasRootPrefix(path) && path.size() > suffix.size() && path.ends_with(suffix);
}

bool IsDirectCallAlias(std::string_view path) noexcept {
    constexpr std::string_view callSuffix = ".call";
    return path.size() > callSuffix.size() && path.ends_with(callSuffix) &&
        IsObjectAlias(path.substr(0, path.size() - callSuffix.size()));
}

bool IsModStorageAlias(std::string_view path) noexcept {
    constexpr std::string_view suffixes[]{
        ".modStorage",
        ".modStorage.register",
        ".modStorage.save",
        ".modStorage.load",
    };
    for (const auto suffix : suffixes) {
        if (path.size() > suffix.size() && path.ends_with(suffix) &&
            IsObjectAlias(path.substr(0, path.size() - suffix.size()))) {
            return true;
        }
    }
    return false;
}

bool IsChatInterfaceAlias(std::string_view path) noexcept {
    constexpr std::string_view interfaceSuffix = ".chatInterface";
    constexpr std::string_view methodNames[]{
        "getRuntimeInfo",
        "connect",
        "getAuthState",
        "getConnectionState",
        "sendMessage",
        "disconnect",
        "reportMessage",
        "moderationAction",
        "logout",
        "clearChatAuth",
        "pollEvents",
    };
    const auto interfaceOffset = path.rfind(interfaceSuffix);
    if (interfaceOffset == std::string_view::npos ||
        !IsObjectAlias(path.substr(0, interfaceOffset))) {
        return false;
    }
    const auto remainder = path.substr(
        interfaceOffset + interfaceSuffix.size());
    if (remainder.empty()) {
        return true;
    }
    if (!remainder.starts_with('.') || remainder.size() == 1) {
        return false;
    }
    const auto method = remainder.substr(1);
    for (const auto name : methodNames) {
        if (method == name) {
            return true;
        }
    }
    return false;
}

bool ShouldAttachRootBridge(std::string_view path) noexcept {
    return path == "root1";
}

}

namespace sf {
namespace {

std::atomic<std::uint64_t> nextRouteGeneration{1};
thread_local bool insideMovieRootHook = false;

struct MovieRootValidationCache final {
    std::uint64_t routeGeneration{};
    const void* movieRoot{};
    const void* expectedVtable{};
    modules::CoreModule coreModule{};
};

constexpr std::size_t movieRootValidationCacheSize = 8;
thread_local std::array<
    MovieRootValidationCache,
    movieRootValidationCacheSize> validatedMovieRoots{};
thread_local std::size_t nextMovieRootValidationCacheSlot{};

// Unreadable: the movie root memory cannot be read, so nothing may be forwarded.
// UnexpectedVtable: the movie root is readable but is not the hooked MovieRoot class.
enum class MovieRootValidation {
    Unreadable,
    UnexpectedVtable,
    Expected,
};

[[nodiscard]] bool TryReadPath(
    const char* path,
    std::string_view& result) noexcept {
    constexpr std::size_t maximumPathLength = 4096;
    result = {};
    if (path == nullptr) {
        return false;
    }

    std::size_t length{};
    __try {
        while (length < maximumPathLength && path[length] != '\0') {
            ++length;
        }
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    if (length == 0 || length == maximumPathLength) {
        return false;
    }
    result = std::string_view{path, length};
    return true;
}

[[nodiscard]] bool TryReadMovieRootVtable(
    const void* movieRoot,
    void*& result) noexcept {
    result = nullptr;
    if (movieRoot == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&result, movieRoot, sizeof(result));
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        result = nullptr;
        return false;
    }
    return true;
}

[[nodiscard]] bool IsReadableMovieRoot(
    const VtableHookPlatform& platform,
    const void* movieRoot) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return platform.virtualQuery(movieRoot, &memory, sizeof(memory)) != 0 &&
        memory.State == MEM_COMMIT &&
        platform::IsReadableProtection(memory.Protect) &&
        platform::RegionContains(memory, movieRoot, sizeof(void*));
}

class RecursionGuard final {
public:
    RecursionGuard() noexcept { insideMovieRootHook = true; }
    ~RecursionGuard() { insideMovieRootHook = false; }
};

// Checks movieRoot against the expected vtable. A cached entry skips the
// VirtualQuery call but still reads the vtable on every check.
[[nodiscard]] MovieRootValidation ValidateMovieRoot(
    const VtableHookPlatform& platform,
    std::uint64_t routeGeneration,
    const void* movieRoot,
    const void* expectedVtable,
    MovieRootValidationCache*& acceptedEntry) noexcept {
    acceptedEntry = nullptr;
    if (movieRoot == nullptr) {
        return MovieRootValidation::Unreadable;
    }

    const auto cachedEntry = std::find_if(
        validatedMovieRoots.begin(),
        validatedMovieRoots.end(),
        [routeGeneration, movieRoot, expectedVtable](
            const MovieRootValidationCache& entry) {
            return entry.routeGeneration == routeGeneration &&
                entry.movieRoot == movieRoot &&
                entry.expectedVtable == expectedVtable;
        });
    const bool cached = cachedEntry != validatedMovieRoots.end();
    if (!cached && !IsReadableMovieRoot(platform, movieRoot)) {
        return MovieRootValidation::Unreadable;
    }

    void* incomingVtable{};
    if (!TryReadMovieRootVtable(movieRoot, incomingVtable)) {
        if (cached) {
            *cachedEntry = {};
        }
        return MovieRootValidation::Unreadable;
    }
    if (incomingVtable != expectedVtable) {
        if (cached) {
            *cachedEntry = {};
        }
        return MovieRootValidation::UnexpectedVtable;
    }
    if (cached) {
        acceptedEntry = &*cachedEntry;
        return MovieRootValidation::Expected;
    }

    auto destination = std::find_if(
        validatedMovieRoots.begin(),
        validatedMovieRoots.end(),
        [](const MovieRootValidationCache& entry) {
            return entry.routeGeneration == 0;
        });
    if (destination == validatedMovieRoots.end()) {
        destination = validatedMovieRoots.begin() +
            nextMovieRootValidationCacheSlot;
        nextMovieRootValidationCacheSlot =
            (nextMovieRootValidationCacheSlot + 1) %
            validatedMovieRoots.size();
    }
    *destination = {routeGeneration, movieRoot, expectedVtable};
    acceptedEntry = &*destination;
    return MovieRootValidation::Expected;
}

// Retries `path` after the canonical bridge was attached. If that also misses,
// tries `canonicalPath` unless the caller already requested it.
[[nodiscard]] bool RetryWithCanonicalPath(
    MovieRootGetVariable original,
    void* movieRoot,
    ScaleformValue* outValue,
    const char* path,
    unsigned int callerR9Scratch,
    std::string_view requestedPath,
    const char* canonicalPath) noexcept {
    if (original(movieRoot, outValue, path, callerR9Scratch)) {
        return true;
    }
    return requestedPath != canonicalPath &&
        original(movieRoot, outValue, canonicalPath, 0U);
}

}

MovieRootRouteState::MovieRootRouteState(
    std::shared_ptr<NativeFunctionHandler> functionHandler,
    VtableHookPlatform platform,
    MovieRootGetVariable original,
    void* expectedVtable,
    ResolvedScaleformApi api,
    TargetKind targetKind,
    const config::ScaleformConfig& scaleformConfig) noexcept
    : platform_{platform},
      functionHandler_{std::move(functionHandler)},
      original_{original},
      expectedVtable_{expectedVtable},
      api_{api},
      validationGeneration_{
          nextRouteGeneration.fetch_add(1, std::memory_order_relaxed)},
      attachments_{
          original_,
          api_,
          functionHandler_.get(),
          targetKind,
          scaleformConfig} {}

bool MovieRootRouteState::VisitMovieRoot(
    void* movieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept {
    MovieRootValidationCache* validationEntry{};
    if (visitor == nullptr ||
        ValidateMovieRoot(
            platform_,
            validationGeneration_,
            movieRoot,
            expectedVtable_,
            validationEntry) != MovieRootValidation::Expected) {
        return false;
    }
    const MovieRootContext context{
        movieRoot,
        original_,
        api_,
        functionHandler_.get()};
    return visitor(context, userData);
}

bool MovieRootRouteState::VisitTrackedMovieRoots(
    void* preferredMovieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept {
    if (visitor == nullptr) {
        return false;
    }

    BridgeAttachmentService::TrackedMovieRoots candidates;
    if (!attachments_.CopyTrackedMovieRoots(
            preferredMovieRoot, candidates)) {
        return false;
    }
    for (void* candidate : candidates) {
        MovieRootValidationCache* validationEntry{};
        if (ValidateMovieRoot(
                platform_,
                validationGeneration_,
                candidate,
                expectedVtable_,
                validationEntry) != MovieRootValidation::Expected) {
            continue;
        }
        const MovieRootContext context{
            candidate,
            original_,
            api_,
            functionHandler_.get()};
        if (visitor(context, userData)) {
            return true;
        }
    }
    return false;
}

bool MovieRootRouteState::Route(
    void* movieRoot,
    ScaleformValue* outValue,
    const char* path,
    unsigned int callerR9Scratch) noexcept {
    const auto original = original_;
    if (movieRoot == nullptr) {
        return false;
    }
    if (insideMovieRootHook) {
        return original(movieRoot, outValue, path, callerR9Scratch);
    }
    MovieRootValidationCache* validationEntry{};
    switch (ValidateMovieRoot(
        platform_,
        validationGeneration_,
        movieRoot,
        expectedVtable_,
        validationEntry)) {
        case MovieRootValidation::Unreadable:
            return false;
        case MovieRootValidation::UnexpectedVtable:
            return original(movieRoot, outValue, path, callerR9Scratch);
        case MovieRootValidation::Expected:
            break;
    }
    // Without an output value or a path there is nothing to route; the game decides.
    if (outValue == nullptr || path == nullptr) {
        return original(movieRoot, outValue, path, callerR9Scratch);
    }

    RecursionGuard recursionGuard;
    const bool originalResult =
        original(movieRoot, outValue, path, callerR9Scratch);

    const MovieRootContext validationContext{
        movieRoot,
        original_,
        api_,
        functionHandler_.get()};
    validationEntry->coreModule.Update(validationContext);
    // ExamineMenu and PLM component shells may appear after an earlier lookup.
    attachments_.ProbeComponents(movieRoot);
    std::string_view lookupPath;
    if (TryReadPath(path, lookupPath)) {
        movieapi::NotifyLookup(validationContext, path);
    }

    std::string_view requestedPath;
    const bool routedPath = (path[0] == 'r' || path[0] == '_') &&
        TryReadPath(path, requestedPath);
    if (!routedPath) {
        return originalResult;
    }

    const auto componentPathMatch =
        attachments_.MatchComponentPath(requestedPath);
    const bool configuredComponentPath = componentPathMatch !=
        BridgeAttachmentService::ComponentPathMatch::None;
    const bool rootAttachmentPath =
        routing::ShouldAttachRootBridge(requestedPath);
    if (!configuredComponentPath && !rootAttachmentPath &&
        requestedPath.find("__SFCodeObj") == std::string_view::npos) {
        return originalResult;
    }

    if (configuredComponentPath) {
        attachments_.AttachConfiguredComponent(
            movieRoot,
            requestedPath,
            originalResult &&
                    componentPathMatch ==
                        BridgeAttachmentService::ComponentPathMatch::Exact
                ? outValue
                : nullptr);
    }
    if (originalResult &&
        componentPathMatch ==
            BridgeAttachmentService::ComponentPathMatch::Exact) {
        return true;
    }
    if (originalResult && rootAttachmentPath) {
        (void)attachments_.AttachRoot(movieRoot, true);
    }

    const bool directCall = routing::IsDirectCallAlias(requestedPath);
    const bool directStorage = routing::IsModStorageAlias(requestedPath);
    const bool directChatInterface =
        routing::IsChatInterfaceAlias(requestedPath);
    const bool objectAlias = routing::IsObjectAlias(requestedPath);
    if (!directCall && !directStorage && !directChatInterface &&
        !objectAlias) {
        return originalResult;
    }

    MovieRootContext context{
        movieRoot,
        original,
        api_,
        functionHandler_.get()};
    if (directChatInterface) {
        constexpr std::string_view interfaceSuffix = ".chatInterface";
        const auto interfaceOffset = requestedPath.rfind(interfaceSuffix);
        std::array<char, 4096> codeObjectPath{};
        std::copy_n(
            requestedPath.data(),
            interfaceOffset,
            codeObjectPath.data());

        ScaleformValue codeObject{};
        const bool codeObjectReadable = original(
            movieRoot, &codeObject, codeObjectPath.data(), 0U);
        const bool codeObjectFound =
            IsObjectReadback(codeObjectReadable, codeObject);
        const bool attached = codeObjectFound &&
            attachments_.AttachChatInterface(movieRoot, codeObject);
        ReleaseValue(context, codeObject);
        if (originalResult) {
            return true;
        }
        if (!attached) {
            return false;
        }
        if (original(movieRoot, outValue, path, callerR9Scratch)) {
            return true;
        }

        constexpr std::string_view canonicalCodeObject{
            "root1.__SFCodeObj"};
        if (std::string_view{codeObjectPath.data()} == canonicalCodeObject) {
            return false;
        }
        ScaleformValue canonical{};
        const bool canonicalReadable = original(
            movieRoot,
            &canonical,
            canonicalCodeObject.data(),
            0U);
        const bool canonicalFound =
            IsObjectReadback(canonicalReadable, canonical);
        const bool canonicalAttached = canonicalFound &&
            attachments_.AttachChatInterface(movieRoot, canonical);
        ReleaseValue(context, canonical);
        if (!canonicalAttached) {
            return false;
        }
        std::array<char, 4096> canonicalPath{};
        const auto suffix = requestedPath.substr(interfaceOffset);
        std::copy(
            canonicalCodeObject.begin(),
            canonicalCodeObject.end(),
            canonicalPath.begin());
        std::copy(
            suffix.begin(),
            suffix.end(),
            canonicalPath.begin() + canonicalCodeObject.size());
        return original(
            movieRoot,
            outValue,
            canonicalPath.data(),
            0U);
    }

    if (directCall) {
        if (originalResult && attachments_.IsAuthoritativeCall(movieRoot, *outValue)) {
            return true;
        }

        constexpr std::size_t callSuffixLength = 5;
        std::array<char, 4096> bridgePath{};
        const std::size_t bridgePathLength =
            requestedPath.size() - callSuffixLength;
        std::copy_n(
            requestedPath.data(),
            bridgePathLength,
            bridgePath.data());

        ScaleformValue requestedBridge{};
        const bool bridgeReadable = original(
            movieRoot, &requestedBridge, bridgePath.data(), 0U);
        const bool requestedBridgeFound =
            IsObjectReadback(bridgeReadable, requestedBridge);
        BridgeAttachmentResult attachment{};
        if (requestedBridgeFound) {
            attachment = attachments_.AttachResolved(
                movieRoot, requestedBridge);
        }
        ReleaseValue(context, requestedBridge);
        // A contended lock keeps the game's result, which outValue still holds.
        // Other attach failures return false as before.
        if (requestedBridgeFound && !attachment.ready) {
            return attachment.contended && originalResult;
        }
        if (attachment.ready) {
            if (originalResult) {
                ReleaseValue(context, *outValue);
                *outValue = {};
            }
            if (original(movieRoot, outValue, path, callerR9Scratch)) {
                return true;
            }
        }
        const auto rootStatus = attachments_.AttachRoot(movieRoot, true);
        if (rootStatus != RootAttachStatus::Attached) {
            // A ready bridge already replaced outValue, so only an unchanged value can be returned.
            return rootStatus == RootAttachStatus::Contended &&
                originalResult && !attachment.ready;
        }
        return original(
            movieRoot,
            outValue,
            "root1.__SFCodeObj.call",
            0U);
    }

    if (directStorage) {
        if (originalResult) {
            return true;
        }
        if (attachments_.AttachRoot(movieRoot, true) !=
            RootAttachStatus::Attached) {
            return false;
        }
        const char* canonical = "root1.__SFCodeObj.modStorage";
        if (requestedPath.ends_with(".modStorage.register")) {
            canonical = "root1.__SFCodeObj.modStorage.register";
        } else if (requestedPath.ends_with(".modStorage.save")) {
            canonical = "root1.__SFCodeObj.modStorage.save";
        } else if (requestedPath.ends_with(".modStorage.load")) {
            canonical = "root1.__SFCodeObj.modStorage.load";
        }
        return RetryWithCanonicalPath(
            original,
            movieRoot,
            outValue,
            path,
            callerR9Scratch,
            requestedPath,
            canonical);
    }

    if (originalResult) {
        (void)attachments_.AttachResolved(movieRoot, *outValue);
        return true;
    }

    if (attachments_.AttachRoot(movieRoot, true) !=
        RootAttachStatus::Attached) {
        return false;
    }
    return RetryWithCanonicalPath(
        original,
        movieRoot,
        outValue,
        path,
        callerR9Scratch,
        requestedPath,
        "root1.__SFCodeObj");
}

}
