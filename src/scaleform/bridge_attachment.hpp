#pragma once

#include "common/fixed_unique_set.hpp"
#include "config/config.h"
#include "scaleform/value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace sf {

struct NativeCallIdentity final {
    void* objectInterface{};
    void* data{};

    [[nodiscard]] bool IsValid() const noexcept {
        return objectInterface != nullptr && data != nullptr;
    }

    [[nodiscard]] constexpr bool operator==(
        const NativeCallIdentity&) const noexcept = default;
};

struct NativeBridgeAttachmentState final {
    NativeCallIdentity call{};
    bool membersReady{};
    bool chatReady{};
};

struct BridgeAttachmentResult final {
    NativeBridgeAttachmentState attachment{};
    bool ready{};
    bool callAttached{};
    // Set when the attachment lock was held by another thread and nothing was attached.
    bool contended{};
};

enum class RootAttachStatus {
    Attached,
    Failed,
    Contended,
};

class BridgeAttachmentService final {
public:
    static constexpr std::size_t kMaximumBridgedMovieRoots = 64;
    using TrackedMovieRoots = FixedUniqueSet<
        void*, kMaximumBridgedMovieRoots + 1>;

    enum class ComponentPathMatch {
        None,
        Exact,
        Descendant,
    };

    BridgeAttachmentService(
        MovieRootGetVariable original,
        ResolvedScaleformApi api,
        NativeFunctionHandler* functionHandler,
        TargetKind targetKind,
        const config::ScaleformConfig& scaleformConfig = {}) noexcept;

    BridgeAttachmentService(const BridgeAttachmentService&) = delete;
    BridgeAttachmentService& operator=(
        const BridgeAttachmentService&) = delete;

    [[nodiscard]] RootAttachStatus AttachRoot(
        void* movieRoot,
        bool refreshTracked) noexcept;
    [[nodiscard]] ComponentPathMatch MatchComponentPath(
        std::string_view requestedPath) const noexcept;
    void AttachConfiguredComponent(
        void* movieRoot,
        std::string_view requestedPath,
        ScaleformValue* resolvedComponent) noexcept;
    void ProbeComponents(void* movieRoot) noexcept;
    [[nodiscard]] BridgeAttachmentResult AttachResolved(
        void* movieRoot,
        ScaleformValue& bridge) noexcept;
    [[nodiscard]] bool AttachChatInterface(
        void* movieRoot,
        ScaleformValue& codeObject) noexcept;
    [[nodiscard]] bool IsAuthoritativeCall(
        void* movieRoot,
        const ScaleformValue& call) noexcept;
    [[nodiscard]] bool CopyTrackedMovieRoots(
        void* preferredMovieRoot,
        TrackedMovieRoots& destinations) noexcept;

private:
    struct ProbeState final {
        void* movieRoot{};
        std::uint8_t attempts{};
    };

    using ProbeStates =
        std::array<ProbeState, kMaximumBridgedMovieRoots>;

    struct ComponentAttachments final {
        std::array<char, config::kMaximumScaleformComponentPathLength + 1>
            path{};
        bool probeOnLookup{};
        FixedUniqueSet<void*, kMaximumBridgedMovieRoots> roots;
        ProbeStates probes{};
        std::size_t probeCount{};

        [[nodiscard]] std::string_view Path() const noexcept {
            return path.data();
        }
    };

    struct AttachedBridgeCall final {
        void* movieRoot{};
        void* bridgeObjectInterface{};
        void* bridgeData{};
        NativeBridgeAttachmentState attachment{};

        [[nodiscard]] bool operator==(
            const AttachedBridgeCall& other) const noexcept {
            return movieRoot == other.movieRoot &&
                bridgeObjectInterface == other.bridgeObjectInterface &&
                bridgeData == other.bridgeData;
        }
    };

    [[nodiscard]] MovieRootContext Context(void* movieRoot) noexcept;
    // componentPath names the component in the log line; nullptr logs the canonical shell.
    [[nodiscard]] BridgeAttachmentResult Attach(
        MovieRootContext& context,
        ScaleformValue& bridge,
        const char* componentPath) noexcept;
    [[nodiscard]] bool AttachComponent(
        MovieRootContext& context,
        ScaleformValue& component,
        const char* componentPath) noexcept;
    [[nodiscard]] bool AttachNamedComponent(
        void* movieRoot,
        bool forceProbe,
        const char* componentPath,
        ComponentAttachments& componentState) noexcept;
    [[nodiscard]] static bool ShouldProbe(
        ProbeStates& probes,
        std::size_t& probeCount,
        void* movieRoot) noexcept;
    static void ForgetProbe(
        ProbeStates& probes,
        std::size_t& probeCount,
        void* movieRoot) noexcept;
    void RememberRoot(void* movieRoot) noexcept;
    static void ForgetComponent(
        ComponentAttachments& componentState,
        void* movieRoot) noexcept;

    MovieRootGetVariable original_{};
    ResolvedScaleformApi api_{};
    NativeFunctionHandler* functionHandler_{};
    TargetKind targetKind_{TargetKind::Steam};
    std::mutex mutex_;
    FixedUniqueSet<void*, kMaximumBridgedMovieRoots> roots_{};
    std::array<
        ComponentAttachments,
        config::kMaximumScaleformComponentPaths> components_{};
    std::size_t componentCount_{};
    FixedUniqueSet<
        AttachedBridgeCall,
        kMaximumBridgedMovieRoots *
            (config::kMaximumScaleformComponentPaths + 1)> attachedCalls_{};
};

}
