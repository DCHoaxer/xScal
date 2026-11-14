#include "scaleform/bridge_attachment.hpp"

#include "api/mod_storage.hpp"
#include "chat/chat_interface.hpp"
#include "config/config.h"
#include "platform/diagnostics.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sf {
namespace {

using CreateFunctionRoutine = void(__fastcall*)(
    void*, ScaleformValue*, void*, void*);

struct DefaultComponentPath final {
    std::string_view path;
    bool probeOnLookup;
};

constexpr std::array<DefaultComponentPath, 4> kDefaultComponentPaths{
    DefaultComponentPath{"root1.BaseInstance", true},
    DefaultComponentPath{"root1.Menu_mc", false},
    DefaultComponentPath{"root1.FilterHolder_mc.Menu_mc", false},
    DefaultComponentPath{"root1.SpecialBuildsMenu_mc", true},
};

[[nodiscard]] constexpr bool ProbeOnLookup(
    std::string_view path) noexcept {
    for (const auto& component : kDefaultComponentPaths) {
        if (component.path == path) {
            return component.probeOnLookup;
        }
    }
    return true;
}

std::atomic<std::uint64_t> nextServiceId{1};

// The last bridge this thread saw ready. Lookups of an __SFCodeObj alias
// repeat the same bridge, and this entry lets them skip mutex_ and the
// attachedCalls_ scan.
struct ReadyBridge final {
    std::uint64_t serviceId{};
    std::uint64_t attachmentChanges{};
    void* movieRoot{};
    NativeCallIdentity bridge{};
    NativeBridgeAttachmentState attachment{};
};

thread_local ReadyBridge lastReadyBridge{};

[[nodiscard]] bool IsCreatedObjectValue(
    const ScaleformValue& value) noexcept {
    const auto info = InspectScaleformValue(value);
    return (info.rawType & layout::kTypeMask) == layout::kObjectType &&
        info.objectInterface != nullptr && info.data != nullptr;
}

[[nodiscard]] bool CreateNativeFunction(
    MovieRootContext& context,
    void* handler,
    ScaleformValue& outFunction) noexcept {
    const auto createFunction = GetMovieRootRoutine<CreateFunctionRoutine>(
        context.movieRoot,
        layout::kCreateFunctionVtableOffset);
    if (createFunction == nullptr || handler == nullptr) {
        return false;
    }
    createFunction(context.movieRoot, &outFunction, handler, nullptr);
    if (IsCreatedObjectValue(outFunction)) {
        return true;
    }
    ReleaseValue(context, outFunction);
    outFunction = {};
    return false;
}

void LogAttachedCall(void* movieRoot, const char* componentPath) noexcept {
    if (componentPath == nullptr) {
        DiagnosticLogFormat(
            "xScal: attached __SFCodeObj call() on MovieRoot %p",
            movieRoot);
        return;
    }
    DiagnosticLogFormat(
        "xScal: attached %s.__SFCodeObj call() on MovieRoot %p",
        componentPath,
        movieRoot);
}

// Returns true when `object` has an object-valued member `name`.
[[nodiscard]] bool HasObjectMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* name) noexcept {
    ScaleformValue member{};
    const bool usable =
        GetScaleformMember(context, object, name, member) &&
        IsObjectLikeScaleformValue(member);
    ReleaseValue(context, member);
    return usable;
}

// Assigns `value` to member `name` of `object`. Returns true when the member
// reads back as an object afterwards.
[[nodiscard]] bool SetObjectMemberAndVerify(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* name,
    ScaleformValue& value) noexcept {
    if (!SetScaleformMember(context, object, name, value)) {
        return false;
    }
    ScaleformValue readback{};
    const bool ready =
        GetScaleformMember(context, object, name, readback) &&
        IsObjectLikeScaleformValue(readback);
    ReleaseValue(context, readback);
    return ready;
}

[[nodiscard]] bool AttachMissingFunction(
    MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    void* handler) noexcept {
    if (HasObjectMember(context, object, memberName)) {
        return true;
    }

    ScaleformValue function{};
    if (!CreateNativeFunction(context, handler, function)) {
        return false;
    }
    ScopedScaleformValue releaseFunction{context, function};
    return SetObjectMemberAndVerify(context, object, memberName, function);
}

