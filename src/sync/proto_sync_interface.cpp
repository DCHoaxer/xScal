#include "sync/proto_sync_interface.hpp"

#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"
#include "sync/proto_sync.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace sf::sync {
namespace {

constexpr std::size_t kArgumentRecordSize = 0x30;
constexpr char kNativeOwner = 0;

[[nodiscard]] bool ReadCandidateNumber(const ArgumentCandidate& candidate, double& value) noexcept {
    switch (static_cast<std::uint8_t>(candidate.type & layout::kTypeMask)) {
    case layout::kIntType:
        value = static_cast<double>(static_cast<std::int32_t>(candidate.payload));
        return true;
    case layout::kUIntType:
        value = static_cast<double>(static_cast<std::uint32_t>(candidate.payload));
        return true;
    case layout::kNumberType:
        value = std::bit_cast<double>(candidate.payload);
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool ReadRevisionArgument(
    const ScaleformCall& call, std::size_t index, std::uint64_t& revision) noexcept {
    if (index >= call.argumentCount || call.arguments == nullptr) {
        return false;
    }
    std::string_view text;
    if (TryGetScaleformStringArgument(call, index, text)) {
        if (text.empty() || text.size() > 20) {
            return false;
        }
        revision = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') {
                return false;
            }
            revision = revision * 10 + static_cast<std::uint64_t>(character - '0');
        }
        return true;
    }
    const auto* const record =
        static_cast<const std::byte*>(call.arguments) + index * kArgumentRecordSize;
    const ArgumentCandidates candidates = ReadArgumentCandidates(record);
    double value{};
    if (!ReadCandidateNumber(candidates.primary, value) &&
        !ReadCandidateNumber(candidates.fallback, value)) {
        return false;
    }
    if (!std::isfinite(value) || value < 0.0 || value > 9007199254740992.0) {
        return false;
    }
    revision = static_cast<std::uint64_t>(value);
    return true;
}

[[nodiscard]] std::string_view CString(const char* text) noexcept {
    return text != nullptr ? std::string_view{text, ::strnlen(text, 256)} : std::string_view{};
}

std::uint32_t StateValue(ProtoSyncState state) noexcept {
    switch (state) {
    case ProtoSyncState::off:
        return XSCAL_PROTO_SYNC_OFF;
    case ProtoSyncState::unavailable:
        return XSCAL_PROTO_SYNC_UNAVAILABLE;
    case ProtoSyncState::idle:
        return XSCAL_PROTO_SYNC_IDLE;
    case ProtoSyncState::connecting:
        return XSCAL_PROTO_SYNC_CONNECTING;
    case ProtoSyncState::ready:
        return XSCAL_PROTO_SYNC_READY;
    }
    return XSCAL_PROTO_SYNC_UNAVAILABLE;
}

int32_t __cdecl NativeOpen(void*, const char* channel) noexcept {
    return GlobalProtoSync().Open(&kNativeOwner, CString(channel)) ? 1 : 0;
}

void __cdecl NativeClose(void*, const char* channel) noexcept {
    GlobalProtoSync().Close(&kNativeOwner, CString(channel));
}

int32_t __cdecl NativePublish(void*, const char* channel, const char* data, uint32_t size) noexcept {
    if (data == nullptr && size != 0) {
        return 0;
    }
    return GlobalProtoSync().Publish(
               CString(channel), std::string_view{data != nullptr ? data : "", size})
        ? 1
        : 0;
}

uint32_t __cdecl NativeState(void*) noexcept {
    return StateValue(GlobalProtoSync().State());
}

uint64_t __cdecl NativeRevision(void*, const char* channel) noexcept {
    return GlobalProtoSync().Revision(CString(channel));
}

uint32_t __cdecl NativeVisitMembers(
    void*, const char* channel, XScalProtoSyncVisitFn visit, void* userData) noexcept {
    if (visit == nullptr) {
        return 0;
    }
    const std::size_t count = GlobalProtoSync().VisitMembers(
        CString(channel), [&](const ProtoSync::Member& member) noexcept {
            const XScalProtoSyncMember entry{
                member.steamId,
                member.playerId,
                static_cast<std::uint32_t>(member.data.size()),
                member.data.data(),
            };
            visit(userData, &entry);
        });
    return static_cast<std::uint32_t>(count);
}

constexpr XScalProtoSyncV1 kInterface{
    sizeof(XScalProtoSyncV1),
    XSCAL_PROTO_SYNC_API_VERSION,
    nullptr,
    &NativeOpen,
    &NativeClose,
    &NativePublish,
    &NativeState,
    &NativeRevision,
    &NativeVisitMembers,
};

[[nodiscard]] bool CallFunction(
    ProtoSyncFunction function,
    const FunctionParams& params,
    const ScaleformCall& call,
    ScaleformResult& result) noexcept {
    ProtoSync& protoSync = GlobalProtoSync();
    std::string_view channel;
    switch (function) {
    case ProtoSyncFunction::Open:
        result.SetBoolean(params.argumentCount == 1 &&
            TryGetScaleformStringArgument(call, 0, channel) &&
            protoSync.Open(params.movie, channel));
        return true;
    case ProtoSyncFunction::Close:
        if (params.argumentCount == 1 && TryGetScaleformStringArgument(call, 0, channel)) {
            protoSync.Close(params.movie, channel);
        }
        return false;
    case ProtoSyncFunction::Publish: {
        std::string_view data;
        result.SetBoolean(params.argumentCount == 2 &&
            TryGetScaleformStringArgument(call, 0, channel) &&
            TryGetScaleformStringArgument(call, 1, data) && protoSync.Publish(channel, data));
        return true;
    }
    case ProtoSyncFunction::GetState:
        return result.SetString(ProtoSyncStateName(protoSync.State()));
    case ProtoSyncFunction::GetRevision:
        if (params.argumentCount != 1 || !TryGetScaleformStringArgument(call, 0, channel)) {
            return false;
        }
        result.SetNumber(static_cast<double>(protoSync.Revision(channel)));
        return true;
    case ProtoSyncFunction::GetMembers: {
        std::uint64_t since = 0;
        if (params.argumentCount < 1 || params.argumentCount > 2 ||
            !TryGetScaleformStringArgument(call, 0, channel) ||
            (params.argumentCount == 2 && !ReadRevisionArgument(call, 1, since))) {
            return false;
        }
        std::string json;
        return protoSync.MembersJson(channel, since, json) && result.SetString(json);
    }
    }
    return false;
}

}  // namespace

