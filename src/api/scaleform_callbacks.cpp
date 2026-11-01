#include "api/scaleform_callbacks.hpp"

#include "api/file_command_service.hpp"
#include "api/game_objects.hpp"
#include "api/internal/enemy_meter_data.hpp"
#include "api/internal/stealth_meter_data.hpp"
#include "chat/fcm_log.hpp"
#include "chat/scaleform_payload.hpp"
#include "common/json.hpp"
#include "config/config.h"
#include "datamanager/pip_boy_providers.hpp"
#include "datamanager/pip_boy_stats_effects_provider.hpp"
#include "datamanager/player_map_position_provider.hpp"
#include "hook/movie_root_hook.hpp"
#include "input/input_state.hpp"
#include "platform/diagnostics.hpp"
#include "xscal/plugin_api.h"
#include "plugins/plugins.hpp"
#include "scaleform/native_handler.hpp"
#include "scaleform/scaleform_layout.hpp"

#include <Xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace sf {
namespace {

constexpr std::string_view kNoEnemyTargetPayload =
    R"json({"data": "notarget"})json";
constexpr std::string_view kUnavailableStealthMeterPayload =
    R"json({"data": "unavailable"})json";
constexpr std::string_view kInactiveProviderPayload =
    R"json({"status":"inactive"})json";
constexpr std::string_view kPendingEffectsPayload =
    R"json({"status":"pending","reason":"snapshot"})json";
constexpr std::string_view kPendingTimeDataPayload =
    R"json({"status":"pending","reason":"time_data"})json";
constexpr std::string_view kPendingPlayerMapPositionPayload =
    R"json({"status":"pending","reason":"player_map_position"})json";
constexpr std::string_view kPendingPipBoySpecialPayload =
    R"json({"status":"pending","reason":"special"})json";
constexpr std::string_view kPendingPipBoyStatusPayload =
    R"json({"status":"pending","reason":"status"})json";
constexpr std::string_view kPendingPipBoyPlayerInfoPayload =
    R"json({"status":"pending","reason":"player_info"})json";
constexpr std::string_view kPendingPipBoyPerksPayload =
    R"json({"status":"pending","reason":"perks"})json";
constexpr std::string_view kPendingPipBoyQuestsPayload =
    R"json({"status":"pending","reason":"quests"})json";
constexpr std::string_view kPendingPipBoyInventoryPayload =
    R"json({"status":"pending","reason":"inventory"})json";
constexpr std::string_view kPendingPipBoyCollectionsPayload =
    R"json({"status":"pending","reason":"collections"})json";
constexpr std::string_view kInvalidRequestPayload =
    R"json({"status":"error","reason":"invalid_request"})json";

// Non-owning view of the router's dependencies. The router builds one per
// Dispatch call.
struct ScaleformCallbackTargets final {
    FileCommandService& fileCommands;
    InputState& input;
    datamanager::PipBoyStatsEffectsProvider& statsEffects;
    datamanager::PlayerMapPositionProvider& playerMapPosition;
    datamanager::PipBoyProviders& pipBoy;
    const std::atomic<TargetKind>& targetKind;
};

using ScaleformCallbackHandler = bool(*)(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept;

struct ScaleformCallbackDefinition final {
    std::string_view name;
    ScaleformCallbackHandler callback;
};

[[nodiscard]] bool DecodeUnsignedCandidate(
    std::uint64_t encodedType,
    std::uint64_t encodedValue,
    std::uint32_t maximum,
    bool allowZero,
    std::uint32_t& decoded) noexcept {
    decoded = 0;
    switch (static_cast<std::uint8_t>(encodedType) & layout::kTypeMask) {
        case layout::kIntType: {
            const auto value = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(encodedValue));
            if (value < 0 || (!allowZero && value == 0)) {
                return false;
            }
            decoded = static_cast<std::uint32_t>(value);
            break;
        }
        case layout::kUIntType:
            decoded = static_cast<std::uint32_t>(encodedValue);
            break;
        case layout::kNumberType: {
            double value{};
            std::memcpy(&value, &encodedValue, sizeof(value));
            if (!std::isfinite(value) || value < 0.0 ||
                (!allowZero && value == 0.0) ||
                std::trunc(value) != value ||
                value > static_cast<double>(maximum)) {
                return false;
            }
            decoded = static_cast<std::uint32_t>(value);
            break;
        }
        default:
            return false;
    }

    return (allowZero || decoded != 0) && decoded <= maximum;
}

[[nodiscard]] bool TryGetUnsignedArgument(
    const ScaleformCall& call,
    std::size_t argumentIndex,
    std::uint32_t maximum,
    bool allowZero,
    std::uint32_t& decoded) noexcept {
    decoded = 0;
    if (call.arguments == nullptr || call.argumentCount <= argumentIndex) {
        return false;
    }

    const auto* arguments = static_cast<const std::byte*>(call.arguments);
    const auto* argument =
        arguments + argumentIndex * layout::kFunctionArgumentSize;
    const auto candidates = ReadArgumentCandidates(argument);
    if (DecodeUnsignedCandidate(
            candidates.primary.type,
            candidates.primary.payload,
            maximum,
            allowZero,
            decoded)) {
        return true;
    }

    return DecodeUnsignedCandidate(
        candidates.fallback.type,
        candidates.fallback.payload,
        maximum,
        allowZero,
        decoded);
}

[[nodiscard]] bool TryGetVirtualKeyArgument(
    const ScaleformCall& call,
    std::uint8_t& key) noexcept {
    std::uint32_t decoded{};
    if (!TryGetUnsignedArgument(
            call,
            1,
            (std::numeric_limits<std::uint8_t>::max)(),
            false,
            decoded)) {
        return false;
    }
    key = static_cast<std::uint8_t>(decoded);
    return true;
}

[[nodiscard]] bool TryGetGamepadArguments(
    const ScaleformCall& call,
    std::uint16_t& buttons,
    std::uint32_t& userIndex) noexcept {
    std::uint32_t decodedButtons{};
    if (!TryGetUnsignedArgument(
            call,
            1,
            (std::numeric_limits<std::uint16_t>::max)(),
            false,
            decodedButtons)) {
        return false;
    }

    userIndex = 0;
    if (call.argumentCount > 2 &&
        !TryGetUnsignedArgument(
            call,
            2,
            XUSER_MAX_COUNT - 1,
            true,
            userIndex)) {
        return false;
    }

    buttons = static_cast<std::uint16_t>(decodedButtons);
    return true;
}

[[nodiscard]] bool LogFcmMessage(
    const ScaleformCall& call,
    const ScaleformCallbackTargets&) noexcept {
    call.result->SetBoolean(false);

    chat::ScaleformPayload payload{call};
    std::string vendor;
    std::string message;
    if (!payload.IsObject() ||
        payload.ReadString("vendor", vendor) !=
            chat::PayloadMemberStatus::Found ||
        vendor != "FCMChatWidget" ||
        payload.ReadString("message", message) !=
            chat::PayloadMemberStatus::Found) {
        return true;
    }

    std::string line;
    if (!chat::BuildFcmLogLine(message, line)) {
        return true;
    }
    DiagnosticLog(line.c_str());
    call.result->SetBoolean(true);
    return true;
}

template <FileCommand Command>
[[nodiscard]] bool HandleQueuedFileWrite(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    call.result->SetBoolean(false);
    if (call.arguments == nullptr || call.argumentCount < 2) {
        return true;
    }

    std::string_view data;
    if (!TryGetScaleformStringArgument(
            call,
            1,
            data,
            FileCommandService::kMaximumPayloadBytes + 1)) {
        return true;
    }

    const auto status = targets.fileCommands.Submit(Command, data);
    call.result->SetBoolean(status == FileCommandSubmitStatus::Accepted);
    return true;
}

[[nodiscard]] bool HandleRuntimeInfo(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    try {
        std::string runtimeInfo{R"json({"runtime":"xScal","version":")json"};
        runtimeInfo.append(std::to_string(config::kXScalVersion.major));
        runtimeInfo.push_back('.');
        runtimeInfo.append(std::to_string(config::kXScalVersion.minor));
        runtimeInfo.push_back('.');
        runtimeInfo.append(std::to_string(config::kXScalVersion.patch));
        runtimeInfo.append(R"json(","platform":")json");
        runtimeInfo.append(TargetKindName(
            targets.targetKind.load(std::memory_order_acquire)));
        runtimeInfo.append(R"json("})json");
        return call.result->SetString(runtimeInfo);
    } catch (...) {
        return false;
    }
}