[[nodiscard]] bool SetStringMember(
    const MovieRootContext& context,
    ScaleformValue& object,
    const char* memberName,
    std::string_view text) noexcept {
    ScaleformValue value{};
    InitializeBorrowedScaleformString(value, text.data());
    return SetScaleformMember(context, object, memberName, value);
}

// Attaches `attach` to the object held by member `name` of `owner`. A missing
// member is replaced by a new object, which is assigned before the read-back check.
template <typename Attach>
[[nodiscard]] bool AttachObjectMember(
    MovieRootContext& context,
    ScaleformValue& owner,
    const char* name,
    Attach attach) noexcept {
    ScaleformValue existing{};
    if (GetScaleformMember(context, owner, name, existing) &&
        IsObjectLikeScaleformValue(existing)) {
        ScopedScaleformValue releaseExisting{context, existing};
        return attach(existing);
    }
    ReleaseValue(context, existing);

    ScaleformValue created{};
    if (!CreateUnnamedScaleformObject(context, created)) {
        return false;
    }
    ScopedScaleformValue releaseCreated{context, created};
    return attach(created) &&
        SetObjectMemberAndVerify(context, owner, name, created);
}

[[nodiscard]] bool AttachVersion(
    MovieRootContext& context,
    ScaleformValue& bridge,
    TargetKind targetKind) noexcept {
    if (HasObjectMember(context, bridge, "version")) {
        return true;
    }

    ScaleformValue version{};
    if (!CreateUnnamedScaleformObject(context, version)) {
        return false;
    }
    ScopedScaleformValue releaseVersion{context, version};
    std::array<char, 12> versionText{};
    std::snprintf(
        versionText.data(),
        versionText.size(),
        "%u.%u.%u",
        static_cast<unsigned int>(config::kXScalVersion.major),
        static_cast<unsigned int>(config::kXScalVersion.minor),
        static_cast<unsigned int>(config::kXScalVersion.patch));
    const std::string_view platform{TargetKindName(targetKind)};
    if (!SetStringMember(context, version, "runtime", "xScal") ||
        !SetStringMember(context, version, "value", versionText.data()) ||
        !SetStringMember(context, version, "platform", platform)) {
        return false;
    }
    return SetObjectMemberAndVerify(context, bridge, "version", version);
}

[[nodiscard]] bool AttachModStorage(
    MovieRootContext& context,
    ScaleformValue& bridge) noexcept {
    auto& handlers = modstorage::GlobalFunctionHandlers();
    return AttachObjectMember(
        context,
        bridge,
        "modStorage",
        [&context, &handlers](ScaleformValue& storage) {
            return AttachMissingFunction(
                       context,
                       storage,
                       "register",
                       handlers.Register().AsScaleformHandler()) &&
                AttachMissingFunction(
                       context,
                       storage,
                       "save",
                       handlers.Save().AsScaleformHandler()) &&
                AttachMissingFunction(
                       context,
                       storage,
                       "load",
                       handlers.Load().AsScaleformHandler());
        });
}

[[nodiscard]] bool AttachChatInterfaceObject(
    MovieRootContext& context,
    ScaleformValue& codeObject) noexcept {
    if (!chat::IsChatInterfaceAvailable()) {
        return false;
    }
    return AttachObjectMember(
        context,
        codeObject,
        "chatInterface",
        [&context](ScaleformValue& chatInterface) {
            for (const auto& member :
                 chat::GlobalChatInterfaceFunctions().Members()) {
                if (!AttachMissingFunction(
                        context,
                        chatInterface,
                        member.name,
                        member.functionHandler)) {
                    return false;
                }
            }
            return true;
        });
}

