#include "hook/movie_root_hook.hpp"

#include "api/scaleform_callbacks.hpp"
#include "api/mod_storage.hpp"
#include "api/scaleform_movie_interface.hpp"
#include "hook/movie_root_router.hpp"
#include "platform/diagnostics.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace sf {
namespace {

std::atomic<std::shared_ptr<MovieRootRouteState>> activeRouteState;

// Set at every install and never cleared. A failed restore can leave the hook in
// the vtable slot with no active route, and the hook then forwards to this pointer.
std::atomic<MovieRootGetVariable> installedOriginalGetVariable{};
std::atomic<MovieRootAdvanceFrame> installedOriginalAdvanceFrame{};
std::atomic<MovieRootDestroy> installedOriginalDestroy{};
std::atomic<MovieRootInvoke> installedOriginalInvoke{};

void ClearActiveRouteState(
    const std::shared_ptr<MovieRootRouteState>& routeState) noexcept {
    auto expectedRoute = routeState;
    (void)activeRouteState.compare_exchange_strong(
        expectedRoute,
        {},
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

}

MovieRootHookController::MovieRootHookController(
    ScaleformCallbackRouter& callbackRouter,
    VtableHookPlatform platform) noexcept
    : callbackRouter_{callbackRouter},
      platform_{platform},
      vtableHook_{platform},
      destroyHook_{platform},
      advanceHook_{platform},
      invokeHook_{platform} {
    try {
        functionHandler_ = std::make_shared<NativeFunctionHandler>();
    } catch (...) {
    }
}

MovieRootHookController::~MovieRootHookController() {
    VtableHookStatus status = VtableHookStatus::NotActive;
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        status = Restore();
        if (!vtableHook_.IsActive() &&
            status != VtableHookStatus::ProtectionRestoreFailed) {
            break;
        }
    }

    ClearActiveRouteState(routeState_);
    if (callbackRouterPublished_) {
        ClearNativeCallbackRouter(callbackRouter_);
        callbackRouterPublished_ = false;
    }
    routeState_.reset();
}

VtableHookStatus MovieRootHookController::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase,
    const config::ScaleformConfig& scaleformConfig) noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    if (vtableHook_.IsActive()) {
        return VtableHookStatus::AlreadyActive;
    }
    // Rejecting here keeps a second controller from touching the callback router
    // that the active controller published.
    if (activeRouteState.load(std::memory_order_acquire) != nullptr) {
        return VtableHookStatus::AlreadyActive;
    }
    if (platform_.virtualQuery == nullptr) {
        return VtableHookStatus::InvalidPlatform;
    }
    // The vtable address is the slot address minus this offset, so a smaller
    // slot offset would underflow it.
    if (profile.primaryGetVariableSlotOffset <
        layout::kGetVariableVtableOffset) {
        return VtableHookStatus::InvalidArgument;
    }

    ResolvedScaleformApi preparedApi{};
    const auto apiStatus = ResolveScaleformApi(
        profile,
        moduleBase,
        platform_.virtualQuery,
        preparedApi);
    if (apiStatus == ResolveScaleformApiStatus::AddressOverflow) {
        return VtableHookStatus::AddressOverflow;
    }
    if (apiStatus != ResolveScaleformApiStatus::Resolved) {
        return VtableHookStatus::TargetRoutineNotExecutable;
    }
    if (functionHandler_ == nullptr) {
        return VtableHookStatus::AllocationFailed;
    }
    if (!PublishNativeCallbackRouter(callbackRouter_)) {
        return VtableHookStatus::AlreadyActive;
    }

    const auto status = vtableHook_.Install(
        profile,
        moduleBase,
        &HookedMovieRootGetVariable);
    if (status != VtableHookStatus::Installed) {
        ClearNativeCallbackRouter(callbackRouter_);
        return status;
    }

    // The route needs the original that the vtable install just read from the slot.
    const auto originalGetVariable = vtableHook_.Original();
    installedOriginalGetVariable.store(
        originalGetVariable,
        std::memory_order_release);
    const auto expectedVtable = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(vtableHook_.SlotAddress()) -
        layout::kGetVariableVtableOffset);

    std::shared_ptr<MovieRootRouteState> candidate;
    try {
        candidate = std::make_shared<MovieRootRouteState>(
            functionHandler_,
            platform_,
            originalGetVariable,
            expectedVtable,
            preparedApi,
            profile.kind,
            scaleformConfig);
    } catch (...) {
        (void)vtableHook_.Restore();
        ClearNativeCallbackRouter(callbackRouter_);
        return VtableHookStatus::AllocationFailed;
    }

    std::shared_ptr<MovieRootRouteState> expectedRoute;
    if (!activeRouteState.compare_exchange_strong(
            expectedRoute,
            candidate,
            std::memory_order_release,
            std::memory_order_acquire)) {
        (void)vtableHook_.Restore();
        ClearNativeCallbackRouter(callbackRouter_);
        return VtableHookStatus::AlreadyActive;
    }
    routeState_ = candidate;
    callbackRouterPublished_ = true;

    const auto vtableOffset =
        profile.primaryGetVariableSlotOffset - layout::kGetVariableVtableOffset;
    const auto destroyStatus = destroyHook_.InstallSlot(
        vtableOffset + layout::kDestroyVtableOffset,
        moduleBase,
        reinterpret_cast<void*>(&HookedMovieRootDestroy));
    if (destroyStatus == VtableHookStatus::Installed) {
        installedOriginalDestroy.store(
            reinterpret_cast<MovieRootDestroy>(
                reinterpret_cast<void*>(destroyHook_.Original())),
            std::memory_order_release);
    }
    if (destroyStatus != VtableHookStatus::Installed) {
        DiagnosticLogFormat(
            "xScal MovieRoot destructor hook: %s",
            VtableHookStatusName(destroyStatus));
        return VtableHookStatus::Installed;
    }

    const auto advanceStatus = advanceHook_.InstallSlot(
        vtableOffset + layout::kAdvanceFrameVtableOffset,
        moduleBase,
        reinterpret_cast<void*>(&HookedMovieRootAdvanceFrame));
    if (advanceStatus == VtableHookStatus::Installed) {
        installedOriginalAdvanceFrame.store(
            reinterpret_cast<MovieRootAdvanceFrame>(
                reinterpret_cast<void*>(advanceHook_.Original())),
            std::memory_order_release);
        movieapi::SetAdvanceAvailable(true);
    } else {
        DiagnosticLogFormat(
            "xScal MovieRoot AdvanceFrame hook: %s",
            VtableHookStatusName(advanceStatus));
    }

    const auto invokeStatus = invokeHook_.InstallSlot(
        vtableOffset + layout::kInvokeVtableOffset,
        moduleBase,
        reinterpret_cast<void*>(&HookedMovieRootInvoke));
    if (invokeStatus == VtableHookStatus::Installed) {
        installedOriginalInvoke.store(
            reinterpret_cast<MovieRootInvoke>(
                reinterpret_cast<void*>(invokeHook_.Original())),
            std::memory_order_release);
        movieapi::SetInvokeAvailable(true);
    } else {
        DiagnosticLogFormat(
            "xScal MovieRoot Invoke hook: %s",
            VtableHookStatusName(invokeStatus));
    }
    return VtableHookStatus::Installed;
}