// Result of one walk over the tracked movie roots. The walk stops at the
// first root whose status is not MeterUnavailable.
template <typename Status, typename Snapshot>
struct MeterRequest final {
    Status status{};
    Snapshot snapshot;
};

template <
    typename Status,
    typename Snapshot,
    Status (*kReadSnapshot)(const MovieRootContext&, Snapshot&) noexcept>
[[nodiscard]] bool ReadMeter(
    const MovieRootContext& context,
    void* userData) noexcept {
    auto& request = *static_cast<MeterRequest<Status, Snapshot>*>(userData);
    request.status = kReadSnapshot(context, request.snapshot);
    return request.status != Status::MeterUnavailable;
}

using EnemyMeterRequest =
    MeterRequest<enemy_meter::ReadStatus, enemy_meter::Snapshot>;
using StealthMeterRequest =
    MeterRequest<stealth_meter::ReadStatus, stealth_meter::Snapshot>;

[[nodiscard]] bool HandleEnemyMeter(
    const ScaleformCall& call,
    const ScaleformCallbackTargets&) noexcept {
    EnemyMeterRequest request;
    (void)VisitTrackedMovieRoots(
        call.movie,
        &ReadMeter<
            enemy_meter::ReadStatus,
            enemy_meter::Snapshot,
            &enemy_meter::ReadSnapshot>,
        &request);
    if (request.status != enemy_meter::ReadStatus::Target) {
        return call.result->SetString(kNoEnemyTargetPayload);
    }

    (void)enemy_meter::ReadNativeResistances(
        request.snapshot.resistances);
    std::string payload;
    if (!enemy_meter::BuildPayload(request.snapshot, payload)) {
        return call.result->SetString(kNoEnemyTargetPayload);
    }
    return call.result->SetString(payload);
}