[[nodiscard]] BridgeAttachmentResult AttachBridgeMembers(
    MovieRootContext& context,
    ScaleformValue& bridge,
    const NativeBridgeAttachmentState* knownAttachment,
    TargetKind targetKind) noexcept {
    BridgeAttachmentResult result{};
    if (context.movieRoot == nullptr || context.functionHandler == nullptr ||
        context.api.getMember == nullptr || context.api.setMember == nullptr ||
        context.api.releaseValue == nullptr) {
        return result;
    }

    ScaleformValue existingCall{};
    const bool existingReadable =
        GetScaleformMember(context, bridge, "call", existingCall);
    const auto existingInfo = InspectScaleformValue(existingCall);
    const NativeCallIdentity existingIdentity{
        existingInfo.objectInterface,
        existingInfo.data};
    const bool alreadyAuthoritative =
        existingReadable && IsObjectLikeScaleformValue(existingCall) &&
        knownAttachment != nullptr && knownAttachment->call.IsValid() &&
        existingIdentity == knownAttachment->call;
    ReleaseValue(context, existingCall);

    if (alreadyAuthoritative) {
        result.attachment.call = knownAttachment->call;
        result.attachment.chatReady = knownAttachment->chatReady;
        if (knownAttachment->membersReady &&
            (!chat::IsChatInterfaceAvailable() ||
             knownAttachment->chatReady)) {
            result.attachment.membersReady = true;
            result.ready = true;
            return result;
        }
    } else {
        ScaleformValue nativeCall{};
        if (!CreateNativeFunction(
                context,
                context.functionHandler,
                nativeCall)) {
            return result;
        }
        ScopedScaleformValue releaseNativeCall{context, nativeCall};
        if (!SetScaleformMember(context, bridge, "call", nativeCall)) {
            return result;
        }

        ScaleformValue readback{};
        const bool readbackReadable =
            GetScaleformMember(context, bridge, "call", readback);
        const auto readbackInfo = InspectScaleformValue(readback);
        const bool readbackUsable =
            readbackReadable && IsObjectLikeScaleformValue(readback);
        ReleaseValue(context, readback);
        if (!readbackUsable) {
            return result;
        }
        result.attachment.call = {
            readbackInfo.objectInterface,
            readbackInfo.data};
        result.callAttached = true;
    }

    const bool versionReady = AttachVersion(context, bridge, targetKind);
    const bool storageReady = versionReady &&
        AttachModStorage(context, bridge);
    result.attachment.membersReady = versionReady && storageReady;
    if (result.attachment.membersReady &&
        chat::IsChatInterfaceAvailable()) {
        result.attachment.chatReady =
            AttachChatInterfaceObject(context, bridge);
    }
    result.ready = result.attachment.membersReady;
    return result;
}

}

BridgeAttachmentService::BridgeAttachmentService(
    MovieRootGetVariable original,
    ResolvedScaleformApi api,
    NativeFunctionHandler* functionHandler,
    TargetKind targetKind,
    const config::ScaleformConfig& scaleformConfig) noexcept
    : original_{original},
      api_{api},
      functionHandler_{functionHandler},
      targetKind_{targetKind},
      serviceId_{nextServiceId.fetch_add(1, std::memory_order_relaxed)} {
    const auto addPath = [this](
                             std::string_view path,
                             bool probeOnLookup) {
        if (path.empty() ||
            path.size() > config::kMaximumScaleformComponentPathLength ||
            componentCount_ == components_.size()) {
            return;
        }
        auto& component = components_[componentCount_++];
        auto& destination = component.path;
        std::copy(path.begin(), path.end(), destination.begin());
        destination[path.size()] = '\0';
        component.probeOnLookup = probeOnLookup;
    };
    if (scaleformConfig.componentPathCount == 0) {
        for (const auto& component : kDefaultComponentPaths) {
            addPath(component.path, component.probeOnLookup);
        }
    } else {
        for (std::size_t index = 0;
             index < scaleformConfig.componentPathCount;
             ++index) {
            const auto path = scaleformConfig.ComponentPath(index);
            addPath(path, ProbeOnLookup(path));
        }
    }
}

MovieRootContext BridgeAttachmentService::Context(void* movieRoot) noexcept {
    return {movieRoot, original_, api_, functionHandler_};
}

bool BridgeAttachmentService::CopyTrackedMovieRoots(
    void* preferredMovieRoot,
    TrackedMovieRoots& destinations) noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return false;
    }
    if (preferredMovieRoot != nullptr) {
        (void)destinations.Insert(preferredMovieRoot);
    }
    for (void* candidate : roots_) {
        if (candidate != nullptr) {
            (void)destinations.Insert(candidate);
        }
    }
    return true;
}

void BridgeAttachmentService::ForgetProbe(
    ProbeStates& probes,
    std::size_t& probeCount,
    void* movieRoot) noexcept {
    const auto end = probes.begin() + probeCount;
    const auto found = std::find_if(
        probes.begin(),
        end,
        [movieRoot](const ProbeState& probe) {
            return probe.movieRoot == movieRoot;
        });
    if (found == end) {
        return;
    }
    std::move(found + 1, end, found);
    probes[--probeCount] = {};
}

