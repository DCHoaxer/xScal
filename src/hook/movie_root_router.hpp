#pragma once

#include "hook/movie_root_hook.hpp"
#include "scaleform/bridge_attachment.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace sf {

class MovieRootRouteState final {
public:
    MovieRootRouteState(
        std::shared_ptr<NativeFunctionHandler> functionHandler,
        VtableHookPlatform platform,
        MovieRootGetVariable original,
        void* expectedVtable,
        ResolvedScaleformApi api,
        TargetKind targetKind,
        const config::ScaleformConfig& scaleformConfig) noexcept;

    [[nodiscard]] bool Route(
        void* movieRoot,
        ScaleformValue* outValue,
        const char* path,
        unsigned int callerR9Scratch) noexcept;

    [[nodiscard]] bool VisitMovieRoot(
        void* movieRoot,
        MovieRootVisitor visitor,
        void* userData) noexcept;

    [[nodiscard]] bool VisitTrackedMovieRoots(
        void* preferredMovieRoot,
        MovieRootVisitor visitor,
        void* userData) noexcept;

private:
    VtableHookPlatform platform_;
    std::shared_ptr<NativeFunctionHandler> functionHandler_;
    MovieRootGetVariable original_{};
    void* expectedVtable_{};
    ResolvedScaleformApi api_{};
    std::uint64_t validationGeneration_{};
    BridgeAttachmentService attachments_;
};

// Path classification for the aliases handled by MovieRootRouteState::Route.
namespace routing {

[[nodiscard]] bool IsObjectAlias(std::string_view path) noexcept;
[[nodiscard]] bool IsDirectCallAlias(std::string_view path) noexcept;
[[nodiscard]] bool IsModStorageAlias(std::string_view path) noexcept;
[[nodiscard]] bool IsChatInterfaceAlias(std::string_view path) noexcept;
[[nodiscard]] bool ShouldAttachRootBridge(std::string_view path) noexcept;

}

}