[[nodiscard]] bool HandleStealthMeter(
    const ScaleformCall& call,
    const ScaleformCallbackTargets&) noexcept {
    StealthMeterRequest request;
    (void)VisitTrackedMovieRoots(
        call.movie,
        &ReadMeter<
            stealth_meter::ReadStatus,
            stealth_meter::Snapshot,
            &stealth_meter::ReadSnapshot>,
        &request);
    if (request.status != stealth_meter::ReadStatus::Meter) {
        return call.result->SetString(kUnavailableStealthMeterPayload);
    }

    std::string payload;
    if (!stealth_meter::BuildPayload(request.snapshot, payload)) {
        return call.result->SetString(kUnavailableStealthMeterPayload);
    }
    return call.result->SetString(payload);
}

[[nodiscard]] bool HandleStatsEffects(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::shared_ptr<const std::string> payload;
    const auto status = targets.statsEffects.Request(payload);
    switch (status) {
        case datamanager::StatsEffectsRequestStatus::Delivered:
            if (payload != nullptr && call.result->SetString(*payload)) {
                return true;
            }
            return call.result->SetString(kInvalidRequestPayload);
        case datamanager::StatsEffectsRequestStatus::Inactive:
            return call.result->SetString(kInactiveProviderPayload);
        case datamanager::StatsEffectsRequestStatus::SnapshotUnavailable:
            return call.result->SetString(kPendingEffectsPayload);
        default:
            return call.result->SetString(kInvalidRequestPayload);
    }
}

[[nodiscard]] [[nodiscard]] [[nodiscard]] bool HandlePlayerMapPosition(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::string payload;
    const auto status = targets.playerMapPosition.Request(payload);
    switch (status) {
        case datamanager::PlayerMapPositionRequestStatus::Delivered:
            if (call.result->SetString(payload)) {
                return true;
            }
            return call.result->SetString(kInvalidRequestPayload);
        case datamanager::PlayerMapPositionRequestStatus::Inactive:
            return call.result->SetString(kInactiveProviderPayload);
        case datamanager::PlayerMapPositionRequestStatus::SnapshotUnavailable:
            return call.result->SetString(kPendingPlayerMapPositionPayload);
        default:
            return call.result->SetString(kInvalidRequestPayload);
    }
}

// Writes a Game.* provider result: the JSON, or the status payload the
// request returned.
// The JSON a Game.* call returns for status: the provider's payload when it
// was delivered, otherwise the matching status payload.
[[nodiscard]] std::string_view SelectGameProviderPayload(
    datamanager::GameProviderRequestStatus status,
    std::string_view pendingPayload,
    std::string_view payload) noexcept {
    switch (status) {
        case datamanager::GameProviderRequestStatus::Delivered:
            return payload;
        case datamanager::GameProviderRequestStatus::Inactive:
            return kInactiveProviderPayload;
        case datamanager::GameProviderRequestStatus::SnapshotUnavailable:
            return pendingPayload;
        default:
            return kInvalidRequestPayload;
    }
}

[[nodiscard]] bool SetGameProviderResult(
    const ScaleformCall& call,
    std::string_view pendingPayload,
    datamanager::GameProviderRequestStatus status,
    std::string_view payload) noexcept {
    if (call.result->SetString(
            SelectGameProviderPayload(status, pendingPayload, payload))) {
        return true;
    }
    return call.result->SetString(kInvalidRequestPayload);
}

// Writes the result of the Game.* provider kProvider points to.
template <auto kProvider, const std::string_view& kPendingPayload>
[[nodiscard]] bool HandlePipBoyProvider(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::string payload;
    const auto status = (targets.pipBoy.*kProvider).Request(payload);
    return SetGameProviderResult(call, kPendingPayload, status, payload);
}

// Requests the provider kProvider points to and returns the JSON its Game.*
// call would return. The text is payload or a status constant.
template <auto kProvider, const std::string_view& kPendingPayload>
[[nodiscard]] std::string_view ReadPipBoyProviderJson(
    datamanager::PipBoyProviders& providers,
    std::string& payload) noexcept {
    const auto status = (providers.*kProvider).Request(payload);
    return SelectGameProviderPayload(status, kPendingPayload, payload);
}

struct GameObjectSource final {
    std::string_view name;
    std::string_view (*readJson)(
        datamanager::PipBoyProviders& providers,
        std::string& payload) noexcept;
};

