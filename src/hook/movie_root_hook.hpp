#pragma once

#include "config/config.h"
#include "hook/vtable_hook.hpp"
#include "scaleform/value.hpp"

#include <cstdint>
#include <memory>
#include <mutex>

namespace sf {

class ScaleformCallbackRouter;
class MovieRootRouteState;

using MovieRootVisitor = bool(*)(
    const MovieRootContext& context,
    void* userData) noexcept;

class MovieRootHookController final {
public:
    explicit MovieRootHookController(
        ScaleformCallbackRouter& callbackRouter,
        VtableHookPlatform platform = SystemVtableHookPlatform()) noexcept;
    ~MovieRootHookController();

    MovieRootHookController(const MovieRootHookController&) = delete;
    MovieRootHookController& operator=(const MovieRootHookController&) = delete;

    [[nodiscard]] VtableHookStatus Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase,
        const config::ScaleformConfig& scaleformConfig = {}) noexcept;
    [[nodiscard]] VtableHookStatus Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept;

private:
    ScaleformCallbackRouter& callbackRouter_;
    VtableHookPlatform platform_;
    VtableHook vtableHook_;
    // Optional: only plugin advance handlers depend on them. The destructor
    // hook ends watches on destroyed movies and is installed first.
    VtableHook destroyHook_;
    VtableHook advanceHook_;
    // Optional: only plugin invoke filters depend on it.
    VtableHook invokeHook_;
    std::shared_ptr<NativeFunctionHandler> functionHandler_;
    bool callbackRouterPublished_{};
    mutable std::mutex lifecycleMutex_;
    std::shared_ptr<MovieRootRouteState> routeState_;
};

void __fastcall HookedMovieRootAdvanceFrame(
    void* movieRoot,
    bool nextFrame) noexcept;
void* __fastcall HookedMovieRootDestroy(
    void* movieRoot,
    unsigned int flags) noexcept;
bool __fastcall HookedMovieRootInvoke(
    void* movieRoot,
    const char* methodPath,
    ScaleformValue* outValue,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept;

[[nodiscard]] bool __fastcall HookedMovieRootGetVariable(
    void* movieRoot,
    ScaleformValue* outValue,
    const char* path,
    unsigned int callerR9Scratch = 0U) noexcept;

// Visits one MovieRoot after checking its vtable. Returns the visitor result,
// or false when no route is active or the movie root is not valid.
[[nodiscard]] bool VisitMovieRoot(
    void* movieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept;

// Visits the calling MovieRoot first, followed by other MovieRoots on which
// xScal has installed its bridge. Returns true when the visitor accepts one.
[[nodiscard]] bool VisitTrackedMovieRoots(
    void* preferredMovieRoot,
    MovieRootVisitor visitor,
    void* userData) noexcept;

}