void BridgeAttachmentService::ForgetComponent(
    ComponentAttachments& componentState,
    void* movieRoot) noexcept {
    (void)componentState.roots.Erase(movieRoot);
    ForgetProbe(componentState.probes, componentState.probeCount, movieRoot);
}

bool BridgeAttachmentService::ShouldProbe(
    ProbeStates& probes,
    std::size_t& probeCount,
    void* movieRoot) noexcept {
    constexpr std::uint8_t initialAttempts = 8;
    constexpr std::uint8_t retryInterval = 64;
    const auto end = probes.begin() + probeCount;
    const auto found = std::find_if(
        probes.begin(),
        end,
        [movieRoot](const ProbeState& probe) {
            return probe.movieRoot == movieRoot;
        });
    if (found != end) {
        ++found->attempts;
        if (found->attempts > initialAttempts) {
            // Keep checking delayed shells without probing on every lookup.
            if (found->attempts < initialAttempts + retryInterval) {
                return false;
            }
            found->attempts = initialAttempts;
        }
        return true;
    }
    if (probeCount == kMaximumBridgedMovieRoots) {
        std::rotate(
            probes.begin(),
            probes.begin() + 1,
            probes.end());
        probes.back() = {movieRoot, 1};
    } else {
        probes[probeCount++] = {movieRoot, 1};
    }
    return true;
}

void BridgeAttachmentService::ForgetRootAttachments(void* movieRoot) noexcept {
    attachedCalls_.EraseIf(
        [movieRoot](const AttachedBridgeCall& entry) {
            return entry.movieRoot == movieRoot;
        });
    for (std::size_t index = 0; index < componentCount_; ++index) {
        ForgetComponent(components_[index], movieRoot);
    }
    modstorage::GlobalService().ForgetMovie(movieRoot);
    attachmentChanges_.fetch_add(1, std::memory_order_release);
}

void BridgeAttachmentService::RememberComponentRoot(
    ComponentAttachments& componentState,
    void* movieRoot) noexcept {
    ForgetProbe(componentState.probes, componentState.probeCount, movieRoot);
    void* evicted{};
    if (componentState.roots.Insert(movieRoot, &evicted) &&
        evicted != nullptr) {
        // The evicted MovieRoot is probed again on its next lookup.
        attachmentChanges_.fetch_add(1, std::memory_order_release);
    }
}

std::uint64_t BridgeAttachmentService::AttachmentChangeCount() const noexcept {
    return attachmentChanges_.load(std::memory_order_acquire);
}

void BridgeAttachmentService::RememberRoot(void* movieRoot) noexcept {
    void* evicted{};
    if (!roots_.Insert(movieRoot, &evicted) || evicted == nullptr) {
        return;
    }
    ForgetRootAttachments(evicted);
}

void BridgeAttachmentService::ForgetMovieRoot(void* movieRoot) noexcept {
    // Blocks instead of try_to_lock: a skipped call would leave the dead root
    // in roots_, and nothing else removes it before the address is reused.
    std::scoped_lock lock{mutex_};
    (void)roots_.Erase(movieRoot);
    ForgetRootAttachments(movieRoot);
}

BridgeAttachmentResult BridgeAttachmentService::Attach(
    MovieRootContext& context,
    ScaleformValue& bridge,
    const char* componentPath) noexcept {
    const auto bridgeInfo = InspectScaleformValue(bridge);
    if (bridgeInfo.objectInterface == nullptr || bridgeInfo.data == nullptr) {
        return {};
    }
    const auto known = std::find_if(
        attachedCalls_.begin(),
        attachedCalls_.end(),
        [&context, &bridgeInfo](const AttachedBridgeCall& entry) {
            return entry.movieRoot == context.movieRoot &&
                entry.bridgeObjectInterface == bridgeInfo.objectInterface &&
                entry.bridgeData == bridgeInfo.data;
        });
    const auto* previous = known == attachedCalls_.end()
        ? nullptr
        : &known->attachment;
    const auto result = AttachBridgeMembers(
        context, bridge, previous, targetKind_);
    if (result.callAttached) {
        LogAttachedCall(context.movieRoot, componentPath);
    }
    if (!result.attachment.call.IsValid()) {
        return result;
    }
    // Only real changes are counted: a lookup of an unchanged bridge must not
    // drop other threads' ReadyBridge entries.
    if (known == attachedCalls_.end()) {
        (void)attachedCalls_.Insert({
            context.movieRoot,
            bridgeInfo.objectInterface,
            bridgeInfo.data,
            result.attachment});
        attachmentChanges_.fetch_add(1, std::memory_order_release);
    } else if (known->attachment != result.attachment) {
        known->attachment = result.attachment;
        attachmentChanges_.fetch_add(1, std::memory_order_release);
    }
    return result;
}