// The Game.* calls Game.GetAS3Object accepts. The index is the cache slot.
constexpr std::array kGameObjectSources{
    GameObjectSource{
        "Game.Clock",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::time,
            kPendingTimeDataPayload>},
    GameObjectSource{
        "Game.GetCollections",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::collections,
            kPendingPipBoyCollectionsPayload>},
    GameObjectSource{
        "Game.GetInventory",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::inventory,
            kPendingPipBoyInventoryPayload>},
    GameObjectSource{
        "Game.GetPerks",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::perks,
            kPendingPipBoyPerksPayload>},
    GameObjectSource{
        "Game.GetPlayerInfo",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::playerInfo,
            kPendingPipBoyPlayerInfoPayload>},
    GameObjectSource{
        "Game.GetQuests",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::quests,
            kPendingPipBoyQuestsPayload>},
    GameObjectSource{
        "Game.GetSpecial",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::special,
            kPendingPipBoySpecialPayload>},
    GameObjectSource{
        "Game.GetStatus",
        &ReadPipBoyProviderJson<
            &datamanager::PipBoyProviders::status,
            kPendingPipBoyStatusPayload>},
};

static_assert(kGameObjectSources.size() == kGameObjectSlotCount);

struct GameObjectRequest final {
    std::size_t slot{};
    std::string_view json;
    ScaleformResult* result{};
};

// Builds the object in the calling movie, which owns the returned value.
[[nodiscard]] bool WriteGameObjectResult(
    const MovieRootContext& context,
    void* userData) noexcept {
    auto& request = *static_cast<GameObjectRequest*>(userData);
    ScaleformValue object{};
    if (!ReadGameObject(context, request.slot, request.json, object) &&
        !ReadGameObject(context, request.slot, kInvalidRequestPayload, object)) {
        return false;
    }
    request.result->SetOwnedValue(object, context.api.releaseValue);
    return true;
}

// Game.GetAS3Object(name): the result of the Game.* call name as an
// ActionScript object instead of a JSON string.
[[nodiscard]] bool HandleGameObject(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    if (call.arguments == nullptr || call.argumentCount < 2) {
        return false;
    }
    std::string_view name;
    if (!TryDecodeScaleformString(
            static_cast<const std::byte*>(call.arguments) +
                layout::kFunctionArgumentSize,
            name)) {
        return false;
    }
    const auto source = std::find_if(
        kGameObjectSources.begin(),
        kGameObjectSources.end(),
        [name](const GameObjectSource& entry) { return entry.name == name; });
    if (source == kGameObjectSources.end()) {
        return false;
    }
    std::string payload;
    GameObjectRequest request{
        static_cast<std::size_t>(source - kGameObjectSources.begin()),
        source->readJson(targets.pipBoy, payload),
        call.result,
    };
    return VisitMovieRoot(call.movie, &WriteGameObjectResult, &request);
}

using KeyOperation = bool (InputState::*)(std::uint8_t) noexcept;

template <KeyOperation kOperation>
[[nodiscard]] bool HandleKeyOperation(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint8_t key{};
    const bool succeeded = TryGetVirtualKeyArgument(call, key) &&
        (targets.input.*kOperation)(key);
    call.result->SetBoolean(succeeded);
    return true;
}

[[nodiscard]] bool HandleClearKeys(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    targets.input.ClearKeys();
    call.result->SetBoolean(true);
    return true;
}

[[nodiscard]] bool HandleClearKeySuppressions(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    call.result->SetBoolean(targets.input.ClearKeySuppressions());
    return true;
}

using GamepadAnalogOperation = bool (InputState::*)(std::uint32_t) noexcept;

template <GamepadAnalogOperation kOperation>
[[nodiscard]] bool HandleGamepadAnalogOperation(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint32_t userIndex{};
    call.result->SetBoolean(
        TryGetUnsignedArgument(
            call, 1, XUSER_MAX_COUNT - 1, true, userIndex) &&
        (targets.input.*kOperation)(userIndex));
    return true;
}