void InvokeProtoSyncFunction(
    ProtoSyncFunction function,
    const FunctionParams* params) noexcept {
    if (params == nullptr || params->result == nullptr) {
        return;
    }
    ScaleformResult result;
    const ScaleformCall call{
        params->arguments,
        params->argumentCount,
        &result,
        params->movie,
        params->thisObject,
        params->argumentFrame,
    };
    if (CallFunction(function, *params, call, result)) {
        (void)CommitScaleformResult(*params, result);
    }
}

std::array<ProtoSyncMember, 6> ProtoSyncFunctions::Members() noexcept {
    return {{
        {"open", open_.AsScaleformHandler()},
        {"close", close_.AsScaleformHandler()},
        {"publish", publish_.AsScaleformHandler()},
        {"getState", getState_.AsScaleformHandler()},
        {"getRevision", getRevision_.AsScaleformHandler()},
        {"getMembers", getMembers_.AsScaleformHandler()},
    }};
}

ProtoSyncFunctions& GlobalProtoSyncFunctions() noexcept {
    static ProtoSyncFunctions functions;
    return functions;
}

const XScalProtoSyncV1* ProtoSyncInterface() noexcept {
    return &kInterface;
}

static_assert(sizeof(ProtoSyncFunctionHandler<ProtoSyncFunction::Open>) == 0x10);

}  // namespace sf::sync