bool BridgeAttachmentService::ReadKnownReadyBridge(
    void* movieRoot,
    ScaleformValue& bridge,
    BridgeAttachmentResult& result) noexcept {
    const ReadyBridge known = lastReadyBridge;
    if (movieRoot == nullptr || known.serviceId != serviceId_ ||
        known.movieRoot != movieRoot ||
        known.attachmentChanges !=
            attachmentChanges_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto bridgeInfo = InspectScaleformValue(bridge);
    if (NativeCallIdentity{bridgeInfo.objectInterface, bridgeInfo.data} !=
            known.bridge ||
        (chat::IsChatInterfaceAvailable() && !known.attachment.chatReady)) {
        return false;
    }

    // Same check as AttachBridgeMembers: script code can replace call() or
    // the bridge can be a new object at the old address. Reading the member
    // touches no service state, so mutex_ is not needed for it.
    auto context = Context(movieRoot);
    ScaleformValue call{};
    const bool callReadable =
        GetScaleformMember(context, bridge, "call", call);
    const auto callInfo = InspectScaleformValue(call);
    const bool callAttached = callReadable &&
        IsObjectLikeScaleformValue(call) &&
        NativeCallIdentity{callInfo.objectInterface, callInfo.data} ==
            known.attachment.call;
    ReleaseValue(context, call);
    if (!callAttached) {
        return false;
    }
    result = {};
    result.attachment = known.attachment;
    result.ready = true;
    return true;
}

BridgeAttachmentResult BridgeAttachmentService::AttachResolved(
    void* movieRoot,
    ScaleformValue& bridge) noexcept {
    BridgeAttachmentResult knownResult{};
    if (ReadKnownReadyBridge(movieRoot, bridge, knownResult)) {
        return knownResult;
    }
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        BridgeAttachmentResult contendedResult{};
        contendedResult.contended = true;
        return contendedResult;
    }
    if (movieRoot == nullptr) {
        return {};
    }
    auto context = Context(movieRoot);
    const auto result = Attach(context, bridge, nullptr);
    if (result.ready) {
        const auto bridgeInfo = InspectScaleformValue(bridge);
        lastReadyBridge = {
            serviceId_,
            attachmentChanges_.load(std::memory_order_relaxed),
            movieRoot,
            {bridgeInfo.objectInterface, bridgeInfo.data},
            result.attachment};
    }
    return result;
}

bool BridgeAttachmentService::AttachChatInterface(
    void* movieRoot,
    ScaleformValue& codeObject) noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock() || movieRoot == nullptr) {
        return false;
    }
    auto context = Context(movieRoot);
    return AttachChatInterfaceObject(context, codeObject);
}

bool BridgeAttachmentService::IsAuthoritativeCall(
    void* movieRoot,
    const ScaleformValue& call) noexcept {
    if (!IsObjectLikeScaleformValue(call)) {
        return false;
    }
    const auto info = InspectScaleformValue(call);
    const NativeCallIdentity identity{info.objectInterface, info.data};
    std::unique_lock lock{mutex_, std::try_to_lock};
    return lock.owns_lock() && std::any_of(
        attachedCalls_.begin(),
        attachedCalls_.end(),
        [movieRoot, identity](const AttachedBridgeCall& entry) {
            return entry.movieRoot == movieRoot &&
                entry.attachment.membersReady &&
                entry.attachment.call == identity;
        });
}

RootAttachStatus BridgeAttachmentService::AttachRoot(
    void* movieRoot,
    bool refreshTracked) noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return RootAttachStatus::Contended;
    }
    if (movieRoot == nullptr || original_ == nullptr ||
        api_.getMember == nullptr) {
        return RootAttachStatus::Failed;
    }
    if (roots_.Contains(movieRoot) && !refreshTracked) {
        return RootAttachStatus::Attached;
    }

    auto context = Context(movieRoot);
    ScaleformValue bridge{};
    const bool bridgeReadable = original_(
        movieRoot, &bridge, "root1.__SFCodeObj", 0U);
    if (!IsObjectReadback(bridgeReadable, bridge)) {
        ReleaseValue(context, bridge);
        // Component shells can appear before the canonical shell. Its absence
        // does not end the MovieRoot or invalidate component attachment state.
        (void)roots_.Erase(movieRoot);
        return RootAttachStatus::Failed;
    }

    const auto result = Attach(context, bridge, nullptr);
    ReleaseValue(context, bridge);
    if (!result.ready) {
        return RootAttachStatus::Failed;
    }
    RememberRoot(movieRoot);
    return RootAttachStatus::Attached;
}