[[nodiscard]] bool HandlePollGamepad(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint32_t userIndex{};
    const InputState::GamepadSnapshot pad =
        TryGetUnsignedArgument(
            call, 1, XUSER_MAX_COUNT - 1, true, userIndex)
        ? targets.input.PollGamepad(userIndex)
        : InputState::GamepadSnapshot{};
    try {
        std::string payload{"{\"connected\":"};
        payload.append(pad.connected ? "true" : "false");
        payload.append(",\"lt\":");
        payload.append(std::to_string(pad.leftTrigger));
        payload.append(",\"rt\":");
        payload.append(std::to_string(pad.rightTrigger));
        payload.append(",\"lx\":");
        payload.append(std::to_string(pad.leftX));
        payload.append(",\"ly\":");
        payload.append(std::to_string(pad.leftY));
        payload.append(",\"rx\":");
        payload.append(std::to_string(pad.rightX));
        payload.append(",\"ry\":");
        payload.append(std::to_string(pad.rightY));
        payload.append(",\"buttons\":");
        payload.append(std::to_string(pad.buttons));
        payload.push_back('}');
        return call.result->SetString(payload);
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool HandleBeginMouseCapture(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    call.result->SetBoolean(targets.input.BeginMouseCapture());
    return true;
}

[[nodiscard]] bool HandleEndMouseCapture(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    call.result->SetBoolean(targets.input.EndMouseCapture());
    return true;
}

// Returned as a JSON string like the other object-shaped results.
[[nodiscard]] bool HandlePollMouse(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    const InputState::MouseSnapshot mouse = targets.input.PollMouse();
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    try {
        std::string payload{"{\"dx\":"};
        payload.append(std::to_string(mouse.dx));
        payload.append(",\"dy\":");
        payload.append(std::to_string(mouse.dy));
        payload.append(",\"wheel\":");
        payload.append(std::to_string(mouse.wheel));
        payload.append(",\"left\":");
        payload.append(flag(mouse.left));
        payload.append(",\"right\":");
        payload.append(flag(mouse.right));
        payload.append(",\"leftPressed\":");
        payload.append(flag(mouse.leftPressed));
        payload.append(",\"rightPressed\":");
        payload.append(flag(mouse.rightPressed));
        payload.append(",\"capturing\":");
        payload.append(flag(mouse.capturing));
        payload.push_back('}');
        return call.result->SetString(payload);
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool HandleBeginInput(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    const std::uint32_t sessionId = targets.input.BeginTextInput();
    if (sessionId == 0) {
        return call.result->SetString(
            R"({"success":false,"error":"input_unavailable"})");
    }

    InputState::TextInputSnapshot snapshot;
    if (!targets.input.PollTextInput(sessionId, snapshot)) {
        (void)targets.input.EndTextInput(sessionId);
        return call.result->SetString(
            R"({"success":false,"error":"input_unavailable"})");
    }

    try {
        std::string payload{"{\"success\":true,\"sessionId\":"};
        payload.append(std::to_string(snapshot.sessionId));
        payload.append(",\"revision\":0,\"text\":\"\",\"caretIndex\":0,\"state\":\"active\"}");
        return call.result->SetString(payload);
    } catch (...) {
        (void)targets.input.EndTextInput(sessionId);
        return false;
    }
}

[[nodiscard]] bool HandlePollInput(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint32_t sessionId{};
    InputState::TextInputSnapshot snapshot;
    if (!TryGetUnsignedArgument(
            call,
            1,
            (std::numeric_limits<std::uint32_t>::max)(),
            false,
            sessionId) ||
        !targets.input.PollTextInput(sessionId, snapshot)) {
        return call.result->SetString(
            R"({"success":false,"error":"invalid_session"})");
    }
    const char* state = snapshot.status == InputState::TextInputStatus::Active
        ? "active"
        : snapshot.status == InputState::TextInputStatus::Submitted
            ? "submitted" : "cancelled";
    try {
        std::string payload{"{\"success\":true,\"sessionId\":"};
        payload.append(std::to_string(sessionId));
        payload.append(",\"revision\":");
        payload.append(std::to_string(snapshot.revision));
        payload.append(",\"text\":");
        json::AppendString(snapshot.text, payload);
        payload.append(",\"caretIndex\":");
        payload.append(std::to_string(snapshot.caretIndex));
        payload.append(",\"state\":\"");
        payload.append(state);
        payload.append("\"}");
        return call.result->SetString(payload);
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool HandleEndInput(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint32_t sessionId{};
    const bool success = TryGetUnsignedArgument(
            call,
            1,
            (std::numeric_limits<std::uint32_t>::max)(),
            false,
            sessionId) &&
        targets.input.EndTextInput(sessionId);
    if (!success) {
        return call.result->SetString(
            R"({"success":false,"error":"invalid_session"})");
    }
    try {
        return call.result->SetString(
            std::string{"{\"success\":true,\"sessionId\":"} +
            std::to_string(sessionId) + "}");
    } catch (...) {
        return false;
    }
}

using GamepadOperation = bool (InputState::*)(
    std::uint16_t,
    std::uint32_t) noexcept;

template <GamepadOperation kOperation>
[[nodiscard]] bool HandleGamepadOperation(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    std::uint16_t buttons{};
    std::uint32_t userIndex{};
    const bool succeeded =
        TryGetGamepadArguments(call, buttons, userIndex) &&
        (targets.input.*kOperation)(buttons, userIndex);
    call.result->SetBoolean(succeeded);
    return true;
}

[[nodiscard]] bool HandleClearGamepadButtons(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    targets.input.ClearGamepadButtons();
    call.result->SetBoolean(true);
    return true;
}

[[nodiscard]] bool HandleClearGamepadSuppressions(
    const ScaleformCall& call,
    const ScaleformCallbackTargets& targets) noexcept {
    call.result->SetBoolean(targets.input.ClearGamepadSuppressions());
    return true;
}

[[nodiscard]] bool DecodePluginScalar(
    std::uint64_t encodedType,
    std::uint64_t encodedValue,
    XScalScaleformValue& decoded) noexcept {
    decoded = {};
    switch (static_cast<std::uint8_t>(encodedType) & layout::kTypeMask) {
        case layout::kBooleanType:
            decoded.type = XSCAL_SCALEFORM_VALUE_BOOLEAN;
            // The boolean is one byte; the bytes above it are not cleared,
            // so false can arrive as a nonzero 64-bit value.
            decoded.value.booleanValue = (encodedValue & 0xFF) != 0;
            return true;
        case layout::kIntType:
            decoded.type = XSCAL_SCALEFORM_VALUE_INT32;
            decoded.value.int32Value = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(encodedValue));
            return true;
        case layout::kUIntType:
            decoded.type = XSCAL_SCALEFORM_VALUE_UINT32;
            decoded.value.uint32Value = static_cast<std::uint32_t>(encodedValue);
            return true;
        case layout::kNumberType:
            decoded.type = XSCAL_SCALEFORM_VALUE_NUMBER;
            std::memcpy(
                &decoded.value.numberValue,
                &encodedValue,
                sizeof(decoded.value.numberValue));
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool DecodePluginString(
    const std::byte* argument,
    XScalScaleformValue& decoded) noexcept {
    std::string_view text;
    if (!TryDecodeScaleformString(argument, text)) {
        return false;
    }
    decoded = {};
    decoded.type = XSCAL_SCALEFORM_VALUE_STRING;
    decoded.stringLength = static_cast<std::uint32_t>(text.size());
    decoded.value.stringValue = text.data();
    return true;
}

// Tries the primary argument candidate first, then the fallback candidate.
[[nodiscard]] bool DecodePluginArgument(
    const std::byte* argument,
    XScalScaleformValue& decoded) noexcept {
    const auto candidates = ReadArgumentCandidates(argument);
    const auto primaryKind =
        static_cast<std::uint8_t>(candidates.primary.type) & layout::kTypeMask;
    if (primaryKind == layout::kStringType) {
        if (DecodePluginString(argument, decoded)) {
            return true;
        }
    } else if (DecodePluginScalar(
                   candidates.primary.type, candidates.primary.payload, decoded)) {
        return true;
    }

    const auto fallbackKind =
        static_cast<std::uint8_t>(candidates.fallback.type) & layout::kTypeMask;
    if (fallbackKind == layout::kStringType) {
        return DecodePluginString(argument, decoded);
    }
    if (DecodePluginScalar(
            candidates.fallback.type, candidates.fallback.payload, decoded)) {
        return true;
    }
    if (primaryKind == 0 && fallbackKind == 0) {
        decoded = {};
        decoded.type = XSCAL_SCALEFORM_VALUE_UNDEFINED;
        return true;
    }
    return false;
}

[[nodiscard]] bool SetPluginResult(
    const XScalScaleformValue& pluginResult,
    ScaleformResult& result) noexcept {
    switch (pluginResult.type) {
        case XSCAL_SCALEFORM_VALUE_BOOLEAN:
            result.SetBoolean(pluginResult.value.booleanValue != 0);
            return true;
        case XSCAL_SCALEFORM_VALUE_INT32:
            result.SetInt32(pluginResult.value.int32Value);
            return true;
        case XSCAL_SCALEFORM_VALUE_UINT32:
            result.SetUInt32(pluginResult.value.uint32Value);
            return true;
        case XSCAL_SCALEFORM_VALUE_NUMBER:
            result.SetNumber(pluginResult.value.numberValue);
            return true;
        case XSCAL_SCALEFORM_VALUE_STRING:
            return result.SetString(std::string_view{
                pluginResult.value.stringValue,
                pluginResult.stringLength});
        default:
            return false;
    }
}

[[nodiscard]] bool DispatchPluginScaleform(
    std::string_view name,
    const ScaleformCall& call,
    PluginScaleformDispatch dispatch) noexcept {
    if (call.arguments == nullptr || call.argumentCount == 0 ||
        call.argumentCount - 1 > XSCAL_SCALEFORM_MAX_ARGUMENTS) {
        return false;
    }
    const auto* const rawArguments =
        static_cast<const std::byte*>(call.arguments);
    const std::uint32_t argumentCount =
        static_cast<std::uint32_t>(call.argumentCount - 1);
    std::array<XScalScaleformValue, XSCAL_SCALEFORM_MAX_ARGUMENTS>
        arguments{};
    for (std::uint32_t index = 0; index < argumentCount; ++index) {
        if (!DecodePluginArgument(
                rawArguments +
                    (static_cast<std::size_t>(index) + 1) *
                        layout::kFunctionArgumentSize,
                arguments[index])) {
            return false;
        }
    }

    XScalScaleformValue result{};
    if (!dispatch(
            name,
            argumentCount != 0 ? arguments.data() : nullptr,
            argumentCount,
            result)) {
        return false;
    }
    return SetPluginResult(result, *call.result);
}

constexpr std::array kScaleformCallbacks{
    ScaleformCallbackDefinition{
        "Game.Clock",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::time,
            kPendingTimeDataPayload>},
    ScaleformCallbackDefinition{"Game.GetAS3Object", &HandleGameObject},
    ScaleformCallbackDefinition{
        "Game.GetCollections",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::collections,
            kPendingPipBoyCollectionsPayload>},
    ScaleformCallbackDefinition{
        "Game.GetInventory",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::inventory,
            kPendingPipBoyInventoryPayload>},
    ScaleformCallbackDefinition{
        "Game.GetPerks",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::perks,
            kPendingPipBoyPerksPayload>},
    ScaleformCallbackDefinition{
        "Game.GetPlayerInfo",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::playerInfo,
            kPendingPipBoyPlayerInfoPayload>},
    ScaleformCallbackDefinition{
        "Game.GetQuests",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::quests,
            kPendingPipBoyQuestsPayload>},
    ScaleformCallbackDefinition{
        "Game.GetSpecial",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::special,
            kPendingPipBoySpecialPayload>},
    ScaleformCallbackDefinition{
        "Game.GetStatus",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::status,
            kPendingPipBoyStatusPayload>},
    ScaleformCallbackDefinition{"GetXSRuntimeInfo", &HandleRuntimeInfo},
    ScaleformCallbackDefinition{"GetZFERuntimeInfo", &HandleRuntimeInfo},
    ScaleformCallbackDefinition{
        "Input.AreGamepadButtonsPressed",
        &HandleGamepadOperation<&InputState::AreGamepadButtonsPressed>},
    ScaleformCallbackDefinition{"Input.BeginInput", &HandleBeginInput},
    ScaleformCallbackDefinition{
        "Input.BeginMouseCapture",
        &HandleBeginMouseCapture},
    ScaleformCallbackDefinition{
        "Input.ClearGamepadButtons",
        &HandleClearGamepadButtons},
    ScaleformCallbackDefinition{
        "Input.ClearGamepadSuppressions",
        &HandleClearGamepadSuppressions},
    ScaleformCallbackDefinition{
        "Input.ClearKeySuppressions",
        &HandleClearKeySuppressions},
    ScaleformCallbackDefinition{"Input.ClearKeys", &HandleClearKeys},
    ScaleformCallbackDefinition{"Input.EndInput", &HandleEndInput},
    ScaleformCallbackDefinition{
        "Input.EndMouseCapture",
        &HandleEndMouseCapture},
    ScaleformCallbackDefinition{
        "Input.IsKeyPressed",
        &HandleKeyOperation<&InputState::IsKeyPressed>},
    ScaleformCallbackDefinition{"Input.PollGamepad", &HandlePollGamepad},
    ScaleformCallbackDefinition{"Input.PollInput", &HandlePollInput},
    ScaleformCallbackDefinition{"Input.PollMouse", &HandlePollMouse},
    ScaleformCallbackDefinition{
        "Input.RegisterGamepadButtons",
        &HandleGamepadOperation<&InputState::RegisterGamepadButtons>},
    ScaleformCallbackDefinition{
        "Input.RegisterKey",
        &HandleKeyOperation<&InputState::RegisterKey>},
    ScaleformCallbackDefinition{
        "Input.SuppressGamepadAnalog",
        &HandleGamepadAnalogOperation<&InputState::SuppressGamepadAnalog>},
    ScaleformCallbackDefinition{
        "Input.SuppressGamepadButtons",
        &HandleGamepadOperation<&InputState::SuppressGamepadButtons>},
    ScaleformCallbackDefinition{
        "Input.SuppressKey",
        &HandleKeyOperation<&InputState::SuppressKey>},
    ScaleformCallbackDefinition{
        "Input.UnregisterGamepadButtons",
        &HandleGamepadOperation<&InputState::UnregisterGamepadButtons>},
    ScaleformCallbackDefinition{
        "Input.UnregisterKey",
        &HandleKeyOperation<&InputState::UnregisterKey>},
    ScaleformCallbackDefinition{
        "Input.UnsuppressGamepadAnalog",
        &HandleGamepadAnalogOperation<&InputState::UnsuppressGamepadAnalog>},
    ScaleformCallbackDefinition{
        "Input.UnsuppressGamepadButtons",
        &HandleGamepadOperation<&InputState::UnsuppressGamepadButtons>},
    ScaleformCallbackDefinition{
        "Input.UnsuppressKey",
        &HandleKeyOperation<&InputState::UnsuppressKey>},
    ScaleformCallbackDefinition{"log", &LogFcmMessage},
    ScaleformCallbackDefinition{
        "writeBlockFile",
        &HandleQueuedFileWrite<FileCommand::WriteBlockFile>},
    ScaleformCallbackDefinition{
        "writeBuffDataFile",
        &HandleQueuedFileWrite<FileCommand::WriteBuffDataFile>},
    ScaleformCallbackDefinition{
        "writeCampDataFile",
        &HandleQueuedFileWrite<FileCommand::WriteCampDataFile>},
    ScaleformCallbackDefinition{
        "writeCampSearchConfigFile",
        &HandleQueuedFileWrite<FileCommand::WriteCampSearchConfigFile>},
    ScaleformCallbackDefinition{
        "writeChallengeDataFile",
        &HandleQueuedFileWrite<FileCommand::WriteChallengeDataFile>},
    ScaleformCallbackDefinition{
        "writeCharacterDataFile",
        &HandleQueuedFileWrite<FileCommand::WriteCharacterDataFile>},
    ScaleformCallbackDefinition{
        "writeDPSMeterFile",
        &HandleQueuedFileWrite<FileCommand::WriteDPSMeterFile>},
    ScaleformCallbackDefinition{
        "writeItemsModFile",
        &HandleQueuedFileWrite<FileCommand::WriteItemsModFile>},
    ScaleformCallbackDefinition{
        "writeLegendaryModsFile",
        &HandleQueuedFileWrite<FileCommand::WriteLegendaryModsFile>},
    ScaleformCallbackDefinition{
        "writePerksFile",
        &HandleQueuedFileWrite<FileCommand::WritePerksFile>},
    ScaleformCallbackDefinition{
        "writeSaveEverythingFile",
        &HandleQueuedFileWrite<FileCommand::WriteSaveEverythingFile>},
    ScaleformCallbackDefinition{
        "writeVendorLogFile",
        &HandleQueuedFileWrite<FileCommand::WriteVendorLogFile>},
    ScaleformCallbackDefinition{"xsGetEnemyMeterData", &HandleEnemyMeter},
    ScaleformCallbackDefinition{"xsGetPipboySTATSEffects", &HandleStatsEffects},
    ScaleformCallbackDefinition{"xsGetPlayerMapPosition", &HandlePlayerMapPosition},
    ScaleformCallbackDefinition{"xsGetStealthMeterData", &HandleStealthMeter},
    ScaleformCallbackDefinition{
        "xsGetTimeData",
        &HandlePipBoyProvider<
            &datamanager::PipBoyProviders::time,
            kPendingTimeDataPayload>},
};

[[nodiscard]] constexpr bool CallbackNamesAreSorted() noexcept {
    for (std::size_t index = 1; index < kScaleformCallbacks.size(); ++index) {
        if (!(kScaleformCallbacks[index - 1].name <
              kScaleformCallbacks[index].name)) {
            return false;
        }
    }
    return true;
}

static_assert(CallbackNamesAreSorted());

[[nodiscard]] const ScaleformCallbackDefinition* FindCallback(
    std::string_view name) noexcept {
    const auto found = std::lower_bound(
        kScaleformCallbacks.begin(),
        kScaleformCallbacks.end(),
        name,
        [](const ScaleformCallbackDefinition& definition,
           std::string_view value) {
            return definition.name < value;
        });
    if (found == kScaleformCallbacks.end() || found->name != name) {
        return nullptr;
    }
    return &*found;
}

}

ScaleformCallbackRouter::ScaleformCallbackRouter(
    FileCommandService& fileCommands,
    InputState& input,
    datamanager::PipBoyStatsEffectsProvider& statsEffects,
    datamanager::PlayerMapPositionProvider& playerMapPosition,
    datamanager::PipBoyProviders& pipBoy,
    PluginScaleformDispatch pluginDispatch) noexcept
    : fileCommands_{fileCommands},
      input_{input},
      statsEffects_{statsEffects},
      playerMapPosition_{playerMapPosition},
      pipBoy_{pipBoy},
      pluginDispatch_{pluginDispatch != nullptr
          ? pluginDispatch
          : &plugins::DispatchScaleform} {}

void ScaleformCallbackRouter::SetTargetKind(TargetKind targetKind) noexcept {
    targetKind_.store(targetKind, std::memory_order_release);
}

bool ScaleformCallbackRouter::Dispatch(
    std::string_view name,
    const ScaleformCall& call) const noexcept {
    if (name.empty() || name.size() > kMaximumNameLength) {
        return false;
    }
    const ScaleformCallbackTargets targets{
        fileCommands_,
        input_,
        statsEffects_,
        playerMapPosition_,
        pipBoy_,
        targetKind_,
    };
    const auto* const callback = FindCallback(name);
    return callback != nullptr
        ? callback->callback(call, targets)
        : DispatchPluginScaleform(name, call, pluginDispatch_);
}

}