VtableHookStatus MovieRootHookController::Restore() noexcept {
    std::scoped_lock lock{lifecycleMutex_};
    if (invokeHook_.IsActive()) {
        movieapi::SetInvokeAvailable(false);
        (void)invokeHook_.Restore();
    }
    if (advanceHook_.IsActive()) {
        movieapi::SetAdvanceAvailable(false);
        (void)advanceHook_.Restore();
    }
    if (destroyHook_.IsActive()) {
        (void)destroyHook_.Restore();
    }
    const auto status = vtableHook_.Restore();
    if (!vtableHook_.IsActive()) {
        ClearActiveRouteState(routeState_);
        if (callbackRouterPublished_) {
            ClearNativeCallbackRouter(callbackRouter_);
            callbackRouterPublished_ = false;
        }
        routeState_.reset();
        modstorage::GlobalService().Clear();
    }
    return status;
}

bool MovieRootHookController::IsActive() const noexcept {
    return vtableHook_.IsActive();
}

bool __fastcall HookedMovieRootGetVariable(
    void* movieRoot,
    ScaleformValue* outValue,
    const char* path,
    unsigned int callerR9Scratch) noexcept {
    const auto routeState =
        activeRouteState.load(std::memory_order_acquire);
    if (routeState == nullptr) {
        // A game thread can enter between the slot exchange in Install and the
        // store that follows it. The original is never cleared once stored.
        auto original =
            installedOriginalGetVariable.load(std::memory_order_acquire);
        while (original == nullptr) {
            std::this_thread::yield();
            original =
                installedOriginalGetVariable.load(std::memory_order_acquire);
        }
        return original(
            movieRoot,
            outValue,
            path,
            callerR9Scratch);
    }
    return routeState->Route(
        movieRoot,
        outValue,
        path,
        callerR9Scratch);
}

void __fastcall HookedMovieRootAdvanceFrame(
    void* movieRoot,
    bool nextFrame) noexcept {
    // A game thread can enter between the slot exchange and the store of the
    // original; the original is never cleared once stored.
    auto original = installedOriginalAdvanceFrame.load(std::memory_order_acquire);
    while (original == nullptr) {
        std::this_thread::yield();
        original = installedOriginalAdvanceFrame.load(std::memory_order_acquire);
    }
    original(movieRoot, nextFrame);
    movieapi::AdvanceMovie(movieRoot);
}

void* __fastcall HookedMovieRootDestroy(
    void* movieRoot,
    unsigned int flags) noexcept {
    auto original = installedOriginalDestroy.load(std::memory_order_acquire);
    while (original == nullptr) {
        std::this_thread::yield();
        original = installedOriginalDestroy.load(std::memory_order_acquire);
    }
    movieapi::ReleaseMovie(movieRoot);
    movieapi::ForgetMovie(movieRoot);
    return original(movieRoot, flags);
}

bool __fastcall HookedMovieRootInvoke(
    void* movieRoot,
    const char* methodPath,
    ScaleformValue* outValue,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept {
    auto original = installedOriginalInvoke.load(std::memory_order_acquire);
    while (original == nullptr) {
        std::this_thread::yield();
        original = installedOriginalInvoke.load(std::memory_order_acquire);
    }
    if (!movieapi::AllowInvoke(movieRoot, methodPath, arguments, argumentCount)) {
        return false;
    }
    return original(movieRoot, methodPath, outValue, arguments, argumentCount);
}

bool VisitMovieRoot(
    void* movieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept {
    const auto routeState =
        activeRouteState.load(std::memory_order_acquire);
    return routeState != nullptr &&
        routeState->VisitMovieRoot(movieRoot, visitor, userData);
}

bool VisitTrackedMovieRoots(
    void* preferredMovieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept {
    const auto routeState =
        activeRouteState.load(std::memory_order_acquire);
    return routeState != nullptr &&
        routeState->VisitTrackedMovieRoots(
            preferredMovieRoot,
            visitor,
            userData);
}

}