bool BridgeAttachmentService::AttachComponent(
    MovieRootContext& context,
    ScaleformValue& component,
    const char* componentPath) noexcept {
    if (context.movieRoot == nullptr || context.api.getMember == nullptr) {
        return false;
    }
    ScaleformValue bridge{};
    const bool readable = GetScaleformMember(
        context, component, "__SFCodeObj", bridge);
    if (!IsObjectReadback(readable, bridge)) {
        ReleaseValue(context, bridge);
        return false;
    }
    const auto result = Attach(context, bridge, componentPath);
    ReleaseValue(context, bridge);
    return result.ready;
}

BridgeAttachmentService::ComponentPathMatch
BridgeAttachmentService::MatchComponentPath(
    std::string_view requestedPath) const noexcept {
    bool descendant{};
    for (std::size_t index = 0; index < componentCount_; ++index) {
        const auto componentPath = components_[index].Path();
        if (requestedPath == componentPath) {
            return ComponentPathMatch::Exact;
        }
        descendant = descendant ||
            (requestedPath.size() > componentPath.size() &&
             requestedPath.starts_with(componentPath) &&
             requestedPath[componentPath.size()] == '.');
    }
    return descendant
        ? ComponentPathMatch::Descendant
        : ComponentPathMatch::None;
}

void BridgeAttachmentService::AttachConfiguredComponent(
    void* movieRoot,
    std::string_view requestedPath,
    ScaleformValue* resolvedComponent) noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }

    ComponentAttachments* descendant{};
    for (std::size_t index = 0; index < componentCount_; ++index) {
        auto& component = components_[index];
        const auto componentPath = component.Path();
        if (requestedPath == componentPath) {
            if (resolvedComponent == nullptr || movieRoot == nullptr) {
                return;
            }
            auto context = Context(movieRoot);
            if (AttachComponent(context, *resolvedComponent, nullptr)) {
                RememberComponentRoot(component, movieRoot);
            }
            return;
        }
        if (requestedPath.size() > componentPath.size() &&
            requestedPath.starts_with(componentPath) &&
            requestedPath[componentPath.size()] == '.' &&
            (descendant == nullptr ||
             componentPath.size() > descendant->Path().size())) {
            descendant = &component;
        }
    }
    if (descendant != nullptr) {
        (void)AttachNamedComponent(
            movieRoot, true, descendant->path.data(), *descendant);
    }
}

bool BridgeAttachmentService::ProbeComponents(void* movieRoot) noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return false;
    }
    bool componentsAttached = true;
    for (std::size_t index = 0; index < componentCount_; ++index) {
        auto& component = components_[index];
        if (!component.probeOnLookup) {
            continue;
        }
        if (!AttachNamedComponent(
                movieRoot, false, component.path.data(), component)) {
            componentsAttached = false;
        }
    }
    return componentsAttached;
}

bool BridgeAttachmentService::AttachNamedComponent(
    void* movieRoot,
    bool forceProbe,
    const char* componentPath,
    ComponentAttachments& componentState) noexcept {
    if (componentState.roots.Contains(movieRoot) && !forceProbe) {
        return true;
    }
    if (!forceProbe && !ShouldProbe(
            componentState.probes, componentState.probeCount, movieRoot)) {
        return false;
    }
    if (movieRoot == nullptr || original_ == nullptr ||
        api_.getMember == nullptr || api_.setMember == nullptr ||
        api_.releaseValue == nullptr || functionHandler_ == nullptr) {
        return false;
    }

    auto context = Context(movieRoot);
    ScaleformValue component{};
    const bool componentReadable = original_(
        movieRoot, &component, componentPath, 0U);
    const bool ready = IsObjectReadback(componentReadable, component) &&
        AttachComponent(context, component, componentPath);
    ReleaseValue(context, component);
    if (ready) {
        RememberComponentRoot(componentState, movieRoot);
    }
    return ready;
}

}
