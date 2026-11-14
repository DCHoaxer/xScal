#pragma once

#include "xscal/plugin_api.h"

#include <array>
#include <cstdint>

namespace sf {

struct FunctionParams;

namespace sync {

enum class ProtoSyncFunction {
    Open,
    Close,
    Publish,
    GetState,
    GetRevision,
    GetMembers,
};

struct ProtoSyncMember final {
    const char* name{};
    void* functionHandler{};
};

void InvokeProtoSyncFunction(
    ProtoSyncFunction function,
    const FunctionParams* params) noexcept;

// GFx function handlers are 0x10 bytes, so each function needs its own vtable.
template <ProtoSyncFunction Function>
class ProtoSyncFunctionHandler final {
public:
    [[nodiscard]] void* AsScaleformHandler() noexcept { return this; }

private:
    using HandlerVtableEntry = void(__fastcall*)();

    [[nodiscard]] static void* __fastcall Identity(void* handler) noexcept {
        return handler;
    }

    static void __fastcall Invoke(
        void*,
        const FunctionParams* params) noexcept {
        InvokeProtoSyncFunction(Function, params);
    }

    [[nodiscard]] static const HandlerVtableEntry* Vtable() noexcept {
        static const HandlerVtableEntry entries[]{
            reinterpret_cast<HandlerVtableEntry>(&Identity),
            reinterpret_cast<HandlerVtableEntry>(&Invoke),
        };
        return entries;
    }

    const HandlerVtableEntry* vtable_{Vtable()};
    volatile std::int32_t referenceCount_{1};
    std::uint32_t referenceCountPadding_{};
};

class ProtoSyncFunctions final {
public:
    [[nodiscard]] std::array<ProtoSyncMember, 6> Members() noexcept;

private:
    ProtoSyncFunctionHandler<ProtoSyncFunction::Open> open_;
    ProtoSyncFunctionHandler<ProtoSyncFunction::Close> close_;
    ProtoSyncFunctionHandler<ProtoSyncFunction::Publish> publish_;
    ProtoSyncFunctionHandler<ProtoSyncFunction::GetState> getState_;
    ProtoSyncFunctionHandler<ProtoSyncFunction::GetRevision> getRevision_;
    ProtoSyncFunctionHandler<ProtoSyncFunction::GetMembers> getMembers_;
};

[[nodiscard]] ProtoSyncFunctions& GlobalProtoSyncFunctions() noexcept;

[[nodiscard]] const XScalProtoSyncV1* ProtoSyncInterface() noexcept;

}  // namespace sync
}  // namespace sf
